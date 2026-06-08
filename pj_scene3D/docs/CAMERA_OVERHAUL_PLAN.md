# Scene3D Camera Overhaul — Implementation Plan & Handoff

> **Status:** ✅ **implemented in PR #152** (`feat/scene3d-camera-overhaul`). Kept as the original
> design/handoff record. Where this plan and the shipped code disagree, **the code is authoritative** —
> notably `zoomToCursor` shipped as a homothety about the cursor point (scale eye *and* focal toward
> the target), not the pivot-fraction method in the prompts below, and the overlay controls sit
> **top-right** (left of the orientation gizmo), not top-left.
>
> **Companion doc:** [`CAMERA_MODELS_DESIGN.md`](./CAMERA_MODELS_DESIGN.md) — the full investigation
> report (5-viewer research, math derivations, citations). This plan is the *how*; that doc is the
> *what + why*.

---

## 0. Orientation (read first)

- **Worktree:** `/home/davide/ws_plotjuggler/PJ4/.worktrees/scene3d-camera-overhaul` — **branch
  `feat/scene3d-camera-overhaul`**, based on **`final_scene3D`** (the factory-unified `pj_scene_common`
  base, tip `72c677b`). Verify with `git rev-parse --abbrev-ref HEAD`. Do all work in this worktree.
- **Re-target note (IMPORTANT):** this plan was first drafted against `feat/scene3d-occupancy-grid`.
  It has been re-targeted for `final_scene3D`, where the `pj_scene_common` factory unification reshaped
  the dock/entity layer. `camera.h`/`camera.cpp` are byte-identical across the two branches (Units 1–4
  port verbatim), but the entity base, the entity collection, and several line anchors changed — the
  Unit 0/5/6 prompts below already reflect `final_scene3D`. A clean tree (no unrelated WIP) is expected.
- **Collaboration model (`pj_scene3D/CLAUDE.md`):** **Codex writes the implementation code**; Claude
  is lead engineer + reviewer (drafts Codex prompts, reviews each deliverable, surfaces diffs for
  user-approved commits). The 8 Codex prompts in §5 (Units 0–7) are the executable artifacts.
- **Commit policy (root `CLAUDE.md`):** **never commit autonomously** — surface the diff, get
  explicit approval per turn.
- **Build:** `./build.sh` (incremental; ccache auto-detected). Needs Qt 6.8.3 in `./.qt/`.
- **Run:** `./run.sh` (unsets `QT_IM_MODULE` to avoid an IBus segfault).
- **Tests (headless, the TDD gate):** each `pj_scene3D/tests/*_test.cpp` listed in
  `PJ_SCENE3D_CORE_TESTS` (`pj_scene3D/core/CMakeLists.txt:26`) compiles to an executable named by
  file stem, linked against **`pj_scene3d_core + GTest::gtest_main` only — no Qt, no OpenGL**, and
  registered via `add_test`. Run them with:
  ```bash
  cmake --build build -j$(nproc)
  ctest --test-dir build -R 'camera' --output-on-failure        # camera_* tests
  ctest --test-dir build --output-on-failure                    # full scene3d suite
  ```
  This headless-ness is *why* the camera moves into `pj_scene3d_core` (§3.1) — the math becomes
  testable without linking Qt6::Widgets/OpenGLWidgets.

---

## 1. Task & locked decisions

**Problem:** the 3D camera misbehaves on **large areas**, zoom especially. Root causes are verified
against the tree (§3). **User-locked decisions:**

- **Combo models (4):** Orbit (improved) · Top-down orthographic · First-person/Fly · XYOrbit (horizon-locked).
- **Home button** (home icon, top-right overlay, left of the gizmo) = **reset to a fixed default view**, *not* fit-to-scene.
- **Sequencing:** research-first (done) → re-plan implementation (this doc).

---

## 2. Status

