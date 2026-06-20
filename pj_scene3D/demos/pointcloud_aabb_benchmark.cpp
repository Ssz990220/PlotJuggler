// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Self-verifying offscreen-GL benchmark for the point-cloud AABB reduction. It
// generates synthetic clouds, runs BOTH the production CPU scan
// (scanBoundsAndScalarRange) and the GPU compute reduction over the same VBO,
// asserts they agree (so it is a correctness check too), and reports timings:
//
//   CPU scan       — full bounds scan, today entirely on the GUI thread.
//   GPU submit     — glDispatchCompute + fence (the ONLY GUI-thread cost in the
//                    async design; the reduction itself runs on the GPU).
//   GPU end-to-end — submit + glFinish + readback (the total latency until the
//                    result is available a frame or two later).
//
// Run headless with QT_QPA_PLATFORM=offscreen, or on a real display to get
// representative GPU hardware numbers. It prints GL_RENDERER so software vs
// hardware GL is never ambiguous. No app, no user interaction.
//
// Usage: pointcloud_aabb_benchmark [N1 N2 ...]   (point counts; default sweep)

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/span.hpp"
#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/pointcloud_convert.h"
#include "pj_scene3d_widgets/passes/pointcloud_aabb_reducer.h"

namespace {

using pj::scene3d::AABB;
using pj::scene3d::AttribLayout;
using pj::scene3d::BoundsScanResult;
using pj::scene3d::checkFastPath;
using pj::scene3d::PointcloudAabbReducer;
using pj::scene3d::scanBoundsAndScalarRange;
using PJ::sdk::PointCloud;
using PJ::sdk::PointField;
using DT = PointField::Datatype;

constexpr uint32_t kStride = 16;  // xyz float32 + intensity float32

using Clock = std::chrono::steady_clock;

double medianMs(std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  return samples.empty() ? 0.0 : samples[samples.size() / 2];
}

struct SyntheticCloud {
  std::vector<uint8_t> bytes;
  PointCloud cloud;

  explicit SyntheticCloud(std::size_t n) : bytes(n * kStride, 0) {
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> dist(-500.0f, 500.0f);
    for (std::size_t i = 0; i < n; ++i) {
      const float xyz[3] = {dist(rng), dist(rng), dist(rng)};
      std::memcpy(bytes.data() + i * kStride, xyz, sizeof(xyz));
    }
    cloud.width = static_cast<uint32_t>(n);
    cloud.height = 1;
    cloud.point_step = kStride;
    cloud.row_step = static_cast<uint32_t>(n) * kStride;
    cloud.is_bigendian = false;
    cloud.frame_id = "lidar";
    cloud.fields = {
        {"x", 0, DT::kFloat32, 1},
        {"y", 4, DT::kFloat32, 1},
        {"z", 8, DT::kFloat32, 1},
        {"intensity", 12, DT::kFloat32, 1}};
    cloud.data = PJ::Span<const uint8_t>(bytes.data(), bytes.size());
  }
};

bool approxEqual(const AABB& a, const AABB& b) {
  const float eps = 1e-3f;
  return a.valid == b.valid && std::abs(a.min.x - b.min.x) < eps && std::abs(a.min.y - b.min.y) < eps &&
         std::abs(a.min.z - b.min.z) < eps && std::abs(a.max.x - b.max.x) < eps && std::abs(a.max.y - b.max.y) < eps &&
         std::abs(a.max.z - b.max.z) < eps;
}

GLuint uploadBuffer(QOpenGLFunctions* f, const std::vector<uint8_t>& bytes) {
  GLuint id = 0;
  f->glGenBuffers(1, &id);
  f->glBindBuffer(GL_ARRAY_BUFFER, id);
  f->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes.size()), bytes.data(), GL_STATIC_DRAW);
  f->glBindBuffer(GL_ARRAY_BUFFER, 0);
  return id;
}

