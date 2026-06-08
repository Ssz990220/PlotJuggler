# Scene3D Camera Overhaul — Investigation & Design Report

> Status: ✅ **implemented in PR #152** (`feat/scene3d-camera-overhaul`); kept as the *what + why*
> record. Produced from a 9-agent investigation workflow (survey → 5-viewer research → synthesize →
> adversarial critique → finalize). Where this report and the shipped code differ, **the code is
> authoritative** — notably `zoomToCursor` shipped as a homothety about the cursor point (§3.3), and
> the overlay controls sit top-right, left of the gizmo (§3.5–3.6), not top-left.

## 1. Why we're doing this

The 3D camera misbehaves on **large areas**, zoom especially. Reading the code pinned five
concrete causes (all quoted against the current tree):

| # | Symptom (large scenes) | Cause | Evidence |
|---|---|---|---|
| 1 | Can't zoom out far enough to frame the scene; far geometry clipped | radius hard-clamped to `[0.1, 1000]` **and** `far` fixed at `1000`; no scene-extent awareness | `camera.cpp:65`, `camera.h:28` |
| 2 | Severe z-fighting / depth flicker at distance | `near=0.05` vs `far=1000` = 20000:1 depth range, constant, not derived from radius/bounds | `camera.h:27-28`, `camera.cpp:17-19` |
| 3 | **Wheel zoom drifts to the wrong place** — pulls toward orbit center, not the cursor | `wheelEvent` forwards only tick count, discards `event->position()`; `zoom()` only moves along the focal→eye axis | `scene_view_widget.cpp:256-260`, `camera.cpp:42-66` |
| 4 | No one-click recovery from a mis-framed view | `reset()`/`fitToBoundingBox()` exist but have **zero call sites**; no Home/Fit control | `camera.cpp:68-75` |
| 5 | Zoom feels jumpy far out | step is **additive** `max(radius*0.15, 0.05)` → at radius 800 a tick jumps ~120 units | `camera.cpp:54` |

Architectural root: `OrbitCamera` is a single concrete, perspective-only class with no
abstraction boundary at which to vary projection (ortho), control scheme (fly/top-down), or
depth policy.

### Locked decisions (from you)
- **Combo models:** Orbit (improved) · Top-down orthographic · First-person/Fly · XYOrbit (horizon-locked).
- **Home button** (home icon, top-right, left of the gizmo) = **reset to a fixed default view**, *not* fit-to-scene.
- **Sequencing:** research first → this report → re-plan implementation before any code.
- Module policy: **Codex writes the implementation**; Claude designs + reviews.

## 2. How the reference viewers actually behave

Primary source was reached for every one (not just docs).

