// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/undistort_remap.h"

#include <array>
#include <cmath>

namespace PJ {
namespace {

// Forward radial-tangential distortion (plumb_bob / rational_polynomial), the
// OpenCV / ROS model. Maps a normalized, rectified ray (x, y) to the distorted
// normalized coordinates (xd, yd). D order: [k1, k2, p1, p2, k3, k4, k5, k6];
// missing trailing entries are treated as zero (plumb_bob uses the first five,
// rational_polynomial all eight).
void distortRadTan(double x, double y, const std::vector<double>& d, double& xd, double& yd) {
  const double k1 = d.size() > 0 ? d[0] : 0.0;
  const double k2 = d.size() > 1 ? d[1] : 0.0;
  const double p1 = d.size() > 2 ? d[2] : 0.0;
  const double p2 = d.size() > 3 ? d[3] : 0.0;
  const double k3 = d.size() > 4 ? d[4] : 0.0;
  const double k4 = d.size() > 5 ? d[5] : 0.0;
  const double k5 = d.size() > 6 ? d[6] : 0.0;
  const double k6 = d.size() > 7 ? d[7] : 0.0;

  const double r2 = x * x + y * y;
  const double r4 = r2 * r2;
  const double r6 = r4 * r2;
  const double num = 1.0 + k1 * r2 + k2 * r4 + k3 * r6;
  const double denom = 1.0 + k4 * r2 + k5 * r4 + k6 * r6;
  // The rational_polynomial denominator (k4..k6) can vanish or flip sign for
  // extreme coefficients; that would yield inf/NaN sample coords (silent black
  // patches). Fall back to the pure-radial numerator when it does.
  const double radial = std::abs(denom) > 1e-9 ? num / denom : num;
  xd = x * radial + 2.0 * p1 * x * y + p2 * (r2 + 2.0 * x * x);
  yd = y * radial + p1 * (r2 + 2.0 * y * y) + 2.0 * p2 * x * y;
}

// Forward equidistant (fisheye / Kannala-Brandt) distortion. D order: [k1..k4].
void distortEquidistant(double x, double y, const std::vector<double>& d, double& xd, double& yd) {
  const double k1 = d.size() > 0 ? d[0] : 0.0;
  const double k2 = d.size() > 1 ? d[1] : 0.0;
  const double k3 = d.size() > 2 ? d[2] : 0.0;
  const double k4 = d.size() > 3 ? d[3] : 0.0;

  const double r = std::sqrt(x * x + y * y);
  if (r < 1e-9) {
    xd = x;
    yd = y;
    return;
  }
  const double theta = std::atan(r);
  const double t2 = theta * theta;
  const double t4 = t2 * t2;
  const double t6 = t4 * t2;
  const double t8 = t4 * t4;
  const double theta_d = theta * (1.0 + k1 * t2 + k2 * t4 + k3 * t6 + k4 * t8);
  const double scale = theta_d / r;
  xd = x * scale;
  yd = y * scale;
}

[[nodiscard]] bool isEquidistantModel(const std::string& m) noexcept {
  return m == "equidistant" || m == "fisheye" || m == "kannala_brandt";
}

}  // namespace

bool isRectifiable(const sdk::CameraInfo& ci) noexcept {
  // Need real pinhole intrinsics (fx = K[0], fy = K[4]) and a known native size.
  return ci.K[0] != 0.0 && ci.K[4] != 0.0 && ci.width > 0 && ci.height > 0;
}

UndistortMap computeUndistortMap(const sdk::CameraInfo& ci, int src_w, int src_h, int out_w, int out_h) {
  UndistortMap map;
  if (!isRectifiable(ci) || src_w <= 0 || src_h <= 0 || out_w <= 0 || out_h <= 0) {
    return map;  // invalid -> caller skips rectification.
  }

  // Original intrinsics (defined at ci.width x ci.height).
  const double fx = ci.K[0];
  const double fy = ci.K[4];
  const double cx = ci.K[2];
  const double cy = ci.K[5];

  // Rectified projection: prefer P (3x4, row-major) when fully populated, else
  // fall back to K. P = [fx' 0 cx' Tx; 0 fy' cy' Ty; 0 0 1 0]. Some converters
  // emit a P with focal lengths but a zeroed principal point, which would place
  // the optical center at the image corner — treat that as unusable and use K
  // entirely rather than mixing P's focals with a (0,0) center.
  double fxp = ci.P[0];
  double fyp = ci.P[5];
  double cxp = ci.P[2];
  double cyp = ci.P[6];
  const bool p_usable = fxp != 0.0 && fyp != 0.0 && (cxp != 0.0 || cyp != 0.0);
  if (!p_usable) {
    fxp = fx;
    fyp = fy;
    cxp = cx;
    cyp = cy;
  }

  // Rectification rotation R (3x3, row-major). rectified_ray = R * original_ray,
  // so original_ray = R^T * rectified_ray. Monocular cameras ship R = identity.
  const std::array<double, 9>& r = ci.R;
  const bool has_r = (r[0] != 0.0 || r[4] != 0.0 || r[8] != 0.0);

  const bool equi = isEquidistantModel(ci.distortion_model);

  // The calibration is defined at ci.width x ci.height; the decoded image may be
  // smaller (downsampled). Distorted pixels are computed at full calibration res,
  // then scaled into the source image's pixel space for sampling.
  const double sx = static_cast<double>(src_w) / static_cast<double>(ci.width);
  const double sy = static_cast<double>(src_h) / static_cast<double>(ci.height);

  map.out_width = out_w;
  map.out_height = out_h;
  map.src_width = src_w;
  map.src_height = src_h;
  const auto n = static_cast<size_t>(out_w) * static_cast<size_t>(out_h);
  map.src_x.resize(n);
  map.src_y.resize(n);

  for (int v = 0; v < out_h; ++v) {
    for (int u = 0; u < out_w; ++u) {
      // 1. Normalized rectified ray from the rectified projection.
      double x = (static_cast<double>(u) - cxp) / fxp;
      double y = (static_cast<double>(v) - cyp) / fyp;

      const size_t idx = static_cast<size_t>(v) * static_cast<size_t>(out_w) + static_cast<size_t>(u);

      // 2. Undo rectification rotation: original_ray = R^T * rectified_ray.
      if (has_r) {
        const double xr = r[0] * x + r[3] * y + r[6];
        const double yr = r[1] * x + r[4] * y + r[7];
        const double zr = r[2] * x + r[5] * y + r[8];
        if (std::abs(zr) < 1e-12) {
          // Ray parallel to the rectified image plane: no finite source pixel.
          // Emit an out-of-bounds sentinel so rectifyFrame renders it black.
          map.src_x[idx] = -1.0F;
          map.src_y[idx] = -1.0F;
          continue;
        }
        x = xr / zr;
        y = yr / zr;
      }

      // 3. Forward-distort the normalized ray with the original distortion.
      double xd = x;
      double yd = y;
      if (equi) {
        distortEquidistant(x, y, ci.D, xd, yd);
      } else {
        distortRadTan(x, y, ci.D, xd, yd);
      }

      // 4. Project to full-resolution pixels, then scale into source pixel space.
      const double u_full = fx * xd + cx;
      const double v_full = fy * yd + cy;
      map.src_x[idx] = static_cast<float>(u_full * sx);
      map.src_y[idx] = static_cast<float>(v_full * sy);
    }
  }
  return map;
}

}  // namespace PJ