| Item | State |
|---|---|
| 5-viewer investigation (three.js/threepp, RViz, MeshLab, Rerun, Foxglove) | ✅ done — [design doc](./CAMERA_MODELS_DESIGN.md) |
| Diagnosis verified against current code | ✅ done |
| Camera-model design (abstraction, 4 models, zoom-to-cursor, near/far, ortho, home) | ✅ done (critique-corrected) |
| 7 Codex implementation prompts | ✅ drafted (§5) |
| **Any implementation code** | ✅ **done — PR #152** |
| Open questions needing user input | ⏳ 3 material ones (§7) — recommended defaults given |
| Workflow raw result (diagnosis/findings/report/critique JSON) | `/tmp/claude-1000/-home-davide-ws-plotjuggler-PJ4/3fb792b5-35d0-4b8d-ba9e-c2cae88614a1/tasks/whxuphdgi.output` (transient `/tmp`; the design doc is the durable copy) |

---

## 3. Verified diagnosis (the bugs we're fixing)

1. **No zoom-to-cursor.** `wheelEvent` forwards only tick count, discards `event->position()`;
   `zoom()` only moves along the focal→eye axis → always center-of-view.
   `scene_view_widget.cpp:224-226` (final_scene3D), `camera.cpp:42-66`.
2. **Hard limits kill large scenes.** radius clamped `[0.1, 1000]` + `far` fixed `1000` → can't
   frame or even see past ~1 km. `camera.cpp:65`, `camera.h:28`.
3. **Depth precision collapse.** `near=0.05`/`far=1000` (20000:1), constant → z-fighting at scale.
   `camera.h:27-28`, `camera.cpp:17-19`.
4. **No recovery.** `reset()`/`fitToBoundingBox()` have **zero call sites**; no Home control.
   `camera.cpp:68-75`.
5. **Jumpy zoom.** Additive `max(radius*0.15, 0.05)` step → huge absolute jumps far out.
   `camera.cpp:54`.

**Key research finding:** of the 4 robotics viewers, **none** do zoom-to-cursor (all zoom to
focal/center); only three.js does. So cursor-anchored zoom is a deliberate three.js-borrowed UX
upgrade, gated behind a unit test, with center-of-view `zoom()` as fallback.

---

## 4. Design in brief (full detail in the design doc)

- **Abstraction (RViz-style):** `ICamera` interface + shared serializable `CameraState` + `AABB`,
  **moved into `pj_scene3d_core`** (Qt/GL-free → headless-testable). 4 controllers:
  `OrbitCamera : ICamera` **(not final)** → `XYOrbitCamera final : OrbitCamera`;
  `TopDownOrthoCamera final : ICamera`; `FlyCamera final : ICamera`.
  `SceneViewWidget` holds `unique_ptr<ICamera>`; `setCameraModel()` does capture-state → construct →
  `adoptState` → swap. `reset()` is virtual (the old `*this = OrbitCamera{}` is illegal through a base).
- **Zoom-to-cursor (CORRECTED):** slide the *eye* along the cursor ray
  (`new_eye = mix(target, eye, pow(0.9, ticks))`, target from ray∩ground-plane), then re-derive
  `radius/azimuth/elevation`. The original "hold-angles-fixed, lerp-focal" closed form was proven
  wrong for off-axis targets and dropped. Ortho: before/after unproject, `focal += before - after`.
- **Adaptive near/far (CORRECTED — decoupled):** `near` from working distance only
  (`max(d*1e-2, 1e-3)`); `far` from `max(d*4, scene_reach)*1.5`; ratio cap `near = max(near, far/1e5)`.
  Separate **ortho flat-scene guard** (near/far from eye-height, not scene_diag) so a zoomed costmap
  never clips ground. Clamp `[max(2*near, 1e-3), bounds? max(1e7, scene_reach*5) : 1e7]`. Geometric
  zoom step `pow(0.9, ticks)`.
- **Scene bounds:** new `virtual std::optional<AABB> Scene3DEntity::worldBounds()`; dock unions over
  entities and pushes to `camera().setSceneBounds()`. No reporter → `valid=false` → working-distance
  fallback.