int runBenchmark(const std::vector<std::size_t>& sizes) {
  QSurfaceFormat format;
  format.setVersion(4, 5);
  format.setProfile(QSurfaceFormat::CoreProfile);

  QOffscreenSurface surface;
  surface.setFormat(format);
  surface.create();
  QOpenGLContext context;
  context.setFormat(format);
  if (!surface.isValid() || !context.create() || !context.makeCurrent(&surface)) {
    std::fprintf(stderr, "Failed to create an offscreen GL 4.5 context\n");
    return 1;
  }
  QOpenGLFunctions* f = context.functions();
  const auto* renderer = reinterpret_cast<const char*>(f->glGetString(GL_RENDERER));
  const auto* version = reinterpret_cast<const char*>(f->glGetString(GL_VERSION));
  std::printf("GL_RENDERER : %s\n", renderer != nullptr ? renderer : "(null)");
  std::printf("GL_VERSION  : %s\n\n", version != nullptr ? version : "(null)");

  std::printf(
      "%12s | %11s | %11s | %13s | %8s | %s\n", "points", "CPU scan", "GPU submit", "GPU e2e", "speedup", "match");
  std::printf("%s\n", std::string(78, '-').c_str());

  constexpr int kIters = 30;
  int exit_code = 0;
  for (const std::size_t n : sizes) {
    SyntheticCloud cloud(n);
    const AttribLayout layout = *checkFastPath(cloud.cloud, "");
    const GLuint buffer = uploadBuffer(f, cloud.bytes);

    // CPU scan (bounds-only, sf == nullptr) — the path the GPU replaces.
    BoundsScanResult cpu_scan{};
    std::vector<double> cpu_ms;
    for (int i = 0; i < kIters; ++i) {
      const auto t0 = Clock::now();
      cpu_scan = scanBoundsAndScalarRange(cloud.cloud, layout, nullptr);
      const auto t1 = Clock::now();
      cpu_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }

    PointcloudAabbReducer reducer;
    // Warm up (compile the program, first dispatch).
    reducer.dispatch(buffer, n, kStride, 0);
    f->glFinish();
    AABB gpu_box = reducer.poll().value_or(AABB{});

    std::vector<double> submit_ms;
    std::vector<double> e2e_ms;
    for (int i = 0; i < kIters; ++i) {
      const auto t0 = Clock::now();
      reducer.dispatch(buffer, n, kStride, 0);
      const auto t1 = Clock::now();  // GUI-thread cost: command submission only
      f->glFinish();
      gpu_box = reducer.poll().value_or(gpu_box);
      const auto t2 = Clock::now();  // total latency incl. GPU work + readback
      submit_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
      e2e_ms.push_back(std::chrono::duration<double, std::milli>(t2 - t0).count());
    }

    const AABB cpu_box = cpu_scan.bounds;
    const bool match = reducer.available() && approxEqual(cpu_box, gpu_box);
    if (!match) {
      exit_code = 2;  // correctness failure -> non-zero exit
    }
    const double cpu = medianMs(cpu_ms);
    const double submit = medianMs(submit_ms);
    const double e2e = medianMs(e2e_ms);
    std::printf(
        "%12zu | %8.3f ms | %8.3f ms | %10.3f ms | %6.1fx | %s\n", n, cpu, submit, e2e,
        submit > 0.0 ? cpu / submit : 0.0, match ? "OK" : (reducer.available() ? "MISMATCH" : "no-compute"));

    f->glDeleteBuffers(1, &buffer);
  }
  std::printf(
      "\nGPU submit = the only GUI-thread cost in the async design (the reduction runs on the GPU).\n"
      "GPU e2e includes glFinish + readback; production polls the result a frame later, off the hot path.\n");
  return exit_code;
}

}  // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  std::vector<std::size_t> sizes;
  for (int i = 1; i < argc; ++i) {
    sizes.push_back(static_cast<std::size_t>(std::stoll(argv[i])));
  }
  if (sizes.empty()) {
    sizes = {100000, 500000, 1000000, 5000000};
  }
  return runBenchmark(sizes);
}