| Viewer | Zoom model | Zoom-to-cursor? | Top-down ortho | Large-scene depth |
|---|---|---|---|---|
| **three.js / threepp** | persp = dolly radius; ortho = `camera.zoom` rescale | **Yes** (`OrbitControls.zoomToCursor`) — the *only* zoom-to-cursor precedent | Ortho camera + `MapControls` preset | None automatic; only `min/maxDistance`. threepp C++ port lacks `zoomToCursor`/reset |
| **RViz / RViz2** | dolly `distance` (Orbit/XYOrbit/Follower); ortho-scale (TopDownOrtho); fwd dolly (FPS) | **No** (zooms to focal) | `FixedOrientationOrthoViewController`: `PT_ORTHOGRAPHIC`, scale→ortho window, angle about Z | Fixed `near=0.1`/`far=200`; no adaptive — known large-transform instability (rviz#502) |
| **MeshLab** | uniform **model scale** about fixed trackball center (object-space arcball) | **No** (scales about fixed center) | FOV→5 sentinel flips to `glOrtho`; "View From" presets | near/far derived from camera distance: `dist*clipRatioNear/Far` |
| **Rerun** | dolly orbit radius; fwd translate (first-person) | **No** — open issue #7900; zooms to center | Ortho exists but not an interactive button; `fov_y=None` | **Reversed/infinite-far** projection (`perspective_infinite_rh`, near=0.01, far=∞) |
| **Foxglove** | dolly `distance` (persp); ortho-scale from `distance` (ortho) | **No** | Toggle "Perspective" off (or press `3`) → dedicated `OrthographicCamera`, bird's-eye | Explicit user `near=0.5`/`far=5000`; optional **Log depth** toggle |

**Key honest finding:** of the four *robotics* viewers, **none** do zoom-to-cursor — they all
zoom to focal/center. Only three.js (a web lib) does. So cursor-anchored zoom is a deliberate
UX improvement borrowed from three.js, **not** "matching the robotics norm" — and it must be
ported faithfully and gated behind a unit test, with center-of-view zoom as the fallback.

## 3. Recommended design

### 3.1 Abstraction — RViz-style interchangeable controller, shared serializable state

Move the camera into **`pj_scene3d_core`** (Qt/GL-free static lib that already links `glm` and
has a wired GTest `tests/` dir). This is the load-bearing structural fix: camera math is pure
GLM, so headless unit tests become possible *without* linking Qt/OpenGL — satisfying the
module's TDD gate. `camera()` has **no external callers** outside `scene_view_widget.cpp`, so
the refactor is low-risk.

```
pj_scene3D/core/include/pj_scene3d_core/camera/camera.h        // ICamera, CameraState, AABB, 4 controllers
pj_scene3D/core/include/pj_scene3d_core/camera/camera_math.h   // unprojectRay, rayPlane (free, unit-tested)
pj_scene3D/core/src/camera/camera.cpp
pj_scene3D/core/src/camera/camera_math.cpp
// delete pj_scene3D/widgets/{include/pj_scene3d_widgets/camera.h, src/camera.cpp}
```

```cpp
struct AABB { glm::vec3 min{0.f}; glm::vec3 max{0.f}; bool valid{false}; };

struct CameraState {              // shared, serializable, used by all controllers (RViz mimic)
  glm::vec3 focal{0.f};
  float radius{5.f};
  float azimuth{0.f};
  float elevation{glm::radians(30.f)};
  float fov_y{glm::radians(45.f)};
  float ortho_scale{5.f};
  bool  perspective{true};
};

class ICamera {
 public:
  virtual ~ICamera() = default;
  virtual glm::mat4 viewMatrix() const = 0;
  virtual glm::mat4 projMatrix(float aspect) const = 0;
  virtual glm::vec3 position() const = 0;
  virtual void setSceneBounds(const AABB&) = 0;
  virtual void rotate(float dx_px, float dy_px) = 0;
  virtual void pan(float dx_px, float dy_px) = 0;
  virtual void zoom(float ticks) = 0;                                  // center-of-view
  virtual void zoomToCursor(float ticks, glm::vec2 cursor_px, int w, int h) = 0;
  virtual void reset() = 0;                                            // fixed defaults, NOT fit
  virtual void fitToBoundingBox(const AABB&) = 0;
  virtual CameraState state() const = 0;                               // mimic
  virtual void adoptState(const CameraState&) = 0;
};
```

Concrete: `OrbitCamera : ICamera` (**not** `final`); `XYOrbitCamera final : OrbitCamera`;
`TopDownOrthoCamera final : ICamera`; `FlyCamera final : ICamera`.
`SceneViewWidget` holds `std::unique_ptr<ICamera>`, `camera()` returns `ICamera&`,
`setCameraModel(CameraModel)` does capture→construct→`adoptState`→swap→`update()`. (`reset()`
is a virtual override — the old `*this = OrbitCamera{}` is illegal through a base pointer.)

### 3.2 The four models

- **Orbit (improved, default)** — perspective; LEFT=rotate, MIDDLE/SHIFT+LEFT=pan, WHEEL=zoom-to-cursor, RIGHT-drag=center-of-view zoom; world up +Z; geometric zoom step.
- **Top-down orthographic** — ortho looking straight down −Z; LEFT=rotate map about +Z, pan in XY scaled by `ortho_scale`, WHEEL=ortho zoom-to-cursor (exact, singularity-free). The robotics "2D mode" for costmaps/occupancy grids/trajectories.
- **First-person / Fly** — perspective; eye+yaw+pitch, no orbit pivot; LEFT=look around, strafe pan, RIGHT/WHEEL=forward dolly. Zoom-to-cursor degenerates to plain forward dolly (documented).
- **XYOrbit (horizon-locked)** — perspective Orbit but focal constrained to z=0 (ray-cast pan against the ground plane); orbit pivot never floats off the floor.

### 3.3 Zoom-to-cursor — CORRECTED (the original synthesis was mathematically wrong)

The first synthesis proposed holding azimuth/elevation fixed and lerping the focal
(`focal = mix(focal, target, 1 - r_new/r_old)`). The critique proved this only keeps the
hovered point screen-fixed when the target lies on the focal plane — for an off-axis
ground/cursor target it **drifts**, and incremental wheel deltas accumulate focal walk. Dropped.

**Faithful three.js port:** slide the *eye* along the cursor ray, then re-derive the full
spherical state. Shared helpers (pure GLM, unit-tested standalone):

```cpp
struct Ray { glm::vec3 origin, dir; };
Ray unprojectRay(glm::vec2 px, int w, int h, const glm::mat4& view, const glm::mat4& proj) {
  float nx = 2.f*px.x/float(w) - 1.f;
  float ny = 1.f - 2.f*px.y/float(h);                 // Qt y is top-down → flip to GL
  glm::mat4 inv = glm::inverse(proj * view);
  glm::vec4 pn = inv * glm::vec4(nx, ny, -1.f, 1.f); pn /= pn.w;
  glm::vec4 pf = inv * glm::vec4(nx, ny,  1.f, 1.f); pf /= pf.w;
  return { glm::vec3(pn), glm::normalize(glm::vec3(pf) - glm::vec3(pn)) };
}
bool rayPlane(glm::vec3 ro, glm::vec3 rd, glm::vec3 p, glm::vec3 n, float& t) {
  float denom = glm::dot(rd, n);
  if (std::abs(denom) < 1e-6f) return false;          // parallel → caller falls back to zoom()
  t = glm::dot(p - ro, n) / denom; return true;
}
```

Perspective (Orbit/XYOrbit): find ground (or focal-plane fallback) target under cursor; slide
`new_eye = mix(target, eye, pow(0.9, ticks))` (all points on this segment lie on the cursor
ray ⇒ target stays screen-fixed *by construction*). **As shipped (PR #152):** the focal is scaled
toward the target the same way — `new_focal = mix(target, focal, pow(0.9, ticks))`, a homothety
about the cursor point — instead of the preserved-pivot-fraction method sketched here; both keep the
hovered point pixel-locked. Re-derive `radius/azimuth/elevation` via `setEyeFocal`; clamp.
Degenerate denominator → `zoom()` (never NaN). **Acceptance gate:** an NDC-invariance
unit test (pick world point, record its screen NDC, zoom-to-cursor at that pixel, assert NDC
unchanged within 1e-3) — the port is not trusted until this passes.

Orthographic: `groundHit` before, rescale `ortho_scale *= pow(0.9, ticks)`, `groundHit` after,
`focal += (before - after)`. Exact and singularity-free.

### 3.4 Adaptive near/far — CORRECTED (decouple the two ends)

The original tied **both** near and far to `max(working_distance, scene_diag)`, which
re-introduces near-clipping for close inspection of a large scene (e.g. radius 2 inside a
5000-unit scene → near 5.0 clips the geometry) and breaks the ortho costmap case. Fixed:

```cpp
float d = perspective_ ? radius_ : ortho_scale_;        // working distance
float near = std::max(d * kNearFrac, kAbsMinNear);       // near ← working distance ONLY (~1% of d)
float scene_reach = scene_bounds_.valid
      ? glm::length(scene_bounds_.max - scene_bounds_.min) + glm::length(focal_ - sceneCenter())
      : 0.f;
float far = std::max(d * kFarMult, scene_reach) * kFarMargin;  // far ← max(working, scene extent)
near = std::max(near, far / kMaxDepthRatio);             // ratio cap (~1e5) — now does real work
```

Worked check (radius 2 in scene_diag 5000): near `0.02` (in front of the 2-unit eye — not
clipped), far `~7500+` (encloses scene), ratio capped raises near to `0.075` (still fine).
Close inspection **and** full enclosure **and** bounded depth precision — all hold.

**Ortho flat-scene guard** (new): the top-down eye sits at `z = focal.z + 2*ortho_scale`, so
near/far must be measured against that gap, not scene_diag: `near = eye_height*0.01`,
`far = eye_height + scene_vertical_extent + margin`. Without this, a zoomed-in costmap clips
the ground.

**Clamp:** replace `[0.1, 1000]` with `[max(2*near, kAbsMinDist), scene_bounds.valid ?
max(kFallbackMax, scene_reach*5) : kFallbackMax]` (Rerun uses ~5× bbox diag; `kFallbackMax`
finite ~1e7 to avoid NaN). **Step:** additive → geometric `pow(0.9, ticks)` (three.js/RViz),
fixing the "jumpy far out" complaint.

**Scene bounds source:** add `virtual std::optional<AABB> worldBounds() const` to
`Scene3DEntity`; dock unions per-entity bounds and pushes to `camera().setSceneBounds()`. No
entity reporting → `valid=false` → falls back to pure working-distance near/far.

*Deferred:* reversed-Z / infinite-far projection (Rerun's approach) — touches GL depth setup
and every render pass; ratio-clamped finite near/far is sufficient and localized to
`projMatrix()`.

### 3.5 Home button

Per your decision, **reset to fixed default view** (not fit). `home_button_::clicked →
view_->camera().reset() + view_->update()`. `reset()` is the virtual override per controller
(Orbit/XYOrbit: focal=origin, radius=5, az=0, el=30°; TopDownOrtho: focal=origin,
ortho_scale=5, az=0; Fly: eye=(5,5,10) toward origin). It does **not** change the active model.
`fitToBoundingBox` stays implemented/exposed for a possible future "Fit" affordance.

### 3.6 UI — reuse the existing overlay pattern verbatim

`Scene3DDockWidget` already overlays `frame_overlay_combo_` (translucent, `raise()`'d, manually
positioned in `layoutFrameOverlayCombo()`, children of `this` **not** the `QOpenGLWidget` — the
existing comment at `:84-87` documents the z-order reason). Clone it for `camera_model_combo_`
(4 items) and `home_button_` (`QToolButton`, home icon). Extend `layoutFrameOverlayCombo()`
(`:463-490`) to position all three near `view_origin + kMargin`; `resizeEvent` already calls it.
Keep objectNames stable per the porting policy.

## 4. Implementation outline (TDD order, file-mapped)

1. **`worldBounds()` on `Scene3DEntity` first** — the dependency the adaptive/Fit story needs. `scene3d_entity.h`, `entities/pointcloud_entity.cpp`, `entities/occupancy_grid_entity.cpp`, `core/.../camera/camera.h` (AABB).
2. **Camera in `pj_scene3d_core`** — `ICamera`/`CameraState`/`AABB`/helpers/`OrbitCamera`; add 4 files to `core/CMakeLists.txt`; delete the widgets camera files. Build stays green.
3. **Decoupled near/far + geometric zoom + lifted clamp + ortho guard** in `OrbitCamera::projMatrix`/`zoom`.
4. **`zoomToCursor`** faithful port + **NDC-invariance test** (acceptance gate).
5. **XYOrbit / TopDownOrtho / Fly** controllers (`OrbitCamera` not final; XYOrbit derives it).
6. **`SceneViewWidget`** → `unique_ptr<ICamera>`, `setCameraModel`, route `event->position()` into `wheelEvent`'s `zoomToCursor`, `setSceneBounds` pass-through. `scene_view_widget.{h,cpp}`.
7. **Dock overlay** — camera-model combo + home button + bounds aggregation. `Scene3DDockWidget.{h,cpp}`.
8. **Headless tests** in `pj_scene3D/tests/` (link `pj_scene3d_core` + GTest only): zoom-to-cursor NDC-invariance, near/far policy across radii {0.5,2,50,5000,1e6}, state-transfer/reset.
9. *(Decision-gated)* persist `CameraState` + active model in the dock's `xmlSaveState/xmlLoadState`.

Codex prompts (7 units: worldBounds → core abstraction → near/far → zoomToCursor+test →
controllers → widget integration → dock UI) are drafted and stored with the workflow result;
they'll be finalized in the implementation re-plan.

## 5. Open questions (need your call before/at the re-plan)

1. **Right-drag-zoom:** keep as center-of-view (this plan) or make it cursor-anchored? (Cursor-anchoring per-delta causes focal creep → drag-zoom-and-pan regression.)
2. **Per-model mouse bindings:** keep RViz-uniform (LEFT=rotate / RIGHT=zoom) everywhere, or adopt MapControls/Foxglove ground convention (LEFT=pan) for TopDownOrtho/XYOrbit?
3. **Fly→Orbit focal synthesis:** fixed nominal forward distance (5 units) vs ray-cast forward onto z=0? (The one lossy mimic transition.)
4. **`worldBounds()` frame:** report in the fixed frame (needs TF at query time) or entity source frame (cheaper, frame-mixed union)?
5. **Persist camera state** in layout XML now (survives reload, like Foxglove/Rerun) or follow-up?
6. **Animated vs instant** reset/model-switch — instant for v1 (this plan) or a short tween later?
7. **Touchpad/high-res wheel** — handle `pixelDelta()` smooth-scroll, or detents-only for now?
8. **Ortho point-size** — defer the dedicated `u_orthographic` shader path (v1 reuses the perspective formula, acceptable) ?
9. **Reversed-Z** — defer until finite-clamp proves insufficient on a real large MCAP?

## 6. Sources

**three.js / threepp:** [OrbitControls.js](https://raw.githubusercontent.com/mrdoob/three.js/dev/examples/jsm/controls/OrbitControls.js) ·
[MapControls.js](https://raw.githubusercontent.com/mrdoob/three.js/dev/examples/jsm/controls/MapControls.js) ·
[OrbitControls docs](https://threejs.org/docs/pages/OrbitControls.html) ·
[markaren/threepp](https://github.com/markaren/threepp) · [threepp OrbitControls.cpp](https://raw.githubusercontent.com/markaren/threepp/master/src/threepp/controls/OrbitControls.cpp)

**RViz2:** [User Guide (Jazzy)](https://docs.ros.org/en/jazzy/Tutorials/Intermediate/RViz/RViz-User-Guide/RViz-User-Guide.html) ·
[OrbitViewController](https://github.com/ros2/rviz/blob/rolling/rviz_default_plugins/src/rviz_default_plugins/view_controllers/orbit/orbit_view_controller.cpp) ·
[XYOrbitViewController](https://github.com/ros2/rviz/blob/rolling/rviz_default_plugins/src/rviz_default_plugins/view_controllers/xy_orbit/xy_orbit_view_controller.cpp) ·
[FPSViewController](https://github.com/ros2/rviz/blob/rolling/rviz_default_plugins/src/rviz_default_plugins/view_controllers/fps/fps_view_controller.cpp) ·
[FixedOrientationOrthoViewController (TopDownOrtho)](https://github.com/ros2/rviz/blob/rolling/rviz_default_plugins/src/rviz_default_plugins/view_controllers/ortho/fixed_orientation_ortho_view_controller.cpp) ·
[buildScaledOrthoMatrix](https://github.com/ros2/rviz/blob/rolling/rviz_rendering/src/rviz_rendering/orthographic.cpp) ·
[rviz#502 large-transform instability](https://github.com/ros-visualization/rviz/issues/502)

**MeshLab / VCGLib:** [trackball.cpp](https://raw.githubusercontent.com/cnr-isti-vclab/vcglib/main/wrap/gui/trackball.cpp) ·
[trackmode.cpp](https://raw.githubusercontent.com/cnr-isti-vclab/vcglib/main/wrap/gui/trackmode.cpp) ·
[glarea.cpp](https://github.com/cnr-isti-vclab/meshlab/blob/main/src/meshlab/glarea.cpp) ·
[navigation notes (Blyth)](https://simoncblyth.bitbucket.io/env/notes/graphics/meshlab/meshlab_navigation/)

**Rerun:** [eye.rs](https://github.com/rerun-io/rerun/blob/main/crates/viewer/re_view_spatial/src/eye.rs) ·
[ui_3d.rs](https://github.com/rerun-io/rerun/blob/main/crates/viewer/re_view_spatial/src/ui_3d.rs) ·
[#7900 zoom-to-cursor request](https://github.com/rerun-io/rerun/issues/7900)

**Foxglove / Lichtblick:** [3D panel docs](https://docs.foxglove.dev/docs/visualization/panels/3d) ·
[camera.ts](https://github.com/Lichtblick-Suite/lichtblick/blob/develop/packages/suite-base/src/panels/ThreeDeeRender/camera.ts) ·
[CameraStateSettings.ts](https://github.com/Lichtblick-Suite/lichtblick/blob/develop/packages/suite-base/src/panels/ThreeDeeRender/renderables/CameraStateSettings.ts) ·
[log-depth discussion #941](https://github.com/orgs/foxglove/discussions/941)
