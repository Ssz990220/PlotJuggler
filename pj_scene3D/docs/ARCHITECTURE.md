# pj_scene3D — Architecture (as built)

How the 3D scene renders and how robot models get on screen. This is the
distilled, as-built successor to the executed planning docs
(`PHOTOREALISM_AND_URDF_MESH_PLAN.md`, `URDF_MESH_ASSET_RESOLUTION_DESIGN.md`,
`CODEX_PROMPTS_URDF_MESH.md` — all removed 2026-06-10; see git history for the
full design rationale). The WHAT lives in [REQUIREMENTS.md](./REQUIREMENTS.md).

## Rendering pipeline

Per frame, `SceneViewWidget::paintGL`:

```
layers + passes → SceneHdrFbo (multisample RGBA16F + DEPTH32F + R8 is-mesh mask)
                → resolve blit (single-sample color + depth + mask)
                → SsaoPass (R16F AO, box-blurred)   [reads resolved depth]
                → EdlPass  (R16F shade factor)      [reads resolved depth + mask]
                → composite/present (fullscreen triangle → backing FBO)
```

- **HDR chain (0A).** The render FBO is created at a fixed MSAA sample count
  (`kDefaultMsaaSamples`, clamped to `GL_MAX_*_TEXTURE_SAMPLES`) — INDEPENDENT of
  the QOpenGLWidget's negotiated `context()->format().samples()`, which is 0 once
  the view is composited inside an ADS dock (the backing store is single-sample).
  Because SceneHdrFbo owns its own multisample textures and the present is a
  fullscreen draw, the single-sample backing FBO never undoes the already-resolved
  anti-aliasing — so the scene is 4x MSAA docked, not just in the demo. (Seeding
  the chain from the context's samples was the bug that made MSAA silently vanish
  in the app.) MSAA→MSAA blits with mismatched formats are illegal, so the resolve
  target is always single-sample; the present pass draws into
  `defaultFramebufferObject()` (never FBO 0 — QOpenGLWidget renders off-screen).
  When the chain is unavailable the widget falls back to direct-to-backing
  rendering (one warning per context).
- **Linear light + tonemap (0B).** All color inputs are linearized (colormaps,
  occupancy LUT, vertex/base colors, theme colors; mesh diffuse textures upload
  as `GL_SRGB8_ALPHA8`). The composite applies exposure → tonemap
  (None/ACES/AgX/Neutral; AgX matrices are **column-major** GLSL `mat3` ctors;
  Neutral is the Khronos PBR Neutral mapper, which preserves colormap hue better
  than ACES) → saturation → manual sRGB encode.
- **Annotation alpha-marker.** The scene FBO's alpha is a per-pixel
  tonemap-bypass marker, not coverage: annotation passes (TF triads via
  `ArrowGizmo`, HUD overlay) write alpha 0 so they present flat and vivid,
  while data draws restore the marker with coverage-union blending
  `(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` on the alpha channel (preserving
  destination alpha instead would ghost occluded annotations through opaque
  meshes). Gizmo opacity rides the gizmo color's alpha through
  `glBlendFuncSeparate(SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ZERO, ONE_MINUS_SRC_ALPHA)`.
- **Background bypass.** Far-plane pixels (depth == 1) keep the exact theme
  color, ungraded (no tonemap/saturation) — the background never shifts with the
  tonemap.
- **Is-mesh mask (EDL gating).** EDL is restricted to **mesh surfaces** — point
  clouds, grid, occupancy, axes and the empty background get no eye-dome contour.
  The scene FBO carries a second color attachment (`SceneHdrFbo::kMaskAttachment`,
  R8), cleared to 0 each frame; only `MeshRenderPass` enables that draw buffer (its
  fragment shader writes 1.0 to `location = 1`) — driven by `ViewParams::write_mesh_mask`,
  set on the off-screen + EDL-on path. The mask is resolved alongside color/depth
  and sampled by EDL. There is no separate object-class G-buffer: the alpha channel
  is the tonemap marker and is identical (1) for meshes and point clouds, so the
  mask is the only per-pixel mesh signal.
- **SSAO** reconstructs view position via `u_inv_proj` (works under the
  ortho camera; the perspective near/far formula does not) with an in-shader
  4×4-tiled hash as the rotation noise; it still applies scene-wide. **EDL** is
  Potree-derived (8 circular neighbours, log-depth response) but **mesh-only**: a
  mesh pixel is darkened when a neighbour is FARTHER — or is non-mesh, which the
  shader reads as "far" (`FAR_SENTINEL`) via the mask. So the mesh's silhouette
  against the void, point clouds, or farther meshes, plus the near side of its
  creases, get the contour; point clouds neither receive it nor cast it. Each
  per-neighbour log-depth gap is clamped to `look::kEdlMaxGap` so the huge
  silhouette gap reads as a graded outline rather than a solid black band; creases
  (much smaller gaps) are unaffected. With no mask bound EDL falls back to
  unrestricted (every pixel treated as mesh). Both passes multiply into the
  composite and degrade to no-ops when unavailable (`u_has_ao` / `u_has_edl`); EDL's
  darkening is floored (`CompositeParams::edl_floor`) so it bottoms out at a
  hue-preserving dark grey, `floor·color`, rather than pure black (floor 0 = the
  original multiply-to-black).
- **Defaults** (look-dev, 2026-06-13). The authoritative numbers live in
  [`scene_look_defaults.h`](../widgets/include/pj_scene3d_widgets/scene_look_defaults.h)
  — treat that header as the source of truth and this paragraph as a summary
  (it has drifted before). As of this writing: ACES, exposure 1.3, saturation
  1.3, SSAO on (strength 1, radius 0.4 m), EDL on (strength 1, radius 0.6 px,
  floor 0.3, max gap 0.02).
  Runtime knobs: `SceneViewWidget::compositeParams()`, `ssaoPass()`,
  `edlPass()`, and the per-view `meshShadingParams()` (roughness 0.6, f0 0.06,
  ambient 0.5, key/"sun" 1.6, fill 0.5, env-reflection 1.0, key-light dir
  azimuth 40° / elevation 55° ≈ high +X+Y — a stopgap for a future per-scene
  lighting object). Shader provenance/licenses: [`../THIRDPARTY.md`](../THIRDPARTY.md).
