#include "pj_scene3d_widgets/camera.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <numbers>

namespace pj::scene3d {

OrbitCamera::OrbitCamera() = default;

glm::mat4 OrbitCamera::viewMatrix() const {
  return glm::lookAt(position(), focal_, glm::vec3{0.0f, 0.0f, 1.0f});
}

glm::mat4 OrbitCamera::projMatrix(float aspect) const {
  return glm::perspective(fov_y_, aspect, near_, far_);
}

glm::vec3 OrbitCamera::position() const {
  return focal_ + radius_ * glm::vec3{
                                std::cos(elevation_) * std::cos(azimuth_),
                                std::cos(elevation_) * std::sin(azimuth_),
                                std::sin(elevation_),
                            };
}

void OrbitCamera::rotate(float dx_pixels, float dy_pixels) {
  constexpr float half_pi = std::numbers::pi_v<float> * 0.5f;
  azimuth_ -= dx_pixels / pixels_per_radian_;
  elevation_ = std::clamp(elevation_ + dy_pixels / pixels_per_radian_, -half_pi + 0.05f, half_pi - 0.05f);
}

void OrbitCamera::pan(float dx_pixels, float dy_pixels) {
  const glm::mat4 view = viewMatrix();
  const glm::vec3 right{view[0][0], view[1][0], view[2][0]};
  const glm::vec3 up{view[0][1], view[1][1], view[2][1]};
  focal_ += (-dx_pixels * right + dy_pixels * up) * radius_ * 0.001f;
}

void OrbitCamera::zoom(float scroll_ticks) {
  // Two-stage zoom:
  //   * Above kMinRadius, shrink the orbit radius (camera moves toward
  //     the focal point — classic orbit zoom).
  //   * At or near kMinRadius, dolly *both* the focal and the camera
  //     forward along the view direction. The orbit pivot moves with
  //     the camera so progress never asymptotes and the user can fly
  //     deeper into the scene than the original focal point would
  //     allow. This matches RViz / Foxglove / Blender behaviour.
  // Step is 15% of current radius (slightly stronger than before) with
  // a 5 cm absolute floor so close-up motion still moves perceptibly.
  constexpr float kMinRadius = 0.1f;
  const float step = std::max(radius_ * 0.15f, 0.05f);
  float dr = step * scroll_ticks;  // positive ticks = zoom in = shrink

  if (dr > 0.0f && radius_ - dr < kMinRadius) {
    // Saturated: spend the overflow as a dolly forward.
    const float overflow = dr - (radius_ - kMinRadius);
    const glm::vec3 fwd = glm::normalize(focal_ - position());
    focal_ += fwd * overflow;
    radius_ = kMinRadius;
    return;
  }
  radius_ = std::clamp(radius_ - dr, kMinRadius, 1000.0f);
}

void OrbitCamera::reset() {
  *this = OrbitCamera{};
}

void OrbitCamera::fitToBoundingBox(const glm::vec3& min_corner, const glm::vec3& max_corner) {
  focal_ = (min_corner + max_corner) * 0.5f;
  radius_ = std::max(glm::length(max_corner - min_corner) * 0.75f, 0.5f);
}

}  // namespace pj::scene3d
