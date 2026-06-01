#pragma once

#include <chrono>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>

namespace pj::scene3d {

using TimePoint = std::chrono::nanoseconds;

struct Transform {
  glm::dvec3 t{0.0};
  glm::dquat q{1.0, 0.0, 0.0, 0.0};

  Transform() = default;
  Transform(const glm::dvec3& tr, const glm::dquat& rot) : t(tr), q(rot) {}

  static Transform identity() {
    return {};
  }

  Transform inverse() const {
    const glm::dquat qi = glm::conjugate(q);
    return {-(qi * t), qi};
  }

  Transform operator*(const Transform& rhs) const {
    return {t + q * rhs.t, q * rhs.q};
  }

  glm::dvec3 operator*(const glm::dvec3& p) const {
    return t + q * p;
  }

  glm::dmat4 matrix() const {
    glm::dmat4 m = glm::mat4_cast(q);
    m[3] = glm::dvec4(t, 1.0);
    return m;
  }
};

struct StampedTransform {
  TimePoint stamp;
  std::string parent_frame;
  std::string child_frame;
  Transform transform;
};

}  // namespace pj::scene3d
