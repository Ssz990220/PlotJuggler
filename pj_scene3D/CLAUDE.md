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

## Layout

- `core/` — pure geometry/scene logic, **no Qt or GL**. The TF buffer + frame
  hierarchy, the SE(3) `Transform`, the `OccupancyGridReconstructor` (stateful
  time-travel over an `OccupancyGrid` base plus incremental `OccupancyGridUpdate`
  patches), and the `DecodedPointCloud` render struct. Links only `glm`,
  `pj_base`, `nlohmann_json`. Key headers:
  `core/include/pj_scene3d_core/{tf/tf_buffer.h, tf/transform.h,
  occupancy_grid_reconstructor.h, pointcloud.h}`.
- `widgets/` — Qt `QOpenGLWidget` viewer, render passes, entities, and
  `Scene3DDockWidget` (an `IDataWidget`). *Landing incrementally.*

# Validation

Before any commit, run the tests and check that they all pass
(`tf_buffer_test`, `tf_buffer_hierarchy_test`, `occupancy_grid_reconstructor_test`).

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