- **Home:** `camera().reset()` (fixed default per model). **Not** fit.
- **UI:** clone the existing `frame_overlay_combo_` overlay pattern (`Scene3DDockWidget.cpp:94-106`
  create/style/raise, `:463-490` layout) for `camera_model_combo_` + `home_button_`, both children of
  `this` (NOT the `QOpenGLWidget` — z-order). Keep objectNames stable (porting policy).

---

## 5. Execution — 7 Codex units (verbatim prompts)

Run **in order**. Each unit must build green and keep existing tests passing before the next. After
each Codex deliverable, **Claude reviews the diff**, then surface to the user for commit approval
(never auto-commit). **Unit 3's NDC-invariance test is a hard acceptance gate — do not proceed past
it if it fails.**

> Hand each prompt to Codex via the module's Codex workflow (e.g. the `codex:rescue` /
> `codex:codex-cli-runtime` path). Prompts are copy-paste ready.

### Unit 0 — `worldBounds()` on `Scene3DLayer` (the dependency everything else needs)
```
UNIT 0 - worldBounds() on Scene3DLayer. In pj_scene3D, add `struct AABB { glm::vec3 min{0.f}; glm::vec3 max{0.f}; bool valid{false}; };` to a new header pj_scene3D/core/include/pj_scene3d_core/camera/camera.h (namespace pj::scene3d). Add a NON-pure virtual `[[nodiscard]] virtual std::optional<AABB> worldBounds() const { return std::nullopt; }` to class Scene3DLayer in pj_scene3D/widgets/include/pj_scene3d_widgets/scene3d_entity.h (`Scene3DEntity` is a `using` alias for `Scene3DLayer`; add it to Scene3DLayer, NOT to PJ::ISceneLayer in pj_scene_common — worldBounds() is 3D-specific and pj_scene_common must stay 2D/3D-neutral). The default keeps existing layers compiling. Implement worldBounds() in PointCloudEntity (entities/pointcloud_entity.{h,cpp} - min/max over the decoded cloud positions, expressed in the entity's source frame; return nullopt before any data) and OccupancyGridEntity (entities/occupancy_grid_entity.{h,cpp} - origin + extents from resolution*width / resolution*height of the reconstructed grid). Add a headless GTest in pj_scene3D/tests for the grid AABB (compute from a known origin/resolution/size) wired into PJ_SCENE3D_CORE_TESTS (pj_scene3D/core/CMakeLists.txt). Build green; no behavior change in rendering.
```

### Unit 1 — `ICamera` + helpers in `pj_scene3d_core`, reshape `OrbitCamera`, delete widgets camera
```
UNIT 1 - ICamera abstraction + helpers IN pj_scene3d_core. Create pj_scene3d_core/camera/{camera.h,camera_math.h} and src/camera/{camera.cpp,camera_math.cpp}; add all four to PJ_SCENE3D_CORE sources in pj_scene3D/core/CMakeLists.txt. (a) CameraState{ glm::vec3 focal{0}; float radius{5}; float azimuth{0}; float elevation=glm::radians(30.f); float fov_y=glm::radians(45.f); float ortho_scale{5}; bool perspective{true}; }. (b) pure-virtual ICamera: viewMatrix()/projMatrix(float aspect)/position() const, setSceneBounds(const AABB&), rotate/pan/zoom(float), zoomToCursor(float ticks, glm::vec2 cursor_px, int w, int h), reset(), fitToBoundingBox(const AABB&), state() const, adoptState(const CameraState&). (c) camera_math.h free funcs: struct Ray{glm::vec3 origin,dir;}; Ray unprojectRay(glm::vec2,int,int,const glm::mat4& view,const glm::mat4& proj) (NDC with Qt y-flip, inverse(proj*view), near/far unproject); bool rayPlane(glm::vec3 ro,glm::vec3 rd,glm::vec3 p,glm::vec3 n,float& t). (d) class OrbitCamera : public ICamera (NOT final) holding CameraState by value; viewMatrix=glm::lookAt(position(),focal,+Z), position()=spherical formula, rotate/pan COPIED VERBATIM from the old widgets camera.cpp:29-40, reset() rewrites CameraState in place to defaults (DO NOT use *this=OrbitCamera{}). Then DELETE pj_scene3D/widgets/include/pj_scene3d_widgets/camera.h and src/camera.cpp, remove src/camera.cpp from widgets/CMakeLists.txt, and repoint scene_view_widget.h's include to pj_scene3d_core/camera/camera.h (SceneViewWidget still holds OrbitCamera by value for THIS unit). Build + all existing tests green; no behavior change.
```

