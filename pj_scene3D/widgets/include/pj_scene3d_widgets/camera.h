#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <glm/glm.hpp>

namespace pj::scene3d {

class OrbitCamera {
 public:
  OrbitCamera();

  [[nodiscard]] glm::mat4 viewMatrix() const;
  [[nodiscard]] glm::mat4 projMatrix(float aspect) const;
  [[nodiscard]] glm::vec3 position() const;

  void rotate(float dx_pixels, float dy_pixels);
  void pan(float dx_pixels, float dy_pixels);
  void zoom(float scroll_ticks);
  void reset();
  void fitToBoundingBox(const glm::vec3& min_corner, const glm::vec3& max_corner);

 private:
  glm::vec3 focal_{0.0f};
  float radius_{5.0f};
  float azimuth_{0.0f};
  float elevation_{glm::radians(30.0f)};
  float fov_y_{glm::radians(45.0f)};
  float near_{0.05f};
  float far_{1000.0f};
  float pixels_per_radian_{200.0f};
};

}  // namespace pj::scene3d