- **No shadows (yet).** Mesh shading has no shadow term: the fixed key light, the
  camera-locked fill, and the IBL ambient are all unoccluded. Adding mesh shadows
  would be a geometry **depth pre-pass** rendered from the light *before*
  `renderScene` — structurally unlike the screen-space SSAO/EDL **post-passes** —
  with the shadow factor multiplied into the key-light term only (the first summand
  of `direct` in `mesh_render_pass.cpp`). Don't mistake the SSAO/EDL passes as the
  template for shadows.

**GL context lifecycle (don't regress).** `QOpenGLWidget` recreates its context
on every ADS reparent. Every pass, layer, the HDR chain, and the present
program implement `releaseGL()` (wired to the dying context's
`aboutToBeDestroyed`) and rebuild lazily — VAOs/FBOs/textures are per-context,
never shared. The app deliberately does NOT set `AA_ShareOpenGLContexts`.

**TF frame hover labels.** Hovering a TF axis triad shows the frame's name in a
small HUD box and draws that triad brighter. The pick is pure screen-space and
lives in `SceneViewWidget`: `paintGL` caches `proj*view`, and a button-free
`mouseMoveEvent` projects every resolvable frame origin through the GL-free,
unit-tested `core/tf/frame_picking.h` (`projectFrameOrigin` + `pickNearestFrame`)
and takes the one closest to the cursor within ~20 logical px (first on an exact
tie). The label is drawn at the tail of `paintGL` as a 2D overlay — the same path
as the perf HUD — re-projecting the live origin so it stays glued to the frame as
the scene streams. Its text and panel are **CPU-rasterized into a `QImage` and
blitted with `drawImage`** (`hud_overlay.h::renderHudPanel`), *not* painted as
`QPainter` text on the GL widget: the latter relies on Qt's per-GL-context glyph
atlas, which does not survive the context recreation ADS triggers on dock reparent
/ **layout restore**, leaving glyphs doubled/garbled while vector fills stayed
correct (the original #214 bug). `drawImage` is a plain textured quad — no glyph
atlas, immune to recreation and DPR changes. The highlight is the one place this touches
the draw pass: `paintGL` pushes the hovered frame to `AxisRenderPass::setHighlightedFrame`,
which luminance-boosts that frame's triad colors. Gated on the triads being
visible; cleared on a camera gesture and on leave. Occlusion is ignored for now
(a frame hidden behind geometry still labels); a one-texel depth-reject is the
planned refinement.

## Mesh shadows

Real-time directional shadows for **meshes only**, from the existing fixed key/"sun"
light (`MeshShadingParams::key_light_dir`). **Casters: meshes only** — URDF/robot
meshes (`RobotModelLayer` visual links) and scene-entity `ModelPrimitive` meshes
(`SceneEntitiesLayer`); point clouds, voxel/occupancy grids, axes, TF triads,
markers, and collision hulls never cast. **Receivers: meshes and the solid grid
floor** (`GridRenderPass` filled-cell mode). Off by default, per-dock
(`MeshShadingParams::shadows_enabled`).

Unlike the screen-space SSAO/EDL post-passes, a shadow map is a **geometry depth
pre-pass** that runs in `paintGL` *before* `renderScene` (between the `FrameContext`
build and the scene render):

```
fit light frustum to mesh-caster bounds  ──►  ShadowMapPass.begin() (depth FBO)
  ──►  each mesh layer renderShadowCasters() (depth-only, from the light)
  ──►  end + rebind scene FBO  ──►  renderScene (receivers PCF-sample the map)
```

- **GL-free core** (`core/shadow_camera.h`, headless-tested). `fitDirectionalShadowCamera`
  builds the orthographic world→light-clip matrix: bounding-**sphere** extents
  (rotation-invariant, no resolution pulsing), padded, and **texel-snapped** along a
  light-only basis so the shadow edge does not crawl as the scene jitters sub-texel.
  `extendAabbToGroundShadow` grows the caster AABB to enclose where the shadow lands
  on `z=0`, so the frustum also covers the **receiving floor** (the floor never casts,
  so it is otherwise absent from caster bounds — and the shadow would fall outside a
  caster-only frustum). `kShadowMapSize` (2048) sizes the map independently of
  `render_scale`.
- **Caster bounds, distinct from `worldBounds()`.** `Scene3DLayer::meshShadowBounds`
  is a separate hook from `worldBounds()` on purpose: `RobotModelLayer` stays
  bounds-less for the **camera** (a moving robot must not yank the view), but the
  shadow frustum *must* enclose the robot mesh. `meshShadowBounds` reports the visual
  caster AABB via `MeshRenderPass::worldBoundsOfDraws` (each resource caches its local
  AABB; lifted by the draw model matrix).
- **Casting.** `Scene3DLayer::renderShadowCasters` (default no-op; overridden by the
  two mesh layers) forwards the visual draws to `MeshRenderPass::renderDepthOnly` — a
  depth-only program (position → light clip, empty fragment) reusing each mesh's
  existing VAO. Collision hulls are not casters (they would double-darken the
  silhouette).
- **Receiving.** The mesh fragment shader and the grid filled-cell shader each project
  the world position into light space and do manual **3×3 PCF** over a plain
  `sampler2D` (the `gl::Texture` wrapper has no compare-mode; the shader guards
  out-of-frustum/beyond-far UVs as *lit*, so the CLAMP_TO_EDGE border can't smear). The
  shadow factor multiplies **only the key-light term** — the camera-locked fill and the
  IBL ambient stay lit, so shadowed surfaces read as shaded, not black. The floor
  darkens toward a floor value (not pure black) so the grid stays legible. A caster-side
  `glPolygonOffset` plus a receiver-side world **normal offset** (sized in shadow
  texels) control acne / peter-panning.
- **Lifecycle / per-dock.** `ShadowMapPass` (depth `GL_DEPTH_COMPONENT32F` FBO) is a
  per-`SceneViewWidget` member released in `releaseGlResources()` and rebuilt lazily —
  the same per-context contract as every pass. Each dock reads its own
  `key_light_dir`, so two docks shadow independently. `shadow_map_id == 0` (feature off,
  or an invalid frustum fit) makes every receiver render fully lit — a safe no-op
  degrade.
- **Demo / verification.** `scene3d_mesh_viewer --shadows on|off` (switches the floor
  to filled cells) renders A/B screenshots; `fixtures/shadow_demo.urdf` is an elevated
  box over the ground. Cost on Iris Xe ≈ 4 ms/frame for a single-caster scene
  (well under the 16.7 ms / 60 Hz budget).

## Camera system

The camera is an interchangeable controller over a shared, serializable pose,
deliberately in `pj_scene3d_core` (Qt/GL-free) so the math is headless unit-
testable (`camera_near_far_test`, `camera_zoom_to_cursor_test`,
`camera_state_transfer_test`, `camera_follow_test`).

- **`ICamera` + `CameraState`** (`core/include/pj_scene3d_core/camera/camera.h`):
  `CameraState` (focal, radius, azimuth, elevation, fov_y, ortho_scale,
  perspective) is the pose every model exports via `state()` and adopts via
  `adoptState()`, so switching models preserves where you were looking.
  `SceneViewWidget` holds a `std::unique_ptr<ICamera>`; `setCameraModel` does
  capture → construct → `adoptState` → swap.
- **Four models, selectable.** `SceneViewWidget::CameraModel` enumerates
  `{ Orbit, XYOrbit, Fly, TopDownOrtho }` — that order is load-bearing because
  the overlay combo's index casts directly to it. Inheritance: `OrbitCamera`
  (perspective spherical orbit, Z-up; non-`final`), `XYOrbitCamera : OrbitCamera`
  (orbit pivot locked to the z=0 ground plane), `TopDownOrthoCamera : ICamera`
  (orthographic bird's-eye, rotatable about +Z — the robotics "2D mode" for
  costmaps/grids), `FlyCamera : ICamera` (first-person free eye + yaw/pitch, no
  orbit pivot: LEFT looks around, pan strafes, wheel dollies forward).
- **Adaptive near/far** (`core/src/camera/camera_math.cpp`, `adaptiveNearFar`):
  `near = max(d·1e-2, 1e-3)` from the working distance only (so close inspection
  never clips), `far = max(d·4, scene_reach)·1.5` (reaches the whole scene), then
  `near = max(near, far/1e5)` — a ratio cap that bounds depth precision.
  `scene_reach` is the scene AABB diagonal plus the focal-to-center distance.
  `TopDownOrthoCamera` uses its own flat-scene guard instead (near/far measured
  against the eye height above the focal plus the scene's vertical extent), so a
  zoomed-in costmap never clips the ground.
- **Zoom-to-cursor** (wheel): a homothety about the world point under the cursor —
  the eye and focal scale toward the cursor target by `pow(0.9, ticks)`, so the
  hovered point stays pixel-locked. A homothety is a uniform scaling, so it cannot
  rotate the orbit: `OrbitCamera::zoomToCursor` keeps azimuth/elevation **untouched**,
  scales the radius (clamped to `[lo, hi]`), and slides the pivot along the cursor
  lever by the *effective post-clamp* factor `new_radius / radius` (so the lock holds
  even when the radius clamps). It deliberately does **not** re-derive the angles from
  `eye − focal` — at large world coordinates that subtraction of two ~1e6 vectors
  loses its low bits to float32 cancellation and spuriously spins the view a fraction
  of a degree on every tick (~1.9°/tick at 5e6). `XYOrbitCamera` re-locks its pivot to
  z=0 the same cancellation-free way (re-pivot along the fixed view ray, never
  `setEyeFocal`). A degenerate ray (no ground/focal-plane hit) falls back to
  center-of-view `zoom()`; right-drag stays center-of-view zoom; on `FlyCamera`
  zoom-to-cursor degenerates to a forward dolly by design.
- **Frame follow (Position-only).** `ICamera::followShift(world_delta)` shifts the
  camera's anchor by a world delta without touching orbit angle / zoom — Orbit and
  TopDownOrtho move the focal, Fly the eye, and `XYOrbitCamera` overrides it to drop
  the z component (ground-locked). `SceneViewWidget` holds the follow target
  (`setFollowFrame`, "" = off) and applies it each tracker tick in `applyFollow`:
  look up the target's origin in the fixed frame at `render_time_`, and shift the
  camera by the delta against the previous tick's origin (`follow_prev_origin_`).
  The first tick after enabling / a fixed-frame change / a lookup gap only *seeds*
  the baseline (no shift), so enabling never jumps and the user's orbit/zoom/pan is
  preserved (follow only *adds* the target's motion). `followRenderKey(time)` hashes
  the followed origin into the dock's `viewRenderKey` so a moving target isn't
  coalesced away by the repaint gate even when the TF overlay is hidden. The UI is
  the "Camera" section's **Follow frame** picker in `Scene3DConfigPanel`, with a
  trailing **recenter** button (`SceneViewWidget::recenterOnFollowFrame` →
  `followShift(target − current focal)`) that snaps the camera onto the target on
  demand. Heading / Pose (rotation-following) modes are future work; the
  `FollowMode` enum already reserves the slot.
- **Large-coordinate precision (camera-relative rendering).** Following a frame in a
  UTM/GPS-scale map parks the camera at ~1e5–1e7 m, where float32 keeps only ~0.1–0.6 m
  of resolution and `view × world-geometry` (subtracting the ~1e6 eye from a ~1e6
  vertex) collapses to cancellation noise — the whole scene visibly *swims* on zoom.
  The scene therefore renders **camera-relative**: each `paintGL` picks a `render_origin`
  (the camera focal, in double), builds the view via
  `ICamera::viewMatrixRelativeTo(render_origin)` (eye/focal offset by the origin in
  double; bit-exact `viewMatrix()` at origin 0, pinned by a test), and threads the origin
  through `FrameContext::render_origin`. `FrameContext::lookup()` subtracts it from every
  resolved transform **in double, before the float downcast** (`toRenderSpace`, in
  `camera_math.h`) — so all TF-placed geometry (every pass/layer), the async hover
  hit-test, and the eye fed to mesh lighting land near the origin and stay precise. The
  camera *state* and *scene bounds* stay in absolute world (follow and near/far need them
  there); only the per-frame render path goes relative. Three passes that don't resolve a
  TF frame take the origin explicitly: the grid (model `= translate(−origin)`; it lives at
  the world origin), TF-connection lines (`buildTfConnectionSegments` takes it), and
  point-cloud **colour-by-axis** — position renders relative for precision, but the colour
  scalar adds the origin back (`u_color_axis_offset`) so a point's colour stays in the
  fixed frame, independent of the camera (the colormap range is lifted by the same offset,
  and algebraically cancels in the shader's normalize). `render_origin == {0,0,0}` (the
  default for hand-built `FrameContext`s in tests/demos) reproduces absolute-world
  rendering verbatim.
- **Frame-list freshness.** The frame pickers (fixed-frame + follow) are fed by
  `SceneViewWidget::refreshAvailableFrames` → `getFrameHierarchy()`, which is
  time-independent (whole buffer). That refresh used to fire only from
  `setTransformBuffer` / `setTrackerTime`, so TF folded into the buffer while the
  playhead was paused (a file load) left the combos stale until the user pressed
  play. `Scene3DDockWidget` now also re-enumerates on `datasetTransformsReady`
  (file ingest), decoupled from the time-gated repaint path, so the full tree is
  listed as soon as it loads.
- **Scene bounds.** `Scene3DLayer::worldBounds()` returns an optional source-frame
  `AABB`; `PointCloudLayer`, `OccupancyGridLayer`, `DepthCloudLayer`, and
  `VoxelGridLayer` override it. `RobotModelLayer` deliberately does **not** (it is
  bounds-less by design — a robot is posed by the live TF tree the camera already
  frames through the other store-backed layers, so adding its links would pull the
  camera around as joints move). `Scene3DDockWidget::updateSceneBounds` unions
  **every** layer's box (`unionAABB` over all layers that report one — there is no
  per-layer visibility filter) and pushes the result to
  `SceneViewWidget::setSceneBounds` → the active camera, feeding the adaptive
  near/far above. No reporting layer → invalid AABB → working-distance fallback.
  (Note: because `RobotModelLayer` reports no bounds, the scene AABB excludes the
  robot mesh — relevant to any future light-frustum fit for shadows.)
- **Overlay UI.** `Scene3DDockWidget` overlays a `camera_model_combo_`
  (`{Orbit, XYOrbit, Fly, Top-down ortho}`) and a `home_button_` (Home icon,
  resets the active model to its default view — not fit-to-scene), anchored flush
  to the top-right edge. The orientation gizmo (`AxisOverlayPass`) sits in the
  bottom-right corner (set in the `SceneViewWidget` ctor) so it no longer crowds
  these controls. They are children of the dock, not the `QOpenGLWidget`, and
  `raise()`'d above it (ADS native-window z-order).
- **Persistence.** `xmlSaveState` writes the active model as a stable string id
  (`orbit` / `xy_orbit` / `fly` / `top_down_ortho` — independent of the enum
  integer / combo order) plus the `CameraState` as JSON (`cameraStateToJson`) and
  the `follow_frame` attribute (empty = off); `xmlLoadState` restores all three,
  sanitizing the state so a corrupt layout can never drive a degenerate view. A
  restored follow target absent from the current TF tree stays inert until it
  appears (`applyFollow` holds on lookup failure).

## Pose-array layer (`PosesInFrame`)

`PosesInFrameLayer` renders a `PJ.PosesInFrame` topic (`geometry_msgs/PoseArray` /
`foxglove.PosesInFrame` equivalent — a flat list of poses in **one** `frame_id`,
not a TF tree) as a per-pose coordinate-axis **triad** gizmo, with per-layer params:
**arrow length** (m), **opacity**, an **X-arrow-only** geometry mode (draw a single
X arrow per pose instead of the triad — useful for dense pose arrays where full
triads clutter), and an orthogonal **color override** (recolor every arm with one
shared color). Geometry and coloring are independent: you can recolor a full triad
or keep a single X arm in its natural red. Defaults (0.15 m, 1.0, off, off) and the
desaturated X/Y/Z colors match the TF "Frames" gizmos.

- **Pure expansion (core, GL-free, unit-tested).** `buildPoseTriadInstances(msg,
  PoseTriadStyle{axis_length, opacity, x_arrow_only, override_color, color})`
  (`core/poses_in_frame_render.{h,cpp}`) turns each pose into three
  `PoseTriadInstance{mat4 model, vec4 color}` arms — `poseToMat4(pose) · arm-rotation ·
  scale(size)`, frame-LOCAL, with `opacity` in the color alpha. The arm rotations
  (+X→+Y / +X→+Z) and colors mirror `renderTriadBound` / `AxisRenderPass`. The style
  struct keeps geometry (`x_arrow_only` → 1 arm vs 3) separate from coloring
  (`override_color` → every produced arm takes the shared `color`, else its natural
  per-axis R/G/B), so an un-overridden X-only arm is still red.
- **GPU-instanced draw.** `PosesRenderPass` (`passes/poses_render_pass.{h,cpp}`) owns
  one unit-arrow mesh (shared with `ArrowGizmo` via `gizmos/arrow_mesh.{h,cpp}`) plus
  an instance VBO (per-instance `mat4 model` at locs 2–5 + `vec4 color` at loc 6,
  divisor 1 — the `MarkerRenderPass` solid-instancing layout). **Key efficiency
  choice:** the per-instance model is frame-LOCAL and the fixed-frame TF transform is
  a `u_frame_world` uniform, so the instance buffer re-uploads only when the sample /
  size / opacity changes — TF and camera motion cost nothing. All `3·N` arms draw in
  one `glDrawElementsInstanced`, so a thousands-of-poses AMCL particle cloud stays one
  draw call. Lit shading + annotation blend (bracketed like the TF axes) keep the look
  identical to the frame gizmos.
- **Ingest.** Mirrors `OccupancyGridLayer`: `attach` bootstraps the source frame;
  `setTrackerTime` defers to `render()`, which decodes `store.latestAt(t)` via a
  **per-use** `parseLocked` binding (never cached — the reload-UAF rule), expands, and
  stages into the pass. A `SequentialUID`-keyed coalescing guard skips redundant
  decodes while scrubbing within one message; a `style_revision_` bump forces the
  current sample to re-expand after any style edit. `render` resolves
  `frame_ctx.lookup(frame_id)` once — an unresolvable frame draws nothing (orphan).
- **Params persistence.** `xmlSaveState`/`xmlLoadState` round-trip `<poses_in_frame
  gizmo_size gizmo_opacity x_arrow_only override_color override_color_value>`, which
  also makes the params copy/paste/apply-to-family-able through `pj_scene_common`'s
  `serializeLayerParams`/`applyLayerParams`. The config widget's "Override color"
  checkbox + always-visible "Color:" swatch reuse `SceneEntitiesLayer`'s marker-recolor
  text and layout (picking a color auto-ticks the box) for cross-layer consistency.
  No host edits.

## Pointcloud layer (`PointCloudLayer`)

`PointCloudLayer` keeps the existing `convertCanonical()` -> `DecodedPointCloud` ->
`CloudVertex` fallback for layouts that need CPU decoding, and adds a narrow zero-copy
fast path for little-endian clouds whose xyz fields are contiguous float32 values. The
fast path uploads the canonical `sdk::PointCloud::data` buffer verbatim and binds the
shader attributes with the point's native stride/offset/type; `FastCloudData` retains the
wire cloud by value so its `BufferAnchor` keeps the bytes alive across GL-context
recreation, letting `releaseGL()`/`initializeGL()` re-upload from the same source just as
the fallback re-uploads from retained decoded vectors.

RGB-direct (`kRgb`) clouds also take the fast path when the cloud carries a packed,
contiguous `rgba`/`rgb` uint32 field (`checkFastPath(..., want_rgba=true)`): the four colour
bytes upload verbatim and bind as a normalized `vec4` straight at the field offset —
pixel-identical to the CPU `convertCanonical(extract_rgba)` path, with no extraction.
Scattered separate `red`/`green`/`blue` channels fall back to the CPU packer.

The geometry AABB (`world_bounds_`) feeds the camera scene-fit every frame, so it must refresh
on every sample. On the bounds-only cases (RGB-direct, solid, spatial-axis, auto-off, non-dirty —
i.e. when no colormap scalar pass is needed) with a 4-byte-aligned fast-path layout, that
reduction runs on the **GPU** rather than the CPU: `PointcloudAabbReducer` dispatches a compute
shader over the already-resident fast-path VBO (no extra upload), reducing min/max via an
order-preserving float→uint key (`aabb_gpu_key.h`) and `atomicMin`/`atomicMax`, and reads the
6-value result back **asynchronously** (fence + non-blocking `poll()` in the next frame's
`render()`). The completed AABB flows to the layer through a bounds callback (`onGpuAabb`) which
updates `world_bounds_` and requests a repaint, so the existing `repaintRequested →
updateSceneBounds` path re-fits the camera. The result lags a frame or two — invisible to the
auto-fit — so the per-sample bounds cost leaves the GUI thread entirely (≈80–170× less GUI-thread
work than the CPU scan at 1–5 M points; see `demos/pointcloud_aabb_benchmark`).

Fallbacks keep the CPU scan: the **first** sample after attach/reset (to seed `world_bounds_`
synchronously and probe compute support), the kField auto-range-dirty case (it needs the scalar
min/max anyway, getting bounds for free in the same pass), a misaligned layout, the decoded
`convertCanonical` path (bounds come free there), and any context without compute (GL < 4.3,
e.g. Windows software GL). The layer only drops the CPU scan once the pass confirms
`gpuAabbAvailable()`, so unsupported drivers degrade safely.

## Depth-cloud layer (`DepthCloudLayer`)

Back-projects a depth image into a 3D point cloud (one point per valid pixel),
colored by depth. Sibling of the 2D depth view — same data, different geometry.

- **No `kDepthImage` producer.** Depth arrives as an `sdk::Image` with a depth
  `encoding`; the parser can't tell depth from color at schema-classification
  time. So `DepthCloudLayer` is registered on `kImage` and the dock **gates** a
  topic by peeking the first sample's `encoding`
  (`isDepthEncoding`: `16UC1` / `32FC1` / `compressedDepth`) — color images are
  refused. Because the two share `kImage`, an empty-placeholder image drop opens
  the **2D** viewer (host policy in `pj_app`); a depth image reaches a 3D dock by
  being dropped onto an existing one or via the 3D family switch.
- **Decode (`toDepthView`).** `16UC1`/`32FC1` alias the image bytes (zero-copy).
  `compressedDepth` is PNG-decoded via `QImage` to a `16UC1` (millimetre) view —
  including a **bare-PNG signature repair**: RealSense bags carry a headerless PNG
  that begins at the `IHDR` chunk, so the 8-byte signature + IHDR length are
  restored before decode (mirrors the 2D path's `toDepthImage`; the matching
  parser-side ConfigHeader handling is `parser_ros`). 32FC1 inverse-quantized
  compressedDepth is not yet handled.
- **Intrinsics by `frame_id`.** A `CameraInfo` is joined to the image by
  **`frame_id`** (authoritative, never the topic name — matches Foxglove's rule /
  Rerun's camera hierarchy). An exact match wins; with none, `resolveIntrinsics`
  falls back to a lone `CameraInfo` only when unambiguous, else refuses rather
  than pair the wrong camera. The result is memoized by `(frame_id, CameraInfo
  SampleId)` and revalidated parse-free via `latestAt`.
- **Back-projection.** The math is the Qt-free core `depth_backproject.{h,cpp}`
  (`depthToPoints`); a single pass yields positions, the per-point depth scalar,
  the world AABB, and the colormap range together (via the optional
  `scalar_out` / `bounds_out` / `scalar_range_out` out-params).
- **Render + color.** The POC feeds the points through the existing
  `PointcloudRenderPass` (a dedicated GPU attributeless / `texelFetch` pass is a
  planned follow-up). Depth is colored through the **shared** `PJ::Colormap`
  (turbo / viridis / plasma / grayscale, `pj_widgets/Colormap.h`) — the same
  source of truth as the 2D depth view and the pointcloud field coloring.
- **Config.** Per-layer colormap, point size, and a min/max depth range.

## Voxel-grid layer (`VoxelGridLayer`)

Renders a dense `sdk::VoxelGrid` (SDK ≥ 0.10.0) — a dense 3D lattice whose
per-voxel value is generic via `fields` (occupancy / cost / ESDF / semantic, or a
direct RGBA channel) — as GPU-instanced cubes. Sibling of the pointcloud layer for
volumetric data.

- **Dense→cubes expansion is entirely GPU-side.** `VoxelGridRenderPass` uploads
  the selected field as a **3D texture** and issues **one** `glDrawElementsInstanced`
  over a unit cube (`column*row*slice` instances). The vertex shader derives each
  voxel from `gl_InstanceID`, `texelFetch`es its value, evaluates the viewer-side
  draw predicate (which the schema does **not** encode), and degenerate-clips culled
  voxels. So the **CPU / draw-call** cost is independent of voxel count (one draw),
  and a re-scrub to a cached grid re-uploads nothing — though the GPU vertex shader
  still runs once per voxel.
- **Qt-free core.** The coordinate/value math (`core/voxel_grid_view.{h,cpp}`,
  `core/voxel_grid_value.{h,cpp}`) is headless unit-tested.
- **Follow-up.** A GPU compute-shader compaction path (`glDrawElementsIndirect` over
  only the accepted voxels, GL ≥ 4.3) is a documented follow-up for very large dense
  grids; not built.

## Repaint coalescing & autoplay

- **Per-tick repaint coalescing.** During playback the scene must not free-run at
  60 Hz redrawing identical frames. `ISceneLayer::renderKey` produces a decode-free
  per-layer fingerprint (sample stamp ⊕ transform), and `SceneDockWidget::onTrackerTime`
  skips the repaint when nothing a layer would draw has changed — holding the
  ≤ 60 Hz / idle→~0 CPU budget the performance goals require (PR #259). `renderKey`
  must derive from index/entry timestamps, **never** from a `latestAt` that would
  decompress cold chunks inside the gate.
- **`--autoplay`.** The app's `--autoplay` CLI flag starts hands-free looped
  playback (PR #260); it doubles as the `PJ_AUTOPLAY` profiling hook used to measure
  the repaint/render cost above.

## URDF / robot-model subsystem

- **Parser** (`widgets/src/urdf_parser.{h,cpp}`): `QDomDocument`-based;
  reads `<link>` visuals/collisions (origin, geometry, material). Geometry is
  the `GeomShape` variant — box/cylinder/sphere primitives **and** meshes.
  `<joint>` is parsed into `RobotModel::joints` (name/type/parent/child/origin),
  but **TF still owns kinematics**: link poses come from the live TF tree. xacro is
  detected (element prefix or filename) and rejected with an actionable error,
  never a cryptic XML failure.
- **Fixed-joint TF bridges** (`core/robot_model_bridges.{h,cpp}`): a URDF may
  contain a frame the data's `/tf` never publishes — classically a gripper rigidly
  mounted to an arm flange, where the mount transform lives only in the URDF (the
  DROID arm-droid dataset is exactly this). `fixedJointStaticTransforms()` turns
  each **fixed** joint into a static (`/tf_static`-style, epoch-stamped)
  `StampedTransform`; `RobotModelLayer` caches them (frame-prefixed) and
  `ensureStaticBridges()` re-asserts them into the dataset `TransformBuffer` every
  tracker tick via `injectMissingStaticTransforms()`. The inject is **guarded**
  (`getParent(child)==nullopt` — never overrides a frame the data publishes),
  **idempotent**, and **self-healing** (a same-file reload that clears the buffer
  re-bridges on the next tick). Once a gripper base is bridged, the movable
  knuckle frames the data *does* publish under it cascade into resolvability on
  their own. Movable-joint articulation (a `JointState` source, `<mimic>`,
  default-to-0) is deferred.
- **Sources.** A `RobotModelLayer` reads its URDF from a store topic
  (`kRobotDescription`, decoded via the topic's parser — payload bytes are
  CDR-framed, never cast to a string), a local file, or an http(s) URL.
  File/URL layers are dock-local: synthetic registry ids that never touch the
  ObjectStore (`Scene3DDockWidget::addRobotModelLayer{,FromUrl}`).
- **Mesh loading** is async (`QtConcurrent::run` → futures polled in
  `render()`); the GL thread never blocks. assimp covers STL/DAE/OBJ/glTF.
  `MeshData` carries UV0 + tangents (`aiProcess_CalcTangentSpace`) and a
  per-`SubMesh` `Material` (glTF 2.0 metallic-roughness, read via assimp's
  material abstraction). Each map (`TextureSource`) is an external file path OR
  inline bytes for embedded glTF/GLB `*N` images (still PNG/JPEG-encoded), keyed
  by content hash; `MeshRenderPass` decodes both, uploads color/emissive sRGB and
  data maps (metallic-roughness/normal/occlusion) linear, and caches per-context
  by key plus texture color space. Sources without PBR factors fall back to
  `MeshShadingParams`.
- **Rendering**: `MeshRenderPass` (metallic-roughness GGX + normal mapping +
  occlusion + emissive + **analytic image-based ambient** — diffuse irradiance
  plus a split-sum specular reflection of a procedural ground→sky environment
  (Karis `envBRDFApprox`, no HDRI cubemap) — lit by a **fixed world key/"sun"
  light** and a dimmer **camera-locked fill headlight**. Metals now reflect the
  sky/ground gradient instead of reading near-black; a true prefiltered-cube IBL
  from an HDRI is still future work) draws meshes by
  key and primitives from unit
  box/cylinder/sphere geometry (URDF cylinder is Z-aligned; sizes ride the
  model matrix). Unresolved meshes render a **magenta unit cube** —
  intentionally ugly, impossible to mistake for data. Visual meshes are split
  into opaque and best-effort translucent draws (layer opacity, override alpha,
  or glTF `BLEND`; no depth sort). Collision geometry with no `<material>` gets
  an **orange tint** (RViz convention) so hulls read as distinct from the visual
  meshes; it renders as a **translucent overlay** (blend on, depth-writes off, so
  the real robot shows through) below full opacity, and as a **solid, occluding**
  hull at opacity ≈ 1.0 (the opaque path: blend off, depth-writes on). Scene-wide
  opacity/visibility comes from `meshShadingParams()`. URDF draw calls are cached
  in camera-relative render space (after `FrameContext::lookup` subtracts the
  current render origin), so `RobotModelLayer` reuses them across pure
  view/projection changes but rebuilds when pan / zoom-to-cursor / follow changes
  the render origin.
- **Time contract**: `RobotModelLayer::timeRange()` returns the inverted
  sentinel `{Timepoint::max(), Timepoint::min()}` — a static decoration must
  neither widen the playback timeline (`{0, INT64_MAX}` would balloon it to
  2262) nor clamp the playhead to its latch timestamp. The dock skips
  inverted ranges and still delivers live tracker time.

## Streaming / live-data path

TF, pointclouds, and markers ingest live as well as from a file.
`TransformService::ingestFrameTransformsForDataset` reads every *new*
`FrameTransforms` message in the dataset into the core `TransformBuffer`
(cursor-based, so repeated calls only fold in what arrived since the last one)
and emits `datasetTransformsReady`. During a **progressive file load** the loader
calls it on every flush, so the buffer fills as the file streams in; the dock
connects `datasetTransformsReady` (in `setTransformService`) and re-renders at the
current playhead via `onTrackerTime`, so a restored 3D scene populates *as the file
loads* instead of only at completion. A non-progressive or already-finished load
runs the same call once at the end. Streaming has no loader to drive that call, so
the dock wires its own incremental path off `samplesIngested`:

- `Scene3DDockWidget::setSessionManager` shadows the base to also call
  `reconnectLiveSamples`, which connects `SessionManager::samplesIngested`
  (fired on the UI thread after each retention trim, `live == true` only while
  following a live stream; file load emits `live == false` and is served by the
  loader-driven `ingestFrameTransformsForDataset` + `datasetTransformsReady` path
  above instead).
- On each live tick, `TransformService::ingestNewTransforms` advances a
  per-topic store cursor and folds only the new `FrameTransforms` into the
  buffer — cheap, and a no-op when nothing new arrived. Without it the TF buffer
  stays empty and every sensor frame is orphaned (red).
- `driveVisibleLayersToLiveEdge` then queries each visible layer's `timeRange()`
  for the newest timestamp now in the `ObjectStore` and drives `setTrackerTime`
  on every visible layer (consulting the *layer's* range, not the store's per
  base-topic range, so a multi-topic layer like OccupancyGrid + its `_updates`
  sibling does not freeze at its last keyframe). Without this, object layers
  would stall while only the TF buffer advanced.

`TransformService` is a `widgets/`-level wrapper over the Qt/GL-free core
`TransformBuffer`. It owns no parser handle: each ingest resolves the topic's
binding through `SessionManager::parserBindingForObjectTopic` and decodes under
`parseLocked` (parse-locked per use — never a cached binding). Streaming uses a
finite `TransformBuffer` cache window (default 10 s) so a growing live stream
trims old samples and stays memory-bounded; the file path constructs the
buffer with eviction disabled, since the whole recording is retained (fed in
incrementally during a progressive load, or in one pass for a finished load).

### Object decode: parser-decoded vs canonical blob

Every object consumer (the layers above + `TransformService`) decodes a store
entry through one seam, `pj_scene3d_widgets/resolve_object.h::resolveObject`,
which picks the path by whether the topic has a bound `MessageParser`:

- **Parser bound** — file / streaming data sources whose objects arrive as
  parser-decoded messages (e.g. `parser_ros`, `parser_protobuf`): decode via the
  topic's parser under `parseLocked` (per-use binding, never cached — the
  reload-UAF rule).
- **No parser bound** — a data-source / toolbox that pushed an *already
  serialized canonical* object (e.g. the Mosaico cloud toolbox, which packages
  the server's `point_cloud2` / `pose` / `motion_state` ontologies into
  `sdk::PointCloud` / `PosesInFrame`): the bytes are a pj_base
  wire blob, so `resolveObject` deserializes them with the canonical codec
  selected by the topic's `builtin_object_type`. This mirrors pj_scene2D's
  `canonical = (binding.parser == nullptr)` discipline.

Either way the **host** performs the decode — the producing plugin never does
(the "canonical-objects-in" boundary; the canonical codec is the canonical
object's own serialization, not a transport/CDR wire format). A layer's
`attach()` therefore accepts a topic that has *either* a parser *or* a canonical
codec for its `builtin_object_type` (`hasCanonical3DCodec`); it rejects only a
topic that has neither.

### SceneEntities lifetime expiry & the decoded-batch cache

`SceneEntitiesLayer` replays every batch up to the tracker time into an id-keyed
entity map (replace-by-id, deletions, lifetime expiry). Two mechanisms support this
on the live path:

- **Dual-clock lifetime anchor.** A `SceneEntity` carries an embedded `timestamp`
  (sensor epoch) and an optional `lifetime_ns`. Under streaming the ObjectStore entry
  is *host*-stamped (tracker clock), a different epoch from the sensor timestamp.
  Expiry must therefore compare against the ingest timestamp, not `entity.timestamp` —
  otherwise every finite-lifetime entity expires the instant it is folded (the bug that
  hid the streamed car mesh). The layer keeps a parallel map `entity_expiry_anchor_ns_`
  (keyed identically to `entities_`) holding each entity's ingest timestamp;
  `expiredAt()` tests `anchor + lifetime < tracker` (overflow-safe; `lifetime == 0` ⇒
  never). The single erase choke-point `eraseEntity()` drops the entity and its anchor
  in lockstep, so the two maps never drift. Deletion gating is intentionally *not*
  re-anchored: deletions carry sensor-epoch timestamps, so
  `deletion.timestamp <= entity.timestamp` stays on the entity clock.
- **Decoded-batch snapshot cache.** Backward scrubs and jumps rebuild the entity map
  from scratch; re-parsing heavy embedded glTF every time hitched the scrub.
  `snapshot_cache_` (keyed by `SequentialUID`, byte-budgeted at `kSnapshotCacheMaxBytes`,
  oldest-UID eviction) holds decoded batches so a rebuild re-folds without re-parsing.
  Each cache entry records `store_ns` (the ingest timestamp) alongside the batch, so a
  cache-hit re-fold restores the *same* lifetime anchor a fresh parse would have produced.

## Asset resolution (`package://` for a non-ROS app)

`UrdfPackageResolver` — URI scheme dispatch first:
`file://` → absolute path; bare path → relative to the URDF's directory
(never enters the package chain); `http(s)://` → allowed only for URL-source
layers; `package://pkg/rel` → the chain below, stop at first hit:

0. **In-band embedded-asset lookup** — exact-name match against the embedded
   asset dictionary (`stepEmbeddedAsset`). The dataset source plugin (not the
   host) is what may carry such assets — e.g. files embedded in the container —
   surfaced format-neutrally via `setEmbeddedAssets`. Built and unit-tested, but
   **not yet connected to the app load path**: `MainWindow::onFileLoaded` does
   not yet surface the extracted asset map, so `setEmbeddedAssets` stays empty in
   production and this step is currently inert. (Steps 1+ — the per-source
   remembered map and auto-seeded search roots — *are* wired: `onFileLoaded` and
   `makeSceneDock` feed `setSourcePath` the loaded source path.)
1. **Remembered per-source mapping** — QSettings
   `pj_scene3d/urdf_per_source_packages`, keyed by the dataset's source path.
2. **Ancestor heuristic** — walk up from the URDF's dir (≤10 levels) looking
   for a directory whose basename == `pkg`, accepting only if
   `candidate/rel` exists (prevents wrong-folder false positives). URL
   sources use the URL-segment variant.
3. **Search roots** — ordered list auto-seeded from the URDF dir, the source
   file's dir, `$ROS_PACKAGE_PATH`, and `$AMENT_PREFIX_PATH`/`$COLCON_PREFIX_PATH`
   (+`/share`), all via `qEnvironmentVariable` — zero ROS dependency.
   Package identity is directory basename only; no `package.xml` check.
4. **Ask once** — unresolved packages surface on the layer's status text;
   a chosen root is stored per-source and globally, then pending meshes retry.

Failure semantics: never silent. Missing meshes → magenta cubes + status
counts; wrong folder picked → explicit "expected a subdirectory named <pkg>"
error; URDF latch not yet received → an indefinite 500 ms-throttled re-check
with a permanent "Waiting for …" status (no timeout escalation).

## Scene-controls panel (pj_app)

`Scene3DConfigPanel` (right side-panel) drives scene-wide state, persisted in
QSettings `pj_scene3d/scene_controls/*` and re-applied to every dock it binds:
grid style/size/divisions/visibility (`GridRenderPass` rebuilds geometry
lazily at render time; style toggles between a plain line grid and a full
two-tone **checkerboard** with the grid lines overlaid — all three colors
auto-derived from the active theme, no user color controls), TF-frame
size/opacity/visibility, mesh/collision
opacity/visibility, and the **Model/URDF** row — a File/Topic/URL source
combo plus an add button; each added robot gets a row with a remove button.
Robot layers are panel-managed: filtered out of the Topics list (the
per-layer config widget — frame prefix, color override, COLLADA up_axis,
resolution override — still exists on the layer but is not reachable from
the panel; resurrect behind an "advanced" disclosure if missed). The
phases-0B/D/B look knobs are runtime APIs with baked defaults — no app UI;
the `scene3d_mesh_viewer` demo exposes them for look-dev.
