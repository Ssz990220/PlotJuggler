// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <string>

namespace pj::scene3d {

// Axis-aligned bounding box in a single coordinate frame. `valid == false` means
// "no bounds reported yet" and acts as the identity element for unionAABB(): an
// invalid box contributes nothing to a union. Lives in pj_scene3d_core (Qt/GL-free)
// so scene geometry and the camera can share it and the math stays headless-testable.
struct AABB {
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
  bool valid{false};
};

// Grow `box` to include point `p`. Initializes `box` to the degenerate box at `p`
// when it was invalid. Callers should pre-filter non-finite points.
inline void expandAABB(AABB& box, const glm::vec3& p) {
  if (!box.valid) {
    box.min = p;
    box.max = p;
    box.valid = true;
    return;
  }
  box.min = glm::min(box.min, p);
  box.max = glm::max(box.max, p);
}

// Union of two boxes. An invalid box is the identity, so unioning over a mix of
// reporting / non-reporting entities "just works".
inline AABB unionAABB(const AABB& a, const AABB& b) {
  if (!a.valid) {
    return b;
  }
  if (!b.valid) {
    return a;
  }
  AABB out;
  out.min = glm::min(a.min, b.min);
  out.max = glm::max(a.max, b.max);
  out.valid = true;
  return out;
}

// Center of a box. Only meaningful when the box is valid.
[[nodiscard]] inline glm::vec3 aabbCenter(const AABB& box) {
  return 0.5f * (box.min + box.max);
}

// Length of the box's space diagonal (0 for a degenerate / point box).
[[nodiscard]] inline float aabbDiagonal(const AABB& box) {
  return glm::length(box.max - box.min);
}

// AABB of an occupancy grid given its origin (the grid's min corner, in the
// grid's own source frame), cell resolution (metres/cell), and dimensions in
// cells. The grid is planar — the z extent is zero at origin.z (origin rotation
// is intentionally ignored; scene bounds drive framing / adaptive near-far, not
// precise culling). Returns an invalid box for an empty or degenerate grid.
inline AABB occupancyGridBounds(const glm::vec3& origin, double resolution, std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0 || resolution <= 0.0) {
    return {};
  }
  const auto res = static_cast<float>(resolution);
  AABB box;
  box.min = origin;
  box.max = origin + glm::vec3{res * static_cast<float>(width), res * static_cast<float>(height), 0.0f};
  box.valid = true;
  return box;
}

// Serializable camera pose, shared by every controller. A controller exports its
// pose with state() and adopts another's with adoptState(), so switching camera
// models preserves where you were looking. `perspective == false` selects an
// orthographic projection driven by `ortho_scale` (half-height in world units)
// instead of `radius`/`fov_y`.
struct CameraState {
  glm::vec3 focal{0.0f};                 ///< Orbit pivot / look-at target (world).
  float radius{5.0f};                    ///< Eye distance from focal (perspective).
  float azimuth{0.0f};                   ///< Yaw about +Z (radians).
  float elevation{glm::radians(30.0f)};  ///< Pitch above the XY plane (radians).
  float fov_y{glm::radians(45.0f)};      ///< Vertical field of view (perspective).
  float ortho_scale{5.0f};               ///< Half-height of the ortho frustum (world units).
  bool perspective{true};                ///< Projection kind.
};

// JSON (de)serialization of CameraState, for layout persistence. Round-trips all
// fields. cameraStateFromJson is tolerant: unparseable, non-object, or wrong-typed
// input returns `fallback` unchanged, and any individually-missing field is taken
// from `fallback` (so an older / partial layout still loads). Surviving values are
// sanitized (finite, in-range) so a corrupt layout can never drive a degenerate view.
[[nodiscard]] std::string cameraStateToJson(const CameraState& state);
[[nodiscard]] CameraState cameraStateFromJson(const std::string& json, const CameraState& fallback = CameraState{});

// Abstract camera controller. Concrete models (Orbit, XYOrbit, TopDownOrtho, Fly)
// differ in how mouse/scroll input maps to CameraState and which projection they
// emit, but all expose the same view/projection/position the renderer consumes.
// Lives in pj_scene3d_core so the math is headless-testable (no Qt / GL).
class ICamera {
 public:
  virtual ~ICamera() = default;

  [[nodiscard]] virtual glm::mat4 viewMatrix() const = 0;
  [[nodiscard]] virtual glm::mat4 projMatrix(float aspect) const = 0;
  [[nodiscard]] virtual glm::vec3 position() const = 0;

  // Latest scene extent (union of entity worldBounds()), used for adaptive
  // near/far and framing. An invalid AABB means "unknown" → working-distance
  // fallback.
  virtual void setSceneBounds(const AABB& bounds) = 0;

  virtual void rotate(float dx_pixels, float dy_pixels) = 0;
  virtual void pan(float dx_pixels, float dy_pixels) = 0;
  virtual void zoom(float scroll_ticks) = 0;
  // Zoom keeping the world point under the cursor fixed. cursor_px is in Qt
  // widget pixels (origin top-left). Falls back to center-of-view zoom() when no
  // ground/focal-plane hit exists.
  virtual void zoomToCursor(float scroll_ticks, glm::vec2 cursor_px, int viewport_w, int viewport_h) = 0;