### Unit 2 — decoupled near/far + geometric zoom + lifted clamp + ortho guard
```
UNIT 2 - Decoupled adaptive near/far + geometric zoom + lifted clamp + ortho guard (OrbitCamera). projMatrix(aspect): d = perspective? radius : ortho_scale; near = max(d*1e-2f, 1e-3f) [working-distance only - DO NOT use scene bounds for near]; scene_reach = bounds.valid? length(bounds.max-bounds.min)+length(focal-center(bounds)) : 0; far = max(d*4.f, scene_reach)*1.5f; near = max(near, far/1e5f) [ratio cap]. setSceneBounds stores the AABB. Replace the additive zoom step (max(radius*0.15,0.05)) with GEOMETRIC: factor=pow(0.9f,ticks); new_radius=clamp(radius*factor, minDist, maxDist), minDist=max(2*near, 1e-3f), maxDist=bounds.valid? max(1e7f, scene_reach*5.f):1e7f. REMOVE the fixed far=1000/near=0.05 and the [0.1,1000] clamp and the two-stage dolly branch. Add a headless GTest (pj_scene3D/tests, into PJ_SCENE3D_CORE_TESTS) asserting: for radius in {0.5,2,50,5000,1e6} with a scene_diag=5000 AABB, near is STRICTLY LESS than the eye-to-focal distance (close inspection never clips), far >= scene_reach, far/near <= 1e5, and a 5000-unit scene is framable (radius can reach >= scene_diag).
```

### Unit 3 — zoom-to-cursor faithful port + **NDC-invariance test (acceptance gate)**
```
UNIT 3 - zoomToCursor faithful port + NDC-invariance test. Implement OrbitCamera::zoomToCursor(ticks,cursor_px,w,h). PERSPECTIVE: view=viewMatrix(), proj=projMatrix(w/h); ray=unprojectRay(...); target = ray.origin+t*ray.dir via rayPlane vs ground(point=0,normal=+Z,t>0) else vs focal-plane(point=focal,normal=normalize(focal-eye),t>0); if neither, call zoom(ticks) and return. eye=position(); k=pow(0.9f,ticks); new_eye=mix(target,eye,k); pivot_frac = length(target-eye)>1e-6? length(focal-eye)/length(target-eye):1; new_focal=mix(new_eye,target,pivot_frac); d=new_eye-new_focal; r=clampRadius(length(d)); azimuth=atan2(d.y,d.x); elevation=clamp(asin(d.z/max(r,1e-6f)), -kPolarLimit, kPolarLimit); radius=r; focal=new_focal. ORTHO branch (perspective==false): before=groundHit(cursor_px,w,h) [unproject onto z=0 with current ortho_scale]; ortho_scale=clamp(ortho_scale*pow(0.9f,ticks),...); after=groundHit(...); focal+=(before-after). Headless GTest: pick world P, compute its NDC under (view,proj), call zoomToCursor at P's pixel, recompute P's NDC after - assert unchanged within 1e-3 for BOTH a perspective Orbit and an ortho TopDownOrtho setup; assert no NaN near nadir. This test is the acceptance gate - do not proceed if it fails.
```

