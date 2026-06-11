# `pj_scene3D` — Requirements

| | |
|---|---|
| Status | Shipping incrementally (core landed; widgets follow) |
| Date | 2026-05-17 (rev. 2026-05-30) |
| Scope | What, not how |
| Supersedes | `PJ4_PLAN.md` §5.5 (refinement) |

This document is the source of truth for the *intent* of `pj_scene3D`. It deliberately avoids design-level prescriptions (API signatures, class layouts, CMake details, file structures). Those belong in a subsequent design document. When this document conflicts with `PJ4_PLAN.md` §5.5, this document wins.

> **Current architecture (supersedes the Phase-1 framing below).** The module
> ships integrated into `pj_app` (not as a standalone demo), and it **does not
> decode wire formats**. DataSource plugins (e.g. `parser_ros`,
> pj-official-plugins#122) decode ROS/CDR messages into canonical
> `pj_base/builtin` objects (`PointCloud`, `FrameTransforms`, `OccupancyGrid`,
> `OccupancyGridUpdate`) and publish them to the `ObjectStore`; `pj_scene3D`
> consumes those objects from the store on tracker change and renders them.
> Pointclouds, TF, and occupancy grids / costmaps are implemented. The Phase-1
> sections below are retained as historical intent — read "lazy host-side CDR
> decode of `PointCloud2`" as "consume the canonical `PointCloud` object the
> plugin already decoded".

## 1. Purpose & target users

PJ4's 3D visualization module — the sibling family to `pj_scene2D`, focused on the robotics-data subset: TF trees, rigid bodies via URDF, pointclouds, gridmaps, 3D markers and primitives, paths, laserscans.

**Target user**: roboticists who use PlotJuggler 4 to debug recorded or live data alongside their time-series plots, scrubbing back and forth through replay time. Inspirations: RViz2, Foxglove, Rerun.

## 2. Primary use cases

- Scrub recorded MCAP data back and forth through time while seeing the 3D scene react correctly at the same tracker time as 2D widgets and plots.
- Switch the rendering frame (fixed-frame) per widget to view the world from different reference points (`map` vs `odom` vs `base_link`, etc.).
- Run multiple 3D widgets side-by-side, each with its own fixed-frame and view, in the same PJ4 layout.
- Inspect a TF tree and understand frame relationships during debugging.

## 3. Supported data types

**Full v1 target** (six types):
- Rigid bodies (TF + URDF meshes).
- Gridmaps (textured planes in a source frame).
- Pointclouds (`sensor_msgs/PointCloud2` and equivalents).
- Compressed pointclouds (`foxglove_msgs/CompressedPointCloud` + `point_cloud_interfaces/CompressedPointCloud2`), formats **Draco** and **Cloudini** — decoded into a `PointCloud` and rendered identically (see §3a).
- 3D markers / visualization primitives (arrows, boxes, spheres, cylinders, line strips, text).
- Paths (`nav_msgs/Path`, `PosesInFrame`).
- Laserscans (`sensor_msgs/LaserScan`).

**Phase 1 subset** (two types):
- **TF axes** as 3D gizmos — substituting for the URDF mesh path since real mesh assets are not available for the Phase 1 input data. This is a permanent first-class display, not a placeholder.
- **Pointclouds** (`sensor_msgs/PointCloud2`).

Image+Pinhole (camera frustum + textured near-plane) from the original `PJ4_PLAN.md` §5.5 list is dropped from v1.

### 3a. Compressed point clouds (Draco / Cloudini)

A `CompressedPointCloud` canonical object carries `{timestamp, frame_id, format, data}` where
`data` is a self-describing codec blob. `pj_scene3D` decodes it into a `PointCloud` and renders it
through the **same** `PointCloudLayer` and `convertCanonical()` path as a raw cloud — one dual-mode
layer, no separate widget.

- **Formats:** `cloudini` (header-embedded schema; via `cloudini/1.2.2`) and `draco`
  (`draco/1.5.7`). Plain `zstd_point_cloud_transport` is **out of scope** — its blob is not
  self-describing (it relies on layout fields the canonical object does not carry).
- **Wire sources:** the ROS parser emits the canonical object for both
  `foxglove_msgs/CompressedPointCloud` (Foxglove ROS2 schema; its `pose` is read but dropped — clouds
  are placed via TF on `frame_id`) and `point_cloud_interfaces/CompressedPointCloud2`
  (ros-perception/point_cloud_transport). The parser only repackages bytes; it never decodes.
- **Decode location:** `core/pointcloud_codecs.cpp` (`decodeCompressedPointCloud`), `draco` + `cloudini`
  linked PRIVATE to `pj_scene3d_core` — mirrors `pj_scene2d_core` decoding JPEG/PNG.
- **Off the UI thread:** decode is CPU-heavy (Draco ≈100 ms for ~1M points), so it runs on the Qt
  thread pool (`QtConcurrent` + `QFutureWatcher`) with latest-wins coalescing; the decoded cloud is
  cached per sample — identity is (store timestamp, payload byte size) — so repaints and color-field
  changes don't re-decode, and a tracker tick that resolves to the already-pushed sample skips the
  re-conversion/upload entirely. A sample whose decode fails clears the view (matching the raw
  path's malformed-cloud behavior) and is never retried. Fast scrubbing of very large clouds may
  show transient lag (the in-flight sample lands a frame late); async decode + the per-sample cache
  keep the UI responsive.
- **Field fidelity:** Cloudini `INT64`/`UINT64` fields (no PJ datatype) are dropped (their bytes
  still occupy `point_step`, so surviving fields keep their offsets). Draco field names are
  recovered from Draco attribute metadata when present (Foxglove / draco_point_cloud_transport store
  the original name, e.g. `intensity`/`ring`), else inferred from attribute type (POSITION→x/y/z,
  COLOR→red/green/blue/alpha, NORMAL→nx/ny/nz, else `generic_N`).

## 4. Scene composition model

- **All layers are frame-locked, always.** On every render, a layer in source frame F is re-transformed via TF to the widget's fixed-frame at the current tracker time. There is no per-layer opt-out and no world-snapshot semantic (Foxglove's per-layer `frame_locked=false` toggle is intentionally not adopted).
- **No decay or lifetime in v1.** A layer persists until the producer topic emits a new sample on the same key. Accumulation over time is by re-emission, not by hold-and-fade.

## 5. Frame model

**Full v1**: per-widget **fixed-frame + display-frame + follow-mode**, with Foxglove-parity follow modes (Pose / Heading / Position / Off). Each widget independent — two widgets side-by-side can render the same scene against different frames with different cameras.

**Phase 1 subset**: per-widget **fixed-frame dropdown only**, populated from the TF buffer's enumerable frame set. Display-frame, follow-modes, and the RViz-style Frames panel are Phase 2+.

## 6. Camera & interaction

**Phase 1 — RViz Orbit subset**:

| Gesture | Action |
|---|---|
| Left-drag | Rotate around focal point (azimuth + elevation) |
| Middle-drag (or shift+left-drag) | Pan focal point in screen plane |
| Wheel scroll | Zoom *toward focal point* — shrink radius, target fixed (not a dolly) |
| Right-drag | Axis-based zoom (RViz parity) |
| Toolbar reset | Restore default view |

**Coordinate convention**: ROS Z-up (per `PJ4_PLAN.md` §5.5).

**Deferred from v1**: WASD / FPS mode, TopDownOrtho, XY Orbit, bookmark / saved views, animated transitions, `F`-recenter-on-selected-drawable.

## 7. Multi-widget behavior

Multiple `SceneViewWidget` instances cohabit in a PJ4 layout (via the existing ADS docking system). Each widget is fully independent:
- Its own fixed-frame.
- Its own camera state.
- Its own per-display configuration.

No camera sync, no fixed-frame sync, no shared selection. Foxglove-style "sync camera across panels" is explicitly deferred from v1.

## 8. Performance goals (qualitative)

Performance is **benchmark-driven**, not pre-committed at the requirements stage. The pointcloud rendering pipeline in particular is to be discovered through the demo / benchmark phases, modeled on `pj_scene2D`'s mpv-vs-ffmpeg shootout.

**Headline target (post-Phase-1)**: aggregate ~10 million points snappy across N concurrent pointcloud displays in a single PJ4 process, characterized empirically against a stress-test MCAP set.

**Render loop policy** (Phase 1 forward):
- Vsync-capped via `QSurfaceFormat::setSwapInterval(1)` (~60 Hz on standard monitors).
- On-demand repaint: the widget invalidates only on tracker change, slider move, or camera interaction. No free-running redraw.
- Idle → 0 CPU.

This matches the pattern `pj_scene2D` already uses for video playback.

## 9. TF buffer requirements

The TF buffer is the central runtime data structure that makes scrub-replay work for 3D data. Its requirements:

- **Per-dataset**: one TF buffer per loaded dataset. Lifetime owned at the session level by `pj_runtime::SessionManager`.
- **PJ4-native** at the type level: not coupled to ROS message types. DataSource plugins decode wire formats into PJ4 SE(3) samples and push them in.
- **Filled at ingest**, not lazy: as DataSource plugins read TF samples from the source (file or stream), they push directly into the buffer. TF data is small enough that eager decoding fits easily in memory.

### Behavioral contract

Behavioral contract of `TransformBuffer` (`core/include/pj_scene3d_core/tf/tf_buffer.h`):

- **`T_target_from_source` convention** (same as ROS tf2): `lookupTransform(target, source, t)` returns the transform `T` such that `p_target = T * p_source`.
- **Tree, not DAG**: each child frame has exactly one parent. Reparenting attempts are rejected.
- **Zero-order hold (ZOH) sample lookup**, *not* linear interpolation:
  - `t` equal to a stored stamp → that sample.
  - `t` strictly between two stamps → the *earlier* sample (sample-and-hold).
  - `t > latest stamp` → the latest sample (forward extrapolation by hold).
  - `t < earliest stamp` → no result (no backward extrapolation).
  - Static transforms always resolve regardless of `t`.

  This is a deliberate departure from tf2's default linear interpolation. The ZOH semantic answers "what was the system's belief at time *t*?" rather than "what would a smooth interpolation of the recorded data look like at time *t*?". For scrub replay over recorded data, the first question is the correct one — the system never actually held an interpolated pose.

- **Thread-safe**: concurrent writers (ingest thread) and readers (render thread + future Frames panel) must be supported without external locking.
- **Introspection**: callers must be able to enumerate all known frames, query the parent of any frame, and query the latest sample stamp for any edge. This is what the future Frames panel and the per-widget fixed-frame dropdown consume.
- **Non-throwing query variant** for the hot render path. A throwing variant remains available for explicit user code where a missing transform is a programmer error.

## 10. Persistence (deferred from v1)

Saving and restoring per-widget fixed-frame selection, per-display configuration, visibility toggles, and camera position into PJ4 layout files. The schema is deferred to the design phase that follows Phase 1.

## 11. Plugin / extension surface (deferred from v1)

Third-party drawable type registration by plugins. Out of v1 scope. Built-in drawable types only in v1.

## 12. Non-goals (explicit)

- **StatePublisher / click-to-publish** (nav goal, 2D pose estimate, point publishing). PJ4 stays viewer-only; the 3D scene never writes back to the system.
- **Measurement tool / snap-to-object.**
- **Custom grid layer.**
- **Map tile layer** (street, satellite, etc. — Foxglove Pro feature).
- **Hot reload** of plugin-provided drawable types.
- **macOS / Windows support** in v1 (Linux-only per `CLAUDE.md`).
- **Per-layer Foxglove-style `frame_locked` opt-in** — chose always-locked (§4).
- **Decay time / marker lifetime** — chose no-decay (§4).
- **Out-of-core / surveying-scale pointclouds** (>500M points).
- **Costmap-3D, ESDF, octomap, voxel grids** beyond OccupancyGrid.

## 13. Phase 1 acceptance — explicit

Phase 1 ships an **end-to-end MCAP-driven demo binary** that exercises the substrate, the adapted TF buffer, the lazy pointcloud pipeline, and the per-widget UI. It is *not* a substrate-only proof; it is the smallest demo that proves the foundation works on real data.

### Phase 1 input

A ROS2 MCAP file (provided by the user) containing at minimum:
- `tf2_msgs/TFMessage` transforms.
- `sensor_msgs/PointCloud2` pointcloud(s).

### Phase 1 ingest behavior

- **TF**: eagerly decoded at load. Every `TFMessage` sample pushed into the per-dataset TF buffer.
- **PointCloud2**: **lazily decoded on tracker change**, mirroring `pj_scene2D`'s MCAP lazy-media pattern. Raw CDR bytes remain indexed in MCAP at open; decoding occurs on demand through a small LRU cache for recently decoded clouds.

### Phase 1 rendering

- **TF axes**: one XYZ axis triad (red = X, green = Y, blue = Z) per frame in the active dataset's TF buffer. Fixed world-space size (per-widget uniform, e.g., 0.3 m). No name labels. No parent-to-child connecting lines.
- **Origin grid**: a 10×10 grid of 1-metre squares on the ground plane (Z = 0) at the world origin, drawn in a subdued color. Always visible in Phase 1 (no per-widget toggle yet). Renders in the fixed-frame (i.e., translates/rotates with whichever frame the user selects as fixed).
- **Pointcloud**: rendered as `GL_POINTS` (single VBO, no LOD), in its source frame, re-transformed to the widget's fixed-frame each frame via the TF buffer.
- **Pointcloud coloring**: a per-display dropdown selects the source scalar field from `{ X, Y, Z, intensity, ring }`. The dropdown is populated by inspecting the first decoded cloud's `fields` schema (the `timestamp` field is explicitly ignored). The selected field drives a **Turbo colormap**. The colormap range auto-fits to the first cloud's min/max and is held across tracker scrub so colors stay stable.

### Phase 1 UI

- **Per-widget fixed-frame dropdown**: populated from the TF buffer's frame enumeration. Switching re-renders within one vsync.
- **Time slider**: drives the demo's tracker time. Dragging back and forth scrubs correctly:
  - TF axes snap to ZOH positions between samples (no glide).
  - Pointcloud re-decodes from cache or MCAP without visible stall on a cache miss for typical cloud sizes.
- **Render loop**: vsync + on-demand repaint (no free-running redraw).

### Phase 1 acceptance criteria

The demo passes Phase 1 when, on the user-provided MCAP, all of the following hold:

1. Loads without error.
2. TF axes render at the position of each frame, expressed in the currently selected fixed-frame.
3. A 10×10 origin grid (1m squares, ground plane Z=0) renders at the fixed-frame's origin.
3. The fixed-frame dropdown lists every frame present in the MCAP's TF data.
4. The color-field dropdown lists every numeric field present in the first decoded pointcloud (excluding `timestamp`).
5. The slider scrubs through the full time range of the MCAP.
6. Dragging the slider back and forth produces correct ZOH axis snapping and stutter-free pointcloud updates on cache hits.
7. `Scene` is implemented with hardcoded `axes_` and `pointcloud_` members; no premature `IDrawable` polymorphism. (Polymorphism arrives in Phase 2 when the third drawable type lands.)
8. No crashes. `GL_DEBUG_OUTPUT` is clean.
9. Render loop sustains 60 Hz under typical scrub load on integrated graphics; idle CPU is approximately zero.

### Explicitly out of Phase 1

assimp, URDF, tinyply, PCD reader, RGB-direct color mode, additional colormaps beyond Turbo, display-frame, follow-mode, TF Frames panel, picking, measurement, click-to-publish, gridmaps, paths, laserscans, integration into `pj_app` (the Phase 1 demo is a standalone binary, mirroring `pj_scene2D/demos/`).

## 14. Stack (locked)

- **GPU API**: OpenGL 4.5 core via `QOpenGLWidget`.
- **Math**: GLM (column-major, GLSL-matching types).
- **Canonical objects in**: `pj_base/builtin` schemas read from the `ObjectStore`
  (no wire/CDR decoding in this module — see the architecture note at the top).
- **Test framework**: `gtest` (matches `pj_scene2D`).

`pj_scene3D` (OpenGL) and `pj_scene2D` (QRhi) form a mixed GPU stack inside the same PJ4 process. They are sibling widget families that never share GPU state. An `IRenderPass` invariant in the substrate keeps GL calls confined so a future QRhi swap would be a contained refactor rather than a rewrite.

## 15. TF prototype adaptation summary

The TF prototype has been promoted into `pj_scene3D/core/include/pj_scene3d_core/tf/` as the `TransformBuffer` class (`tf/tf_buffer.h`, `tf/transform.h`) — single source of truth, no wrapper layer. The edits applied during promotion were:

1. **`sampleAt` rewritten to zero-order hold** (nearest sample at or before `t`), replacing the prototype's lerp+slerp interpolation. The free-function `interpolate()` becomes unused inside the buffer (may be deleted or kept exposed as a free utility).
2. **`TimePoint` retyped** from `std::chrono::steady_clock::time_point` to a replay-time-friendly type compatible with `PlaybackEngine`'s tracker time. (Final concrete type chosen at integration time.)
3. **Internal `std::shared_mutex`** guarding the buffer: `setTransform` exclusive; `lookupTransform`, `tryLookupTransform`, `canTransform`, `latestCommonTime`, and the introspection getters take shared locks.
4. **Introspection getters**:
   - enumerate all known frames,
   - query the parent of a frame,
   - query the latest sample stamp for a frame's edge.
5. **Non-throwing query variant**: `tryLookupTransform(target, source, t) → PJ::Expected<Transform, LookupError>` — the primary accessor for the render hot path; the typed `LookupError` enum (`UnknownSource`/`UnknownTarget`/`Disconnected`/`NoSampleAtTime`) keeps render-loop misses allocation-free. The throwing `lookupTransform` is a thin wrapper over it.
6. **`setTransform` normalizes the quaternion on insert** to defend against ingest noise accumulating through composed transforms.

**Unchanged** from the prototype:
- The `T_target_from_source` convention.
- Tree-not-DAG enforcement: a reparent conflict (a child already claimed under a different parent) is rejected — `setTransform` returns `PJ::unexpected(SetTransformError::ReparentConflict)` and drops that one edge (non-throwing), so a bulk ingest continues.
- Common-ancestor walk algorithm.
- Per-edge `std::deque` sample storage with 10-second cache window pruning.
- The `Transform` struct (translation + unit quaternion).