  virtual void reset() = 0;  ///< Restore this model's default view.
  virtual void fitToBoundingBox(const AABB& bounds) = 0;

  [[nodiscard]] virtual CameraState state() const = 0;
  virtual void adoptState(const CameraState& state) = 0;
};

// Spherical orbit camera: eye orbits `focal` at `radius`, parameterized by
// azimuth/elevation. Z-up. Emits an adaptive near/far perspective projection and
// supports cursor-anchored zoom. Non-final — XYOrbitCamera derives from it.
class OrbitCamera : public ICamera {
 public:
  [[nodiscard]] glm::mat4 viewMatrix() const override;
  [[nodiscard]] glm::mat4 projMatrix(float aspect) const override;
  [[nodiscard]] glm::vec3 position() const override;

  void setSceneBounds(const AABB& bounds) override;

  void rotate(float dx_pixels, float dy_pixels) override;
  void pan(float dx_pixels, float dy_pixels) override;
  void zoom(float scroll_ticks) override;
  void zoomToCursor(float scroll_ticks, glm::vec2 cursor_px, int viewport_w, int viewport_h) override;

  void reset() override;
  void fitToBoundingBox(const AABB& bounds) override;

  [[nodiscard]] CameraState state() const override {
    return state_;
  }
  void adoptState(const CameraState& state) override;

 protected:
  // Set spherical state so position() == eye and the camera looks at `focal`
  // (re-derives radius/azimuth/elevation; clamps radius/elevation but keeps the
  // eye fixed). Shared by zoomToCursor and the XYOrbit ground re-lock.
  void setEyeFocal(const glm::vec3& eye, const glm::vec3& focal);

  CameraState state_{};
  AABB scene_bounds_{};  // last reported scene extent; consumed by adaptive near/far
};

// Orbit camera with the pivot locked to the ground plane (z=0): panning slides the
// focal in the floor plane and zoom keeps focal.z == 0, so the orbit centre never
// floats off the floor. Perspective. Derives from OrbitCamera.
class XYOrbitCamera final : public OrbitCamera {
 public:
  void pan(float dx_pixels, float dy_pixels) override;
  void zoomToCursor(float scroll_ticks, glm::vec2 cursor_px, int viewport_w, int viewport_h) override;
};

// Orthographic bird's-eye camera looking straight down -Z, rotatable about the
// vertical (map heading). The robotics "2D mode" for costmaps / occupancy grids /
// trajectories. `ortho_scale` is the half-height of the view in world units; the
// eye floats above the focal and near/far are derived from that height plus the
// scene's vertical extent (the flat-scene guard, so a zoomed costmap never clips).
class TopDownOrthoCamera final : public ICamera {
 public:
  TopDownOrthoCamera();

  [[nodiscard]] glm::mat4 viewMatrix() const override;
  [[nodiscard]] glm::mat4 projMatrix(float aspect) const override;
  [[nodiscard]] glm::vec3 position() const override;

  void setSceneBounds(const AABB& bounds) override;

  void rotate(float dx_pixels, float dy_pixels) override;  // azimuth (map heading) only
  void pan(float dx_pixels, float dy_pixels) override;
  void zoom(float scroll_ticks) override;
  void zoomToCursor(float scroll_ticks, glm::vec2 cursor_px, int viewport_w, int viewport_h) override;

  void reset() override;
  void fitToBoundingBox(const AABB& bounds) override;

  [[nodiscard]] CameraState state() const override {
    return state_;
  }
  void adoptState(const CameraState& state) override;

 private:
  CameraState state_{};
  AABB scene_bounds_{};
};

// First-person / fly camera: free eye + yaw/pitch, no orbit pivot. LEFT looks
// around, pan strafes, wheel dollies along forward (zoom-to-cursor degenerates to
// a forward dolly, by design). Perspective, Z-up.
class FlyCamera final : public ICamera {
 public:
  FlyCamera();

  [[nodiscard]] glm::mat4 viewMatrix() const override;
  [[nodiscard]] glm::mat4 projMatrix(float aspect) const override;
  [[nodiscard]] glm::vec3 position() const override;

  void setSceneBounds(const AABB& bounds) override;

  void rotate(float dx_pixels, float dy_pixels) override;
  void pan(float dx_pixels, float dy_pixels) override;
  void zoom(float scroll_ticks) override;
  void zoomToCursor(float scroll_ticks, glm::vec2 cursor_px, int viewport_w, int viewport_h) override;

  void reset() override;
  void fitToBoundingBox(const AABB& bounds) override;

  [[nodiscard]] CameraState state() const override;
  void adoptState(const CameraState& state) override;

 private:
  void resetToDefault();                        // eye (5,5,10) looking at the origin
  [[nodiscard]] glm::vec3 forward() const;      // unit view direction from yaw/pitch
  [[nodiscard]] float workingDistance() const;  // synthetic focal distance for near/far

  glm::vec3 eye_{5.0f, 5.0f, 10.0f};
  float yaw_{0.0f};
  float pitch_{0.0f};
  AABB scene_bounds_{};
};

}  // namespace pj::scene3d