### Unit 4 — XYOrbit / TopDownOrtho / Fly controllers
```
UNIT 4 - XYOrbit / TopDownOrtho / Fly controllers. In camera.{h,cpp}: OrbitCamera is NOT final. class XYOrbitCamera final : public OrbitCamera - override pan() to ray-cast the screen delta vs plane z=0 and force focal.z=0; override zoomToCursor() to force the ground target then set focal.z=0 and re-derive radius/azimuth/elevation. class TopDownOrthoCamera final : public ICamera - perspective=false; viewMatrix()=glm::lookAt(focal+vec3(0,0,2*ortho_scale), focal, vec3(-sin(azimuth),cos(azimuth),0)); projMatrix(aspect): eye_height=2*ortho_scale; near=eye_height*0.01f; far=eye_height+sceneVerticalExtent()+eye_height*0.5f; return glm::ortho(-ortho_scale*aspect,ortho_scale*aspect,-ortho_scale,ortho_scale,near,far); rotate()=azimuth ONLY; pan()=focal XY scaled by ortho_scale/viewport; zoom/zoomToCursor rescale ortho_scale. class FlyCamera final : public ICamera - state=eye+yaw+pitch (clamp pitch to +/-(pi/2-eps)), up=+Z, rotate=look in place, pan=strafe up/right, zoom/zoomToCursor=dolly along forward; synthetic working distance (dist to scene center or nominal 5) for near/far; reset()=eye(5,5,10) looking at origin. ALL implement state()/adoptState: ortho seeds ortho_scale from incoming radius and vice-versa; Orbit->Fly eye=focal+radius*dir; Fly->Orbit focal = eye + forward * 5 (fixed nominal for now - flagged as open question). Headless GTest: reset() restores per-model defaults; adoptState round-trips focal/azimuth across Orbit<->TopDownOrtho within eps.
```

### Unit 5 — `SceneViewWidget` integration (`unique_ptr<ICamera>`, model switch, cursor into wheel)
```
UNIT 5 - SceneViewWidget integration (final_scene3D anchors). scene_view_widget.h (member ~:96, camera() accessor ~:62): change member to std::unique_ptr<ICamera> camera_{std::make_unique<OrbitCamera>()}; camera() returns ICamera&. Add enum class CameraModel { Orbit, TopDownOrtho, Fly, XYOrbit } and void setCameraModel(CameraModel m) that captures s=camera_->state(), constructs the controller, calls adoptState(s), assigns to camera_, update(). scene_view_widget.cpp: ViewParams build (~:158-159) camera_. -> camera_->. wheelEvent (~:224-226): camera_->zoomToCursor(angleDelta().y()/120.f, glm::vec2(event->position().x(),event->position().y()), width(), height()); update(). KEEP the RIGHT-drag branch (~:218, currently camera_.zoom(dy*0.01f)) as center-of-view camera_->zoom(dy*0.01f) - DO NOT route incremental drag deltas through zoomToCursor (focal creep). Add void setSceneBounds(const AABB&) forwarding to camera_->setSceneBounds. Build + all existing tests pass.
```

### Unit 6 — Dock overlay (camera-model combo + home button) + bounds aggregation
```
UNIT 6 - Dock overlay + bounds aggregation (final_scene3D anchors). Scene3DDockWidget.{h,cpp}: clone the frame_overlay_combo_ pattern (creation/translucent stylesheet/raise at ~:127-135) to add camera_model_combo_ (QComboBox items Orbit, Top-down ortho, Fly, XYOrbit; child of `this`, NOT view_) and home_button_ (QToolButton, home/recenter icon, same translucent style, child of `this`). Extend layoutFrameOverlayCombo() (~:480-502) to also position camera_model_combo_ (just below the frame combo) and home_button_ (beside it) relative to view_->pos()+kMargin; resizeEvent (~:504-506) already calls it. Wire camera_model_combo_::currentIndexChanged(idx)->view_->setCameraModel(static_cast<CameraModel>(idx))+view_->update(); home_button_::clicked->view_->camera().reset()+view_->update(). BOUNDS AGGREGATION (no entities_ map on final_scene3D): the chokepoint is the existing override Scene3DDockWidget::syncViewLayers(const std::vector<ISceneLayer*>& ordered_layers) (~:248), which already builds std::vector<Scene3DLayer*> ordered and calls view_->setEntities(ordered). Add a helper updateSceneBounds() that unions worldBounds() over those Scene3DLayer* (skip nullopt; valid only if >=1 reports) and calls view_->setSceneBounds(union); call it from syncViewLayers (after setEntities) AND from Scene3DDockWidget::onTrackerTime(double) (~:194, after view_->setTrackerTime) since cloud/grid geometry changes over time. (Per-topic access available via entityFor(topic_id).) Keep all objectNames stable. No fit-to-scene on Home (reset-to-default only).
```

