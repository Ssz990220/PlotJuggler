# Intro

The purpose of this module is to implement 3D visualization of robotics data
(TF, pointclouds, occupancy grids / costmaps, and over time meshes, markers,
paths, laserscans), as a sibling widget family to `pj_scene2D`.

The detailed set of requirements and goals lives in `pj_scene3D/docs/REQUIREMENTS.md`.
You MUST read this file at the beginning of every section and after compacting.

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
  occupancy_grid_reconstructor.h, pointcloud.h, pointcloud_codecs.h}`. The codec
  decoders add a PRIVATE `draco` + `cloudini` link (compressed-cloud transcoding only —
  see "Decoding boundary"); the public API stays `glm` / `pj_base` / `nlohmann_json`.
- `widgets/` — Qt viewer (`SceneViewWidget`, a `QOpenGLWidget` embedded as a
  direct child of `Scene3DDockWidget`), render passes, layers, and
  `Scene3DDockWidget` (an `IDataWidget`). A right-click on the view delivers a
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
    and FBOs are per-context (never shared), so every `IRenderPass`/`Scene3DLayer`
    (and `ArrowGizmo`) implements `releaseGL()`; `SceneViewWidget` calls it from
    the dying context's `aboutToBeDestroyed` and rebuilds in `initializeGL`, so a
    recreated context self-heals instead of binding stale handles or going blank.

# Validation

Before any commit, run the tests and check that they all pass
(`tf_buffer_test`, `tf_buffer_hierarchy_test`, `occupancy_grid_reconstructor_test`,
`occupancy_grid_bounds_test`, `occupancy_grid_layer_rebind_test`,
`scene_entities_decode_test`, `pointcloud_codecs_test`, `camera_near_far_test`,
`camera_zoom_to_cursor_test`, `camera_state_transfer_test`).

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

For this module, **Codex writes the implementation code**; Claude is the
lead engineer + project manager (drafts the design + Codex prompts, reviews
every Codex deliverable, surfaces diffs for user-approved commits).
