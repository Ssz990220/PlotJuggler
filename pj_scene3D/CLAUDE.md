# Intro

The purpose of this module is to implement 3D visualization of robotics data
(TF, pointclouds, occupancy grids / costmaps, meshes / URDF robot models, and
scene entities (markers)); paths and laserscans remain future work. Sibling
widget family to `pj_scene2D`.

The detailed set of requirements and goals lives in `pj_scene3D/docs/REQUIREMENTS.md`.
You MUST read this file at the beginning of every section and after compacting.
The as-built design — rendering pipeline (HDR/tonemap/SSAO/EDL), URDF/mesh
subsystem, `package://` asset resolution, scene-controls bindings, camera
system (four pluggable models, adaptive near/far, zoom-to-cursor, XML
persistence), and live-streaming data path — lives in
`pj_scene3D/docs/ARCHITECTURE.md`.

## Decoding boundary (important)

This module **never decodes wire formats**. DataSource plugins (e.g. `parser_ros`,
see pj-official-plugins#122) decode ROS / CDR messages into canonical
`pj_base/builtin` objects — `PointCloud`, `FrameTransforms`, `OccupancyGrid`,
`OccupancyGridUpdate`, … — and publish them to the `ObjectStore`. `pj_scene3D`
*consumes* those canonical objects and renders them. The core therefore stays
"canonical-objects-in, render-structs-out", with **no `nanocdr` / CDR dependency**.

**One carve-out — compressed point clouds.** A `CompressedPointCloud` is *already* a
canonical object, but its payload is a self-describing codec blob (Draco / Cloudini).
Turning it into a canonical `PointCloud` is canonical→canonical *transcoding*, not
transport/CDR parsing, so it lives here: `core/pointcloud_codecs.{h,cpp}`
(`decodeCompressedPointCloud()`), with `draco` + `cloudini` linked **PRIVATE** into
`pj_scene3d_core` — the same shape as `pj_scene2d_core` decoding JPEG/PNG. The decode is
CPU-heavy, so `PointCloudLayer` runs it on the Qt thread pool (`QtConcurrent` +
`QFutureWatcher`, latest-wins coalescing) and never blocks the UI; the decoded
`PointCloud` then flows through the **same** `convertCanonical()` path as a raw cloud.
Notes: Cloudini `INT64`/`UINT64` fields have no PJ datatype and are dropped; Draco field
names are recovered from Draco attribute metadata when present (Foxglove /
`draco_point_cloud_transport` store the original name there), else inferred from the
attribute type; plain `zstd_point_cloud_transport` is out of scope (its blob isn't
self-describing). This is the *only* codec in the module — the no-`nanocdr`/CDR rule
still holds for everything else.

## Layout

- `core/` — pure geometry/scene logic, **no Qt or GL**. The TF buffer + frame
  hierarchy, the SE(3) `Transform`, the `OccupancyGridReconstructor` (stateful
  time-travel over an `OccupancyGrid` base plus incremental `OccupancyGridUpdate`
  patches), and the `DecodedPointCloud` render struct. Links only `glm`,
  `pj_base`, `nlohmann_json`. Key headers:
  `core/include/pj_scene3d_core/{tf/tf_buffer.h, tf/transform.h,
  occupancy_grid_reconstructor.h, pointcloud.h, pointcloud_codecs.h,
  camera/camera.h, camera/camera_math.h, robot_model.h,
  scene_entities_decode.h, scene_entities_render.h}`. The codec
  decoders add a PRIVATE `draco` + `cloudini` link (compressed-cloud transcoding only —
  see "Decoding boundary"); the public API stays `glm` / `pj_base` / `nlohmann_json`.
- `widgets/` — Qt viewer (`SceneViewWidget`, a `QOpenGLWidget` embedded as a
  direct child of `Scene3DDockWidget`), render passes, layers, and
  `Scene3DDockWidget` (an `IDataWidget`). Key public headers:
  `widgets/include/pj_scene3d_widgets/{Scene3DDockWidget.h,
  transform_service.h, scene3d_layer.h, mesh_data.h, mesh_shading_params.h,
  passes/mesh_render_pass.h, layers/robot_model_layer.h,
  layers/scene_entities_layer.h}`. A right-click on the view delivers a
  native `QContextMenuEvent` that the host `DockWidget`'s event filter catches,
  so the 3D scene gets the same standard menu (Split Horizontally/Vertically,
  Clear) as other widgets with no view-side context-menu code. *Landing
  incrementally.*
  - **GL context lifecycle (don't regress this):** the app deliberately does NOT
    set `Qt::AA_ShareOpenGLContexts` (see `pj_app/src/main.cpp`) — a process-wide
    share group let one 3D view's teardown corrupt sibling views' VAO/FBO state
    (a `glBindVertexArray(non-gen name)` flood + the map texture vanishing). Each
    view's GL context is therefore independent, which means a `QOpenGLWidget`
    *recreates* its context when ADS reparents the dock (dock/float/split). VAOs
    and FBOs are per-context (never shared). Mesh textures are also
    per-context: `MeshRenderPass::releaseGL()` clears its `Texture2D` cache under
    the dying context and lazily reuploads path-keyed or embedded textures after
    recreation.
    Every `IRenderPass`/`Scene3DLayer` (and `ArrowGizmo`) implements
    `releaseGL()`; `SceneViewWidget` calls it from the dying context's
    `aboutToBeDestroyed` and rebuilds in `initializeGL`, so a recreated context
    self-heals instead of binding stale handles or going blank. Since Phase 0A
    the scene renders into `SceneHdrFbo` (a multisample RGBA16F+DEPTH32F chain
    at the backing FBO's achieved sample count, resolved to single-sample and
    presented via a fullscreen passthrough into `defaultFramebufferObject()`).
    The chain and the present program are per-context like everything else:
    released in `releaseGlResources()`, lazily rebuilt after recreation.
  - Scene-wide look controls live on `SceneViewWidget` setters +
    `CompositeParams` + `mesh_shading_params.h`. `pj_app`'s `Scene3DConfigPanel`
    drives grid style/size/divisions/visibility, gizmo size/opacity/visibility,
    mesh/collision opacity/visibility, and the Model/URDF selector (persisted in
    QSettings under `pj_scene3d/scene_controls/*`). The phases-0B/D/B knobs
    (tonemap/exposure/saturation/SSAO/EDL) are runtime APIs with baked defaults
    — no app UI; the mesh_viewer demo exposes them for look-dev.
  - `MeshData` carries per-vertex UV0 + tangents plus a per-`SubMesh` `Material`
    (glTF 2.0 metallic-roughness, read via assimp's material abstraction so it
    also covers DAE/OBJ/FBX): base-color/metallic-roughness/normal/occlusion/
    emissive maps + factors + alpha mode. Each `TextureSource` is EITHER an
    external file path OR inline bytes (embedded glTF/GLB `*N` images, still
    PNG/JPEG-encoded), keyed by content hash. `MeshRenderPass` decodes both
    (`QImage::fromData` for embedded), uploads color/emissive sRGB and data maps
    (MR/normal/AO) linear, caches per-context by key plus texture color space,
    and does full metalness-workflow shading + normal mapping + emissive. Visual
    draws are split into opaque and best-effort translucent buckets (layer
    opacity, override alpha, or glTF `BLEND`; no depth sort). Sources
    without PBR (STL, procedural primitives) fall back to the scene-wide
    `MeshShadingParams`.
    Only compressed embedded images are supported (raw-RGBA `mHeight>0`, rare, is
    skipped). Environment/IBL reflection is still out of scope — metals get
    specular highlights but no environment reflection.

# Validation

Before any commit, run the tests and check that they all pass
(`tf_buffer_test`, `tf_buffer_hierarchy_test`, `occupancy_grid_reconstructor_test`,
`occupancy_grid_bounds_test`, `occupancy_grid_layer_rebind_test`,
`occupancy_grid_layer_updates_test`,
`scene_entities_decode_test`, `pointcloud_codecs_test`,
`pointcloud_layer_cache_test`, `pointcloud_layer_rebind_test`,
`pointcloud_layer_coalescing_test`, `camera_near_far_test`,
`camera_zoom_to_cursor_test`, `camera_state_transfer_test`,
`urdf_parser_test`, `urdf_package_resolver_test`, `mesh_loader_test`,
`robot_model_layer_test`, `scene_entities_layer_model_test`).

Make sure that all the markdown files in this folder are updated, if necessary.

Lessons learned should be saved too, in particular after long debugging
sections where we struggle to find the correct solution.

# Test-driven verification

- Always think first about how a certain piece of software can be tested
  automatically, instead of asking the user to run it and report the results.
- If the user reports an issue, think first about how to reproduce the issue
  in the tests. Do not attempt to fix the issue unless we were able to
  reproduce it.

# Collaboration model

Claude implements directly (design, code, review, verification) and surfaces
diffs for user-approved commits. **Codex delegation was discontinued by the
User on 2026-06-09** — do not dispatch implementation work to Codex for this
module.