### Unit 7 — persist `CameraState` + active model in layout XML (IN SCOPE — user-confirmed)
```
UNIT 7 - persist camera model + state in layout XML (final_scene3D anchors). In Scene3DDockWidget::xmlSaveState (~:572, where it already writes fixed_frame_mode) also write the active CameraModel (as a stable string, e.g. "orbit"/"top_down_ortho"/"fly"/"xy_orbit") and the camera().state() (CameraState fields: focal xyz, radius, azimuth, elevation, fov_y, ortho_scale, perspective) as attributes/child element. In xmlLoadState (~:586) read them back: setCameraModel(parsed) then camera().adoptState(parsed_state); tolerate missing attributes (older layouts) by leaving defaults. Add a headless round-trip test if the (model,state)->xml->(model,state) path can be exercised without Qt widgets (serialize/deserialize CameraState free functions in pj_scene3d_core); otherwise cover CameraState (de)serialization in core and verify the dock wiring manually. Keep attribute names stable.
```

---

## 6. Per-unit verification

| Unit | Build/test check |
|---|---|
| 0 | `ctest -R occupancy` style grid-AABB test passes; full suite green; rendering unchanged. |
| 1 | `cmake --build build` green; **all existing** scene3d tests still pass; app launches, camera behaves exactly as before. |
| 2 | `ctest -R camera_near_far` passes for radii {0.5,2,50,5000,1e6}; 5000-unit scene framable. |
| 3 | **`ctest -R camera_zoom_to_cursor` passes** (NDC within 1e-3, persp + ortho, no NaN at nadir). HARD GATE. |
| 4 | `ctest -R camera_state_transfer` passes (reset defaults; Orbit↔TopDownOrtho round-trip). |
| 5 | Build green; manual: wheel now zooms toward cursor; right-drag still center-zoom. |
| 6 | Build green; combo switches all 4 models; Home resets; overlay lays out top-right, survives resize. |

**End-to-end (manual, after Unit 6):** `./run.sh`, load a **large MCAP** (map/odom fixed frame, data
hundreds of metres out — the original failure case). Confirm: (a) wheel zoom keeps the point under
the cursor fixed; (b) you can zoom out far enough to frame the whole scene with no far-clip; (c) no
z-fighting at distance; (d) Top-down ortho shows a clean bird's-eye costmap with no ground clipping;
(e) Home returns to the default view; (f) Fly and XYOrbit behave per §4. Per module CLAUDE.md, prefer
reproducing any reported issue as a headless test before fixing.

---

## 7. Open questions

**Locked (no action):** 4 models; Home = reset-to-default; camera math in `pj_scene3d_core`;
zoom step geometric; finite ratio-clamped near/far (reversed-Z deferred).

**Material — DECIDED with the user (2026-06-01):**
1. **Right-drag zoom = keep center-of-view.** Only the wheel is cursor-anchored; per-delta cursor-anchoring causes focal creep. (Unit 5 keeps `camera_->zoom()` on right-drag.)
2. **Mouse bindings = RViz-uniform for all 4 models** (LEFT=rotate, RIGHT=zoom, MIDDLE=pan; top-down rotates yaw-only). No map-convention LEFT=pan.
3. **Persist camera state in layout XML = do it now** → Unit 7 is in-scope (no longer gated).

**Minor (sensible defaults baked into the prompts):**
4. `worldBounds()` frame — entity **source frame** (cheaper) vs fixed frame (needs TF at query). Default: source frame; revisit if the union looks wrong across mixed frames.
5. Fly→Orbit focal synthesis — **fixed nominal 5-unit forward** vs ray-cast onto z=0. Default: nominal.
6. Reset/model-switch animation — **instant** for v1.
7. Touchpad `pixelDelta()` smooth scroll — **detents-only** for v1.
8. Ortho point-size shader — **reuse perspective formula** for v1 (acceptable; `u_orthographic` path is an isolated follow-up).

