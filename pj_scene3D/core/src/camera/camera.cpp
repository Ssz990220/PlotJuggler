// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/camera/camera.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#include <numbers>

#include "pj_scene3d_core/camera/camera_math.h"

namespace pj::scene3d {

namespace {
// Drag sensitivity: screen pixels per radian of orbit rotation.
constexpr float kPixelsPerRadian = 200.0f;
// World units of focal/eye translation per pixel of drag, per unit of working distance.
// At distance D a single pixel drag shifts the pivot by D * kPanUnitsPerPixel world units.
constexpr float kPanUnitsPerPixel = 0.001f;
// Elevation is clamped just short of the poles to avoid the gimbal singularity
// (matches the clamp historically used by rotate()).
constexpr float kPolarLimit = std::numbers::pi_v<float> * 0.5f - 0.05f;

// Shared pan-offset kernel: translates `distance * kPanUnitsPerPixel` world units
// per pixel along the view's right/up axes. `right` and `up` come from the view
// matrix rows (OrbitCamera) or forward-cross products (FlyCamera).
glm::vec3 panOffset(const glm::vec3& right, const glm::vec3& up, float dx_pixels, float dy_pixels, float distance) {
  return (-dx_pixels * right + dy_pixels * up) * distance * kPanUnitsPerPixel;
}

// Wrap an angle to (-pi, pi]. std::remainder(x, 2pi) gives the value in [-pi, pi]
// nearest to 0, which is exactly the canonical azimuth range we want.
float wrapToPi(float angle) {
  return std::remainder(angle, 2.0f * std::numbers::pi_v<float>);
}

// Unit direction from focal toward eye for a given azimuth/elevation (Z-up).
glm::vec3 sphericalDir(float azimuth, float elevation) {
  return glm::vec3{
      std::cos(elevation) * std::cos(azimuth), std::cos(elevation) * std::sin(azimuth), std::sin(elevation)};
}

// Azimuth (yaw about +Z) and pole-clamped elevation (pitch) of a unit direction —
// the inverse of sphericalDir, sharing its gimbal clamp. The inner asin clamp
// guards the domain; the outer keeps elevation off the pole singularity.
struct AzimuthElevation {
  float azimuth;
  float elevation;
};
AzimuthElevation azimuthElevationFromDir(const glm::vec3& dir) {
  return {std::atan2(dir.y, dir.x), std::clamp(std::asin(std::clamp(dir.z, -1.0f, 1.0f)), -kPolarLimit, kPolarLimit)};
}

// Min/max orbit radius for the current scene: `lo` keeps the eye outside the near
// plane; `hi` is scene-aware so a large costmap is reachable (or 1e7 when the
// scene extent is unknown). `working_radius` is the radius the near plane is
// derived from.
//
// `lo` is derived from the WORKING-DISTANCE-ONLY near term — NOT the ratio-capped
// near adaptiveNearFar() returns. The ratio cap can lift near to far/1e5, which
// for a scene with one distant finite outlier (reach ~1e6) would inflate lo to
// tens of metres and hard-lock zoom-in (L.7). The eye only needs to clear the
// close-inspection near plane, so lo tracks the working radius alone.
void radiusLimits(const AABB& bounds, const glm::vec3& focal, float working_radius, float& lo, float& hi) {
  const float reach = sceneReach(bounds, focal);
  const float working_near = std::max(working_radius * 1e-2f, 1e-3f);  // near from working distance only
  lo = std::max(2.0f * working_near, 1e-3f);
  hi = bounds.valid ? std::max(1e7f, reach * 5.0f) : 1e7f;
}

// Repair an externally-sourced CameraState (a deserialized layout, or a state
// adopted across a model switch) so it can never drive a degenerate projection:
// non-finite fields fall back to the default, then ranges are clamped (radius/
// ortho_scale > 0, fov_y off 0/π, elevation off the poles). One guard for every
// untrusted entry point — cameraStateFromJson and each adoptState().
CameraState sanitizeCameraState(CameraState state) {
  const CameraState defaults;
  const auto finite_or = [](float value, float fallback) { return std::isfinite(value) ? value : fallback; };
  state.focal = glm::vec3{
      finite_or(state.focal.x, defaults.focal.x), finite_or(state.focal.y, defaults.focal.y),
      finite_or(state.focal.z, defaults.focal.z)};
  state.azimuth = finite_or(state.azimuth, defaults.azimuth);
  state.elevation = std::clamp(finite_or(state.elevation, defaults.elevation), -kPolarLimit, kPolarLimit);
  state.radius = std::max(finite_or(state.radius, defaults.radius), 1e-3f);
  state.ortho_scale = std::max(finite_or(state.ortho_scale, defaults.ortho_scale), 1e-3f);
  state.fov_y = std::clamp(finite_or(state.fov_y, defaults.fov_y), glm::radians(1.0f), glm::radians(179.0f));
  return state;
}
}  // namespace

glm::mat4 OrbitCamera::viewMatrix() const {
  return glm::lookAt(position(), state_.focal, glm::vec3{0.0f, 0.0f, 1.0f});
}

glm::mat4 OrbitCamera::projMatrix(float aspect) const {
  // Decoupled adaptive near/far: near tracks the working distance (so close
  // inspection never clips), far reaches the whole scene, ratio-capped for depth
  // precision. OrbitCamera is always perspective, so the working distance is the
  // orbit radius.
  const float d = state_.radius;
  float near_plane = 0.0f;
  float far_plane = 0.0f;
  adaptiveNearFar(d, sceneReach(scene_bounds_, state_.focal), near_plane, far_plane);
  return glm::perspective(state_.fov_y, aspect, near_plane, far_plane);
}

glm::vec3 OrbitCamera::position() const {
  return state_.focal + state_.radius * sphericalDir(state_.azimuth, state_.elevation);
}

void OrbitCamera::setSceneBounds(const AABB& bounds) {
  scene_bounds_ = bounds;
}

void OrbitCamera::rotate(float dx_pixels, float dy_pixels) {
  state_.azimuth -= dx_pixels / kPixelsPerRadian;
  state_.elevation = std::clamp(state_.elevation + dy_pixels / kPixelsPerRadian, -kPolarLimit, kPolarLimit);
}

void OrbitCamera::pan(float dx_pixels, float dy_pixels) {
  const glm::mat4 view = viewMatrix();
  const glm::vec3 right{view[0][0], view[1][0], view[2][0]};
  const glm::vec3 up{view[0][1], view[1][1], view[2][1]};
  state_.focal += panOffset(right, up, dx_pixels, dy_pixels, state_.radius);
}

void OrbitCamera::zoom(float scroll_ticks) {
  // Geometric step: each tick scales the orbit radius by 0.9 (zoom in) or 1/0.9
  // (zoom out), so the perceived zoom rate is uniform at every scale. The old
  // fixed [0.1, 1000] cap (which made large scenes impossible to frame) and the
  // two-stage dolly are gone; the limits are scene-aware.
  float lo = 0.0f;
  float hi = 0.0f;
  radiusLimits(scene_bounds_, state_.focal, state_.radius, lo, hi);
  const float factor = std::pow(0.9f, scroll_ticks);  // +ticks = zoom in = shrink
  state_.radius = std::clamp(state_.radius * factor, lo, hi);
}

void OrbitCamera::zoomToCursor(float scroll_ticks, glm::vec2 cursor_px, int viewport_w, int viewport_h) {
  if (viewport_w <= 0 || viewport_h <= 0) {
    zoom(scroll_ticks);
    return;
  }
  const float aspect = static_cast<float>(viewport_w) / static_cast<float>(viewport_h);
  const glm::mat4 view = viewMatrix();
  const glm::mat4 proj = projMatrix(aspect);
  const Ray ray = unprojectRay(cursor_px, viewport_w, viewport_h, view, proj);
  const glm::vec3 eye = position();

  // World point under the cursor: ground plane z=0 first (the robotics common
  // case), else the focal plane facing the eye. No forward hit → center zoom.
  //
  // The ground hit is REJECTED when it lands beyond a sanity bound (M.6): a
  // grazing ray at low elevation, or a scene that sits far from z=0, makes the
  // z=0 intersection hundreds of metres out — unrelated to what is under the
  // cursor. Accepting it would homothety-fling the camera. Past the bound we fall
  // through to the focal-plane anchor, which is always near the viewed geometry.
  const float max_hit = std::max(sceneReach(scene_bounds_, state_.focal), 4.0f * state_.radius);
  glm::vec3 target{0.0f};
  float t = 0.0f;
  bool hit = false;
  if (rayPlane(ray.origin, ray.dir, glm::vec3{0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, t) && t > 0.0f && t <= max_hit) {
    target = ray.origin + t * ray.dir;
    hit = true;
  }
  if (!hit) {
    const glm::vec3 n = glm::normalize(state_.focal - eye);
    if (rayPlane(ray.origin, ray.dir, state_.focal, n, t) && t > 0.0f) {
      target = ray.origin + t * ray.dir;
      hit = true;
    }
  }
  if (!hit) {
    zoom(scroll_ticks);
    return;
  }

  // Homothety about the cursor point: scale eye AND focal toward `target` by the
  // same factor. Orientation is preserved and (target - eye) keeps its direction,
  // so the hovered point stays pixel-locked by construction; radius scales like a
  // zoom and the pivot drifts toward what you're zooming into.
  const float s = std::pow(0.9f, scroll_ticks);
  const glm::vec3 new_eye = glm::mix(target, eye, s);
  const glm::vec3 new_focal = glm::mix(target, state_.focal, s);
  if (glm::length(new_eye - new_focal) < 1e-6f) {  // eye collapsed onto focal — bail rather than NaN
    zoom(scroll_ticks);
    return;
  }
  setEyeFocal(new_eye, new_focal);
}

void OrbitCamera::setEyeFocal(const glm::vec3& eye, const glm::vec3& focal) {
  const glm::vec3 d = eye - focal;
  const float r_raw = glm::length(d);
  if (r_raw < 1e-6f) {
    state_.focal = focal;
    return;
  }
  const glm::vec3 dir = d / r_raw;
  float lo = 0.0f;
  float hi = 0.0f;
  radiusLimits(scene_bounds_, focal, r_raw, lo, hi);
  const float r = std::clamp(r_raw, lo, hi);
  const AzimuthElevation orientation = azimuthElevationFromDir(dir);
  state_.azimuth = orientation.azimuth;
  state_.elevation = orientation.elevation;
  state_.radius = r;
  // Re-anchor the focal so position() lands exactly on `eye` even after radius/
  // elevation clamping — this is what keeps the cursor point pixel-locked.
  state_.focal = eye - r * sphericalDir(orientation.azimuth, orientation.elevation);
}

void OrbitCamera::adoptState(const CameraState& state) {
  const CameraState sanitized = sanitizeCameraState(state);
  state_ = sanitized;
  if (!sanitized.perspective) {
    // Coming from an orthographic model: seed the orbit radius from its scale so
    // the perspective view frames roughly the same area.
    state_.radius = sanitized.ortho_scale;
  }
  state_.perspective = true;
}

void OrbitCamera::reset() {
  // Restore the default view in place. Must NOT do `*this = OrbitCamera{}` — that
  // is illegal through an ICamera* and would slice. Scene bounds are scene-derived,
  // not view state, so they are retained.
  state_ = CameraState{};
}

void OrbitCamera::fitToBoundingBox(const AABB& bounds) {
  if (!bounds.valid) {
    return;
  }
  state_.focal = aabbCenter(bounds);
  state_.radius = std::max(aabbDiagonal(bounds) * 0.75f, 0.5f);
}

// ---------------------------------------------------------------------------
// XYOrbitCamera — orbit with the pivot locked to the ground plane (z = 0).
// ---------------------------------------------------------------------------

void XYOrbitCamera::pan(float dx_pixels, float dy_pixels) {
  OrbitCamera::pan(dx_pixels, dy_pixels);
  state_.focal.z = 0.0f;  // keep the pivot on the floor
}

void XYOrbitCamera::zoomToCursor(float scroll_ticks, glm::vec2 cursor_px, int viewport_w, int viewport_h) {
  OrbitCamera::zoomToCursor(scroll_ticks, cursor_px, viewport_w, viewport_h);
  if (std::abs(state_.focal.z) > 1e-6f) {
    // Flatten any residual pivot height without moving the eye (the floor target
    // is already on z=0, so this is usually a no-op; it guards against clamping
    // having nudged the focal off the ground).
    glm::vec3 focal = state_.focal;
    focal.z = 0.0f;
    setEyeFocal(position(), focal);
  }
}

void XYOrbitCamera::adoptState(const CameraState& state) {
  OrbitCamera::adoptState(state);
  if (std::abs(state_.focal.z) > 1e-6f) {
    // Re-lock the adopted pivot to the floor, eye unchanged (same recipe as
    // zoomToCursor's flatten): capture the eye, flatten the focal to z=0, and
    // re-derive the spherical state so position() still lands on the eye.
    const glm::vec3 eye = position();
    glm::vec3 floor_focal = state_.focal;
    floor_focal.z = 0.0f;
    setEyeFocal(eye, floor_focal);
  }
}

// ---------------------------------------------------------------------------
// TopDownOrthoCamera — orthographic bird's-eye looking straight down -Z.
// ---------------------------------------------------------------------------

namespace {
constexpr float kDefaultOrthoScale = 10.0f;
constexpr float kFlyNominalDistance = 5.0f;
// Base eye height = this multiple of ortho_scale above the focal (before the
// scene-aware lift in eyeHeight()). projMatrix()'s near is proportional to this base.
constexpr float kOrthoEyeHeightFactor = 2.0f;

glm::vec3 mapUp(float azimuth) {
  return glm::vec3{-std::sin(azimuth), std::cos(azimuth), 0.0f};
}
glm::vec3 mapRight(float azimuth) {
  return glm::vec3{std::cos(azimuth), std::sin(azimuth), 0.0f};
}
}  // namespace

TopDownOrthoCamera::TopDownOrthoCamera() {
  reset();
}

glm::mat4 TopDownOrthoCamera::viewMatrix() const {
  return glm::lookAt(position(), state_.focal, mapUp(state_.azimuth));
}

float TopDownOrthoCamera::eyeHeight() const {
  const float base = kOrthoEyeHeightFactor * state_.ortho_scale;  // zoom-driven height
  // Scene-aware lift: raise the eye so geometry above the focal plane (e.g. a
  // 1.5 m robot on a costmap zoomed to ortho_scale=0.5) stays in front of the
  // near plane (M.7/M.8). Zero for an unknown scene or geometry below the focal.
  const float lift = scene_bounds_.valid ? std::max(0.0f, scene_bounds_.max.z - state_.focal.z) : 0.0f;
  return base + lift;
}

glm::mat4 TopDownOrthoCamera::projMatrix(float aspect) const {
  const float base = kOrthoEyeHeightFactor * state_.ortho_scale;
  const float eye_height = eyeHeight();
  // near is proportional to the zoom (base), NOT the lifted height: the lift
  // already guarantees geometry above the focal sits in front of the eye, so the
  // near plane can stay tight to the zoom without re-clipping it (M.8).
  const float near_plane = 0.01f * base;
  // far reaches the lowest scene point below the eye (scene-aware), degrading to
  // the old ~3*base value for a flat scene. lowest_z is bounds.min.z clamped to
  // the focal so an above-focal scene still gets the +0.5*base flat-scene margin.
  const float lowest_z = scene_bounds_.valid ? std::min(scene_bounds_.min.z, state_.focal.z) : state_.focal.z;
  const float far_plane = eye_height + (state_.focal.z - lowest_z) + 0.5f * base;
  const float half_w = state_.ortho_scale * aspect;
  return glm::ortho(-half_w, half_w, -state_.ortho_scale, state_.ortho_scale, near_plane, far_plane);
}

glm::vec3 TopDownOrthoCamera::position() const {
  return state_.focal + glm::vec3{0.0f, 0.0f, eyeHeight()};
}

void TopDownOrthoCamera::setSceneBounds(const AABB& bounds) {
  scene_bounds_ = bounds;
}

void TopDownOrthoCamera::rotate(float dx_pixels, float /*dy_pixels*/) {
  state_.azimuth -= dx_pixels / kPixelsPerRadian;  // heading only; top-down has no pitch
}

void TopDownOrthoCamera::pan(float dx_pixels, float dy_pixels) {
  // Drag the map: translate the focal in the ground plane. Sensitivity scales with
  // ortho_scale so a pixel drags the same fraction of the view at any zoom.
  constexpr float kPanFraction = 0.0025f;
  state_.focal +=
      (-dx_pixels * mapRight(state_.azimuth) + dy_pixels * mapUp(state_.azimuth)) * state_.ortho_scale * kPanFraction;
}

void TopDownOrthoCamera::zoom(float scroll_ticks) {
  state_.ortho_scale = std::clamp(state_.ortho_scale * std::pow(0.9f, scroll_ticks), 1e-3f, 1e7f);
}

void TopDownOrthoCamera::zoomToCursor(float scroll_ticks, glm::vec2 cursor_px, int viewport_w, int viewport_h) {
  if (viewport_w <= 0 || viewport_h <= 0) {
    zoom(scroll_ticks);
    return;
  }
  const float aspect = static_cast<float>(viewport_w) / static_cast<float>(viewport_h);
  // Ground point under the cursor before and after the scale change; shift the
  // focal by the difference so it stays pixel-locked (exact, singularity-free).
  const auto ground_hit = [&](glm::vec2 px) -> glm::vec3 {
    const Ray ray = unprojectRay(px, viewport_w, viewport_h, viewMatrix(), projMatrix(aspect));
    float t = 0.0f;
    if (rayPlane(ray.origin, ray.dir, glm::vec3{0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, t)) {
      return ray.origin + t * ray.dir;
    }
    return ray.origin;
  };
  const glm::vec3 before = ground_hit(cursor_px);
  zoom(scroll_ticks);  // same clamped geometric scale step as center-of-view zoom
  const glm::vec3 after = ground_hit(cursor_px);
  state_.focal += (before - after);
}

void TopDownOrthoCamera::reset() {
  state_ = CameraState{};
  state_.perspective = false;
  state_.ortho_scale = kDefaultOrthoScale;
  state_.focal = glm::vec3{0.0f};
  state_.azimuth = 0.0f;
}

void TopDownOrthoCamera::fitToBoundingBox(const AABB& bounds) {
  if (!bounds.valid) {
    return;
  }
  state_.focal = glm::vec3{0.5f * (bounds.min.x + bounds.max.x), 0.5f * (bounds.min.y + bounds.max.y), 0.0f};
  const float half_extent = 0.5f * std::max(bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y);
  state_.ortho_scale = std::max(half_extent, 1e-3f);
}

void TopDownOrthoCamera::adoptState(const CameraState& state) {
  const CameraState sanitized = sanitizeCameraState(state);
  state_ = sanitized;
  if (sanitized.perspective) {
    // Coming from a perspective model: seed ortho_scale from the orbit radius so
    // the bird's-eye shows a comparable area.
    state_.ortho_scale = std::max(sanitized.radius, 1e-3f);
  }
  // The model's invariant is a ground-level pivot (reset()/fitToBoundingBox()
  // enforce it). Adopting an elevated focal.z (e.g. Fly at altitude) would put
  // the near/far clip range around z=focal.z, far-clipping the whole ground
  // scene to a blank view (M.9). Flatten the pivot to the floor.
  state_.focal.z = 0.0f;
  state_.perspective = false;
}

// ---------------------------------------------------------------------------
// FlyCamera — first-person free-look (eye + yaw/pitch, no orbit pivot).
// ---------------------------------------------------------------------------

FlyCamera::FlyCamera() {
  resetToDefault();
}

void FlyCamera::resetToDefault() {
  eye_ = glm::vec3{5.0f, 5.0f, 10.0f};
  const glm::vec3 fwd = glm::normalize(glm::vec3{0.0f} - eye_);  // look at the origin
  const AzimuthElevation orientation = azimuthElevationFromDir(fwd);
  yaw_ = orientation.azimuth;
  pitch_ = orientation.elevation;
}

glm::vec3 FlyCamera::forward() const {
  return sphericalDir(yaw_, pitch_);
}

float FlyCamera::workingDistance() const {
  if (scene_bounds_.valid) {
    return std::max(glm::length(eye_ - aabbCenter(scene_bounds_)), 1e-2f);
  }
  return kFlyNominalDistance;
}

float FlyCamera::motionDistance() const {
  // Floor the motion-step basis so pan/zoom never collapse near the AABB center,
  // where workingDistance() → ~0 froze the controls and made zoom() Zeno-decay (M.10).
  return std::max(workingDistance(), kFlyNominalDistance);
}

glm::mat4 FlyCamera::viewMatrix() const {
  return glm::lookAt(eye_, eye_ + forward(), glm::vec3{0.0f, 0.0f, 1.0f});
}

glm::mat4 FlyCamera::projMatrix(float aspect) const {
  float near_plane = 0.0f;
  float far_plane = 0.0f;
  adaptiveNearFar(workingDistance(), sceneReach(scene_bounds_, eye_), near_plane, far_plane);
  // Clamp the near plane small (M.10): distance-to-AABB-center is unrelated to
  // what is in front of a fly camera, so a peripheral eye in a large scene would
  // otherwise get a metre-scale near that clips geometry the user flew in to
  // inspect. Keep the far/near ratio cap for depth precision as the lower bound.
  near_plane = std::max(std::min(near_plane, 0.05f), far_plane * 1e-5f);
  return glm::perspective(fov_y_, aspect, near_plane, far_plane);
}

glm::vec3 FlyCamera::position() const {
  return eye_;
}

void FlyCamera::setSceneBounds(const AABB& bounds) {
  scene_bounds_ = bounds;
}

void FlyCamera::rotate(float dx_pixels, float dy_pixels) {
  // First-person look: dragging turns the camera in place (opposite sign to the
  // orbit, which drags the scene around its pivot) — drag right looks right,
  // drag down looks down.
  yaw_ += dx_pixels / kPixelsPerRadian;
  pitch_ = std::clamp(pitch_ - dy_pixels / kPixelsPerRadian, -kPolarLimit, kPolarLimit);
}

void FlyCamera::pan(float dx_pixels, float dy_pixels) {
  const glm::vec3 fwd = forward();
  const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3{0.0f, 0.0f, 1.0f}));
  const glm::vec3 up = glm::cross(right, fwd);
  eye_ += panOffset(right, up, dx_pixels, dy_pixels, motionDistance());
}

void FlyCamera::zoom(float scroll_ticks) {
  eye_ += forward() * scroll_ticks * motionDistance() * 0.1f;  // +ticks = dolly forward
}

void FlyCamera::zoomToCursor(float scroll_ticks, glm::vec2 /*cursor_px*/, int /*viewport_w*/, int /*viewport_h*/) {
  zoom(scroll_ticks);  // degenerates to a forward dolly for fly (documented)
}

void FlyCamera::reset() {
  resetToDefault();
}

void FlyCamera::fitToBoundingBox(const AABB& bounds) {
  if (!bounds.valid) {
    return;
  }
  // Back off along -forward so the whole box is in view.
  eye_ = aabbCenter(bounds) - forward() * std::max(aabbDiagonal(bounds), 1.0f);
}

CameraState FlyCamera::state() const {
  CameraState s;
  s.focal = eye_ + forward() * kFlyNominalDistance;  // synthesize a pivot in front (nominal)
  s.radius = kFlyNominalDistance;
  // CameraState azimuth/elevation parameterize the FOCAL-TO-EYE direction
  // (OrbitCamera::position() = focal + radius*sphericalDir(az, el)), the REVERSE
  // of forward(). Emit the reversed direction (H.2) so an orbit/XYOrbit adopting
  // this state lands its eye on eye_ and looks along the original forward, rather
  // than teleporting 2*nominal forward and flipping the view 180°.
  s.azimuth = wrapToPi(yaw_ + std::numbers::pi_v<float>);
  s.elevation = -pitch_;  // pitch_ is pole-clamped, so -pitch_ is within kPolarLimit
  s.fov_y = fov_y_;
  s.ortho_scale = ortho_scale_;
  s.perspective = true;
  return s;
}

void FlyCamera::adoptState(const CameraState& state) {
  const CameraState sanitized = sanitizeCameraState(state);
  // Carry the projection/look fields verbatim (parity with the other models, which
  // round-trip fields they don't themselves consume); fov_y_ also drives projMatrix().
  fov_y_ = sanitized.fov_y;
  ortho_scale_ = sanitized.ortho_scale;
  if (sanitized.perspective) {
    // Sit at the orbit eye and look back toward its focal.
    eye_ = sanitized.focal + sanitized.radius * sphericalDir(sanitized.azimuth, sanitized.elevation);
    const AzimuthElevation orientation = azimuthElevationFromDir(glm::normalize(sanitized.focal - eye_));
    yaw_ = orientation.azimuth;
    pitch_ = orientation.elevation;
  } else {
    // From top-down ortho: float above the focal looking straight down.
    eye_ = sanitized.focal + glm::vec3{0.0f, 0.0f, kOrthoEyeHeightFactor * sanitized.ortho_scale};
    yaw_ = sanitized.azimuth;
    pitch_ = -kPolarLimit;
  }
}

// ---------------------------------------------------------------------------
// CameraState JSON (de)serialization (layout persistence).
// ---------------------------------------------------------------------------

std::string cameraStateToJson(const CameraState& state) {
  const nlohmann::json j{
      {"focal", {state.focal.x, state.focal.y, state.focal.z}},
      {"radius", state.radius},
      {"azimuth", state.azimuth},
      {"elevation", state.elevation},
      {"fov_y", state.fov_y},
      {"ortho_scale", state.ortho_scale},
      {"perspective", state.perspective},
  };
  return j.dump();
}

CameraState cameraStateFromJson(const std::string& json, const CameraState& fallback) {
  const nlohmann::json parsed = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return fallback;  // unparseable / not an object → fallback unchanged
  }
  CameraState state = fallback;
  // value() throws json::type_error when a key is present with the wrong type
  // (e.g. a hand-edited layout with "radius":"fast") — allow_exceptions=false
  // only suppresses *parse* errors, not these. Treat any such field as malformed
  // and fall back, honoring the tolerant contract.
  try {
    if (const auto it = parsed.find("focal"); it != parsed.end() && it->is_array() && it->size() == 3 &&
                                              (*it)[0].is_number() && (*it)[1].is_number() && (*it)[2].is_number()) {
      state.focal = glm::vec3{(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()};
    }
    state.radius = parsed.value("radius", state.radius);
    state.azimuth = parsed.value("azimuth", state.azimuth);
    state.elevation = parsed.value("elevation", state.elevation);
    state.fov_y = parsed.value("fov_y", state.fov_y);
    state.ortho_scale = parsed.value("ortho_scale", state.ortho_scale);
    state.perspective = parsed.value("perspective", state.perspective);
  } catch (const nlohmann::json::exception&) {
    return fallback;  // wrong-typed field → fallback unchanged
  }
  // Clamp out-of-range / non-finite values (e.g. "radius":0) before they reach
  // glm::perspective / glm::lookAt and produce a degenerate view.
  return sanitizeCameraState(state);
}

}  // namespace pj::scene3d