---

## 8. Key file references

| Path | Role |
|---|---|
_All anchors below are **`final_scene3D`** (tip `72c677b`)._

| Path | Role |
|---|---|
| `pj_scene3D/widgets/include/pj_scene3d_widgets/camera.h`, `src/camera.cpp` | current `OrbitCamera` (identical to feat/scene3d-occupancy-grid) — **to be deleted** in Unit 1 (moves to core) |
| `pj_scene3D/core/CMakeLists.txt` | `add_library(pj_scene3d_core …)` sources (~`:8-12`) + `PJ_SCENE3D_CORE_TESTS` loop (~`:24-37`) — add new camera `.cpp`s + tests here |
| `pj_scene3D/widgets/src/scene_view_widget.cpp:158-159` | `ViewParams` build (`camera_.` → `camera_->`) |
| `…/scene_view_widget.cpp:197-226` | `mousePressEvent`/`mouseMoveEvent` (rotate `:214`/pan `:216`/right-drag zoom `:218`), `wheelEvent` (`:224-226`) |
| `…/scene_view_widget.h:62` (accessor), `:96` (member) | `camera()` accessor + `OrbitCamera camera_` member → `unique_ptr<ICamera>` |
| `pj_scene3D/widgets/src/Scene3DDockWidget.cpp:127-135` | `frame_overlay_combo_` create/style/raise (overlay pattern to clone) |
| `…/Scene3DDockWidget.cpp:480-502` (`layoutFrameOverlayCombo`), `:504-506` (`resizeEvent`) | overlay layout to extend |
| `…/Scene3DDockWidget.cpp:248` (`syncViewLayers`), `:194` (`onTrackerTime`) | **bounds-union chokepoints** (no `entities_` map: union over the `ordered` `Scene3DLayer*` vector) |
| `…/Scene3DDockWidget.cpp:572` (`xmlSaveState`), `:586` (`xmlLoadState`) | Unit 7 persistence (alongside `fixed_frame_mode`) |
| `pj_scene3D/widgets/include/pj_scene3d_widgets/scene3d_entity.h` | add `virtual worldBounds()` to **`Scene3DLayer`** (`Scene3DEntity` aliases it) |
| `pj_scene3D/widgets/src/entities/{pointcloud_entity,occupancy_grid_entity}.cpp` | implement `worldBounds()` (`PointCloudEntity`, `OccupancyGridEntity`) |
| `pj_scene3D/widgets/src/passes/pointcloud_render_pass.cpp` | point-size formula (ortho note, OQ 8) |

---

## 9. Gotchas (from project memory + the critique)

- **Do NOT promote the GL canvas to `WA_NativeWindow`** — conflicts with Qt-Advanced-Docking's
  native-flag management and breaks layout. (Not needed here; overlays are children of the dock, not
  the `QOpenGLWidget` — keep it that way.)
- **`OrbitCamera` cannot be `final`** (XYOrbit derives it) **and** `reset()` cannot be
  `*this = OrbitCamera{}` through `ICamera*` — rewrite `CameraState` in place. Both were critique catches.
- **Preserve widget `objectName`s** verbatim (porting policy) for new overlay widgets too.
- **Keep `pj_app`/`pj_runtime` domain-neutral** — none of this work belongs there; it's all in
  `pj_scene3D`. (Camera math → `pj_scene3d_core`; widgets/UI → `pj_scene3D/widgets`.)
- **The depth/alpha colorMask trick** (`scene_view_widget.cpp:187`) is projection-independent — ortho
  needs no change there.
- **Don't chain `pkill … ; ./build.sh`** (exits 144, skips the rebuild) — build standalone and verify
  the `.cpp` actually recompiled.
- **Doc freshness (root CLAUDE.md):** when this lands, update `pj_scene3D/docs/REQUIREMENTS.md` /
  `ARCHITECTURE.md` and the module `CLAUDE.md` if camera behavior/API is now user-facing.
