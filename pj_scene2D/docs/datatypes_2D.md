# Scene Types — Unified Type Catalog

## Implementation status (as of 2026-06-10)

This document is the **design catalogue** for canonical scene types.
Canonical types are realised as C++ structs under
`plotjuggler_sdk/pj_base/include/pj_base/builtin/` (re-exported as
`PJ::sdk::` types through `pj_plugin_sdk`). Not every type listed below
has shipped yet:

| Type | Status | Header (if realised) |
|---|---|---|
| `FrameTransform` (§3) | ✅ Realised | `pj_base/builtin/frame_transforms.hpp` + `frame_transforms_codec.hpp` |
| `Image` (§4) | ✅ Realised | `pj_base/builtin/image.hpp` |
| `DepthImage` (§5) | ✅ Realised | `pj_base/builtin/depth_image.hpp` + `depth_image_utils.hpp` |
| `SegmentationImage` (§6) | 🟡 Designed, not yet realised | — |
| `ClassRegistry` (§7) | 🟡 Designed, not yet realised | — (likely lives next to SegmentationImage) |
| `VideoFrame` (§8) | ✅ Realised + wired | `pj_base/builtin/video_frame.hpp` + `video_frame_codec.hpp` (wire layout matches Foxglove `CompressedVideo`: `timestamp=1, frame_id=2, data=3, format=4`). Loaded as a canonical object: parser_protobuf classifies `PJ.VideoFrame` / `foxglove.CompressedVideo`, parser_ros classifies `foxglove_msgs/CompressedVideo` → `kVideoFrame` ObjectTopic → rendered by `StreamingVideoSource` (parser-mode: unwraps each entry's message to its raw NAL span zero-copy, then `StreamingVideoDecoder`). |
| `CameraCalibration` (§9, realised as `sdk::CameraInfo`) | ✅ Realised | `pj_base/builtin/camera_info.hpp` + `camera_info_codec.hpp` |
| `PointCloud` (§10) | ✅ Realised | `pj_base/builtin/point_cloud.hpp` (+ `compressed_point_cloud.hpp` / `_codec.hpp` for the compressed variant) |
| `ScenePrimitive` variants (§11) | ✅ Realised — in a different shape | `pj_base/builtin/scene_entities.hpp` + `scene_entities_codec.hpp`. Shipped as `sdk::SceneEntities` (`kSceneEntities = 11`, wire schema `PJ.SceneEntities`): a Foxglove-style entity model with typed primitive lists per `SceneEntity` (arrow/cube/sphere/cylinder/line/triangle/text/axes/model), not §11's common-header + variant-payload design — treat §11 as design history. pj_scene2D projects the supported 2D subset via `scene_entities_2d_decoder`: arrows, cubes, spheres, lines, triangles, texts, and axes; cylinders and models are not projected. |
| `Grid` (§13) | ✅ Realised — narrowed | `pj_base/builtin/occupancy_grid.hpp` + `occupancy_grid_codec.hpp` (`sdk::OccupancyGrid`, square cells via `resolution`) plus incremental patches in `occupancy_grid_update.hpp` (`kOccupancyGridUpdate`). The generic multi-channel Grid of §13 remains design-only |
| `ImageAnnotations` (not its own § here, but listed in `pj_base/builtin/builtin_object.hpp` as `kImageAnnotations`) | ✅ Realised | `pj_base/builtin/image_annotations.hpp` + `image_annotations_codec.hpp` |
| `RobotDescription` (not in this catalogue) | ✅ Realised | `pj_base/builtin/robot_description.hpp` |

Types marked 🟡 are spec-level only — there is no struct, no codec, and
no test coverage yet. The schemas below describe the **intent**; once a
type ships, this table is the place to record that and link to its
header. Treat the rest of the document as the source-of-truth contract
for both realised and unrealised types.

---

## 1. Design Principles

### Separate storage backends

The existing columnar store remains unchanged for plottable time-series (scalar fields
over time). This document defines new types for 2D and 3D scene data. Each topic belongs
to exactly one storage kind; the two stores share the same timestamp, dataset, and topic
identity systems.

### Unify where the difference is just an encoding

When two messages differ only in *how their bytes are parsed* — same decoded
semantic, same consumption rules — they are one type with an encoding field. Raw
`rgb8` and `jpeg` both decode to color pixels and live in `Image` together; the
encoding string is the only thing that distinguishes them. There is no separate
`CompressedImage` type.

### Split where the *decoded value* has a different meaning

Two messages with different decoded semantics belong to different families, even
when their byte layout is identical. A `mono16` greyscale photo (luminance) and a
`16UC1` depth map (metric distance) share a byte layout but mean different things —
they live in `Image` and `DepthImage` respectively. The same reasoning splits
`SegmentationImage` (categorical class IDs) from `Image`. Encoding is a parsing
detail; semantics are a type boundary.

### Split where the *consumption rules* differ

`VideoFrame` is split from `Image` not because the decoded result differs (it's
still color pixels) but because each message is **not self-contained**: delta frames
depend on a preceding keyframe and intervening frames. That changes seek, eviction,
and decoder lifecycle in ways no encoding field can paper over.

### 2D is a specialization of 3D

For scene primitives (markers, lines, meshes), the 2D variant is a 3D primitive with
z = 0. There is no separate 2D marker system.

Image annotations (pixel-space overlays) are a genuine separate type because they operate
in image pixel coordinates, not world coordinates.

### TF-style transform model

Transforms use explicit `parent_frame_id` / `child_frame_id` strings, following the
ROS TF convention. This matches PlotJuggler's robotics audience and avoids coupling
the transform graph to any path/entity hierarchy.

---

## 2. Common Building Blocks

These are not standalone types — they are embedded fields within the types below.

| Name | Fields | Notes |
|------|--------|-------|
| `Point3` | `double x, y, z` | Position in 3D. For 2D, z = 0. |
| `Vector3` | `double x, y, z` | Non-positional 3D vector (size, scale, velocity). |
| `Quaternion` | `double x, y, z, w` | Unit quaternion (Hamilton convention). |
| `Pose` | `Point3 position, Quaternion orientation` | Rigid body transform. |
| `Color` | `uint8 r, g, b, a` | RGBA color, [0..255]. |

---

## 3. Transforms

### FrameTransform

Defines a single parent-to-child coordinate frame relationship.

| Field | Type | Notes |
|-------|------|-------|
| `timestamp` | `int64` | Nanoseconds since epoch. |
| `parent_frame_id` | `string` | Parent frame name. |
| `child_frame_id` | `string` | Child frame name. |
| `translation` | `Point3` | Child origin in parent frame. |
| `rotation` | `Quaternion` | Child orientation in parent frame. |

Multiple FrameTransform entries over time form a transform tree. The viewer resolves
chains to compute world-frame positions.

Batch ingest: a single message may carry an array of FrameTransform (the full tree
snapshot at one timestamp), analogous to Foxglove's `FrameTransforms`.

---

## 4. Image

A self-contained 2D color or greyscale image: raw pixels or single-frame compressed
formats. Every Image message is independently displayable — no decoder state carries
across messages.

| Field | Type | Notes |
|-------|------|-------|
| `timestamp` | `int64` | Nanoseconds since epoch. |
| `frame_id` | `string` | Coordinate frame of the camera. |
| `width` | `uint32` | Image width in pixels. |
| `height` | `uint32` | Image height in pixels. |
| `encoding` | `string` | Pixel format or compression codec (see table). |
| `step` | `uint32` | Row stride in bytes. 0 for compressed formats. See "Row padding". |
| `data` | `bytes` | Pixel data (raw) or compressed bitstream. |

### Encoding values

| Category | Encoding strings | Description |
|----------|-----------------|-------------|
| Raw color | `rgb8`, `rgba8`, `bgr8`, `bgra8` | Row-major, interleaved. `step` required. |
| Raw greyscale | `mono8`, `mono16` | Single channel. `mono16` is luminance, **not** depth (use `DepthImage`). |
| Raw Bayer | `bayer_rggb8`, `bayer_grbg8`, `bayer_gbrg8`, `bayer_bggr8` | Single-channel CFA mosaic (name = top-left 2×2 tile). Demosaiced to `rgb8` on decode. |
| Compressed | `jpeg`, `png` | Single-frame compressed image. `step = 0`. Width and height must still be set. (`webp` is reserved — no built-in decoder yet.) |

A raw or Bayer buffer may also arrive wrapped in an 8-bit grayscale container —
PNG/JPEG (the flat `step × height` bytes reshaped as a grayscale image of
`width = step`). PNG wrapping is lossless; JPEG wrapping is lossy and only
approximates the original samples (not byte-exact). The decoder detects the
container signature, recovers the flat bytes, then reinterprets at the logical
geometry; if the wrapped decode fails it falls back to treating the bytes as raw. The `encoding` field still names
the logical pixel layout, not the container.

The `encoding` string is open — implementations may add new values without a schema
revision. Keep encoding strings conventional where one exists in ROS, Foxglove, or
the sensor SDK.

### Row padding (`step`)

`step` is the row stride in bytes. For tight-packed rows it equals
`width * bytes_per_pixel` and is redundant. It exists to support rows with trailing
padding — common in GPU readbacks, hardware-decoded video planes, and V4L2 camera
buffers, which align row strides to 16, 32, 64, or 256 bytes for SIMD, DMA, or
texture-upload constraints.

In practice, most messages on the wire are tight-packed (producers copy out of
padded buffers before publishing). Keeping `step` enables zero-copy ingest from a
padded source when needed; the cost is four bytes per message.
`step >= width * bytes_per_pixel` is the only invariant. For compressed encodings
there are no rows, so `step = 0`. The same field appears on `DepthImage` and
`SegmentationImage` with the same semantics.

### Multi-layer compositing across image families

A camera view in PJ can composite multiple image-family topics as layers (Image
base, DepthImage colormap, SegmentationImage mask, ImageAnnotation overlay). Layer
ordering and rendering mode (direct color, colormap, false-color with transparency)
are viewer/UI configuration — not encoded in the data model.

Layers are associated by `frame_id`: image-family topics sharing the same camera
frame can be composited. ImageAnnotation references its base image explicitly via
`image_topic`.

Example topics for a single camera:

| Topic | Type | Notes |
|-------|------|-------|
| `/camera/image` | Image | Base RGB feed |
| `/camera/depth` | DepthImage | Depth map, viewer colormaps it |
| `/camera/segmentation` | SegmentationImage | Categorical class IDs, viewer applies false colors via ClassRegistry |
| `/camera/annotations` | ImageAnnotation | Bounding boxes, labels |

Video is **not** an Image — see VideoFrame (§8).

---

## 5. DepthImage

A self-contained 2D depth map. Pixel values represent metric distance from the
camera optical center, in meters, after applying `depth_scale`:

```
distance_meters = pixel_value * depth_scale
```

| Field | Type | Notes |
|-------|------|-------|
| `timestamp` | `int64` | Nanoseconds since epoch. |
| `frame_id` | `string` | Coordinate frame of the camera. |
| `width` | `uint32` | Image width in pixels. |
| `height` | `uint32` | Image height in pixels. |
| `encoding` | `string` | Depth pixel format (see table). |
| `depth_scale` | `double` | **Required.** Meters per pixel unit. |
| `step` | `uint32` | Row stride in bytes. 0 for compressed formats. |
| `data` | `bytes` | Depth pixel data. |

`depth_scale` is required, not optional. The whole reason `DepthImage` exists as a
separate family is to make the metric interpretation explicit; omitting the scale
would defeat the split.

### Encoding values

| Encoding | Pixel layout | Typical `depth_scale` |
|----------|--------------|----------------------|
| `16UC1` | uint16, raw | `0.001` (millimeters) |
| `32FC1` | float32, raw | `1.0` (meters) |
| `compressedDepth` | ROS-style: 12-byte header + PNG-encoded 16-bit depth | `0.001` (after decode) |

Other depth encodings may be introduced — for example sensor-specific codecs from
RealSense or OAK-D. Keep the encoding string conventional where one exists.

### Convention: zero means invalid

By convention, `pixel_value == 0` denotes "no depth measurement at this pixel" (out
of range, low confidence, occluded). This schema does not mandate the convention but
recommends following it for compatibility with ROS, RealSense, Open3D, and Foxglove
consumers.

### Conversion to PointCloud

Depth + CameraCalibration can be converted to a PointCloud (derived transform or at
ingest time).

### Out of scope: encoded depth video

Streaming `mono16` depth as H.264 (e.g. RealSense over RTSP) is a real but
specialized case that fits neither DepthImage (no GOP support) nor VideoFrame (no
`depth_scale`). Deferred until demand justifies a schema change.

---

## 6. SegmentationImage

A self-contained 2D mask where each pixel value is a categorical class ID. The
mapping from class ID to a human-readable label and a default color lives in a
separate `ClassRegistry` (§7).

| Field | Type | Notes |
|-------|------|-------|
| `timestamp` | `int64` | Nanoseconds since epoch. |
| `frame_id` | `string` | Coordinate frame of the camera. |
| `width` | `uint32` | Image width in pixels. |
| `height` | `uint32` | Image height in pixels. |
| `encoding` | `string` | Class-ID pixel format (see table). |
| `step` | `uint32` | Row stride in bytes. 0 for compressed formats. |
| `class_registry_id` | `string` | Identifier of the associated ClassRegistry. May be empty when the registry is bound out-of-band (dataset metadata). |
| `data` | `bytes` | Class-ID pixel data. |

### Encoding values

| Encoding | Pixel layout | Class capacity |
|----------|--------------|---------------|
| `mono8` | uint8, raw | 256 classes |
| `mono16` | uint16, raw | 65 536 classes |
| `png` | PNG-compressed integer image | matches underlying bit depth |

Compressed encodings are useful because masks compress dramatically (large flat
regions of the same class). PNG with palette mode is a particularly good fit.

### Why not a regular Image?

A class ID is neither a color nor a luminance — it is a categorical label. By the
family criterion (§1 "Split where the decoded value has a different meaning"), that
is a different decoded semantic, so it gets its own family. Treating segmentation as
a `mono8` Image and "applying false color in the viewer" loses the categorical
nature of the data: viewers cannot reliably know they are looking at labels rather
than greyscale.

### Instance and panoptic segmentation

This type covers semantic segmentation (one class ID per pixel). Instance
segmentation (per-pixel instance ID) and panoptic segmentation (combined class +
instance) are deferred — they can be expressed as multiple SegmentationImage topics
with disjoint registries, or as a future schema extension.

---

## 7. ClassRegistry

A class-ID lookup table referenced by one or more SegmentationImage topics.
Registries are typically published infrequently (once per session or once per
dataset) and re-published only when the class set changes.

| Field | Type | Notes |
|-------|------|-------|
| `timestamp` | `int64` | Nanoseconds since epoch. |
| `id` | `string` | Stable identifier. SegmentationImage's `class_registry_id` references this. |
| `entries` | `ClassEntry[]` | One entry per class. |

`ClassEntry`:

| Field | Type | Notes |
|-------|------|-------|
| `class_id` | `uint32` | Pixel value in the segmentation mask. |
| `name` | `string` | Human-readable label (e.g., `"pedestrian"`). |
| `color` | `Color` | Default color for visualization. May be ignored by viewers using their own palette. |

Multiple registries may coexist with different IDs; each segmentation topic
declares which one applies.

---

## 8. VideoFrame

A single encoded video frame (one NAL unit group for H.264/H.265, one set of OBUs
for AV1). Unlike Image, video frames are **not self-contained**: delta frames (P-frames)
depend on a preceding keyframe (I-frame) and all intermediate frames for decoding.

This fundamental difference drives separate storage and query semantics:

- **Seeking**: latest-at must find the nearest preceding keyframe, then decode forward.
- **Eviction**: cannot evict a keyframe without invalidating its dependent delta frames.
- **Decoder lifecycle**: stateful — the consumer maintains a codec decoder across frames.
- **Joining mid-stream**: consumer must wait for the next keyframe before displaying.

| Field | Type | Notes |
|-------|------|-------|
| `timestamp_ns` | `int64` | Presentation timestamp (nanoseconds since epoch). See "Timestamp semantics". |
| `frame_id` | `string` | Coordinate frame of the camera. |
| `format` | `string` | `h264`, `h265`, `av1`, `vp9`. |
| `data` | `bytes` | Encoded frame data (see codec rules below). |

The realised struct also carries a `BufferAnchor anchor` for zero-copy lifetime.

Width and height are not in the schema — they live in the bitstream (SPS/VPS for
H.264/H.265, Sequence Header OBU for AV1, frame header for VP9) and are read by the
decoder.

### Codec rules

**Recommended: I+P only (no B-frames).** This guarantees presentation order
equals decode order, simplifying seeking, eviction, and mid-stream join.
Foxglove and Rerun follow this convention. New streaming sources (ROS 2,
RTSP) should encode with `-bf 0` to avoid B-frames.

**B-frames are tolerated** when ingesting from container formats (MP4, MKV,
LeRobot datasets) that use them. The decoder handles reordering internally
via FFmpeg's reorder buffer. B-frames add startup latency (reorder buffer
fill time: ~30–100 frames depending on encoder settings) and complicate
seeking, so I+P is preferred for interactive use.

Each message must contain exactly enough data to decode **exactly one frame**.

| Codec | Keyframe `data` must include | Bitstream format |
|-------|------------------------------|------------------|
| `h264` | SPS + PPS NAL units before IDR slice | Annex B (start codes `00 00 00 01`) |
| `h265` | VPS + SPS + PPS NAL units before IDR/IRAP slice | Annex B |
| `av1` | Sequence Header OBU before KEY_FRAME OBU | Low overhead bitstream (spec section 5.2) |
| `vp9` | (implicit in keyframe) | Native |

Keyframes are self-contained: a decoder can be fully initialized from any keyframe
message without prior state. Delta frames require the decoder to have processed
the preceding keyframe and all intermediate frames.

### Timestamp semantics (PTS vs DTS)

- **I+P only (no B-frames)**: PTS == DTS. Use either as the ObjectStore
  entry timestamp.
- **With B-frames**: PTS (presentation order) is non-monotonic. DTS
  (decode order) is always monotonic. The DataSource must use **DTS** as
  the ObjectStore entry timestamp to satisfy monotonicity. The decoded
  output's `AVFrame::pts` gives the correct presentation timestamp for
  display and timeline synchronization.

### No explicit keyframe flag

Following both Rerun and Foxglove, there is no `is_keyframe` boolean in the schema.
Keyframe detection is determined by parsing the bitstream (NAL unit type for H.264/H.265,
OBU type for AV1, frame header for VP9). This avoids the risk of the flag contradicting
the actual bitstream content.

### Why not unified with Image?

| Concern | Image | VideoFrame |
|---------|-------|------------|
| Self-contained per message? | Yes | No (delta frames need prior state) |
| Latest-at query | Return one message, display it | Find keyframe, decode forward |
| Eviction | Any message independently | GOP-aware (keyframe protects dependents) |
| Consumer decoder state | None (stateless) | Persistent codec decoder |
| Width/height known? | Always (in schema) | Only from bitstream (SPS/VPS) |

---

## 9. Camera Calibration

| Field | Type | Notes |
|-------|------|-------|
| `timestamp` | `int64` | Nanoseconds since epoch. |
| `frame_id` | `string` | Camera coordinate frame. |
| `width` | `uint32` | Image width in pixels. |
| `height` | `uint32` | Image height in pixels. |
| `distortion_model` | `string` | `plumb_bob`, `rational_polynomial`, `kannala_brandt`, `fisheye62`. |
| `D` | `double[]` | Distortion coefficients. |
| `K` | `double[9]` | 3x3 intrinsic matrix (row-major). |
| `R` | `double[9]` | 3x3 rectification matrix (row-major). |
| `P` | `double[12]` | 3x4 projection matrix (row-major). |

Field-for-field compatible with ROS `sensor_msgs/CameraInfo`. Applies to all four
image-family types when projection between image-pixel space and world coordinates
is needed.

Realised as `PJ::sdk::CameraInfo` (`pj_base/builtin/camera_info.hpp`), field-for-field
as documented; enum slot `BuiltinObjectType::kCameraInfo = 14`, wire schema `PJ.CameraInfo`.

---

## 10. Point Cloud

| Field | Type | Notes |
|-------|------|-------|
| `timestamp` | `int64` | Nanoseconds since epoch. |
| `frame_id` | `string` | Coordinate frame. |
| `pose` | `Pose` | Origin of the cloud relative to `frame_id`. |
| `point_stride` | `uint32` | Bytes per point in `data`. |
| `fields` | `FieldDescriptor[]` | Describes each channel (name, offset, type). |
| `data` | `bytes` | Packed point data. |

FieldDescriptor: `{ string name, uint32 offset, uint8 type }`.
Type enum: UINT8, UINT16, UINT32, INT8, INT16, INT32, FLOAT32, FLOAT64.

Standard field names: `x`, `y`, `z`, `r`, `g`, `b`, `a`, `intensity`, `ring`,
`nx`, `ny`, `nz` (normals).

This is compatible with ROS `sensor_msgs/PointCloud2` and Foxglove `PointCloud`.

---

## 11. Scene Primitives

### ScenePrimitive (common header + variant payload)

Every scene primitive shares a common header. The type-specific data lives in a
discriminated variant payload. This avoids Foxglove's "8 parallel arrays" pattern
while keeping compile-time type safety.

**Common header** (present on every primitive):

| Field | Type | Notes |
|-------|------|-------|
| `timestamp` | `int64` | When this primitive is valid. |
| `frame_id` | `string` | Coordinate frame. |
| `id` | `string` | Primitive identifier. Same (topic, id) replaces prior primitive. |
| `lifetime_ns` | `int64` | Auto-expiry duration. 0 = persist until replaced/deleted. |
| `frame_locked` | `bool` | If true, re-transform every frame as TF updates. |
| `pose` | `Pose` | Position and orientation in `frame_id`. |
| `color` | `Color` | Uniform color. Type-specific semantics (see payload). |
| `payload` | `variant` | One of the payload types below. |

**Deletion**: a primitive with the same `(topic, id)` replaces the previous one.
To delete, publish a deletion marker (a special payload kind or a flag).

**SceneUpdate** (optional container): a single message carrying
`ScenePrimitive[]` + `SceneDeletion[]` for atomic multi-primitive updates. A topic
can carry either individual ScenePrimitive messages or SceneUpdate batches — the
storage layer treats them equivalently.

For 2D visualization in world space, use z = 0 in all positions.

**Realised pj_scene2D projection contract:** `sdk::SceneEntities` projection is
a stateless per-message snapshot. The decoder does not retain prior entities,
apply deletion markers, expire `lifetime_ns`, or re-transform `frame_locked`
entities. It projects only XY translation from each primitive's `pose.position`;
`pose.orientation` is ignored. Supported primitive lists are arrows, cubes,
spheres, lines, triangles, texts, and axes. Cylinders and models are currently
not projected.

### Payload: ArrowData

| Field | Type |
|-------|------|
| `shaft_length` | `double` |
| `shaft_diameter` | `double` |
| `head_length` | `double` |
| `head_diameter` | `double` |

### Payload: CubeData

| Field | Type | Notes |
|-------|------|-------|
| `size` | `Vector3` | Full extents along each axis. |

### Payload: SphereData

| Field | Type | Notes |
|-------|------|-------|
| `size` | `Vector3` | Diameter along each axis (ellipsoid if non-uniform). |

### Payload: CylinderData

| Field | Type | Notes |
|-------|------|-------|
| `size` | `Vector3` | Bounding box: x/y = diameter, z = height. |
| `bottom_scale` | `double` | 0..1, ratio of bottom face diameter. |
| `top_scale` | `double` | 0..1, ratio of top face diameter. 0 = cone. |

### Payload: MarkersData

Generic vertex-based primitive with a topology enum that maps 1:1 to OpenGL draw modes.
Covers points, lines, polylines, and closed polygons (outlines).

| Field | Type | Notes |
|-------|------|-------|
| `topology` | `enum` | OpenGL draw mode (see table). |
| `thickness` | `double` | Line width or point size. |
| `scale_invariant` | `bool` | If true, `thickness` is in screen pixels. |
| `points` | `Point3[]` | Vertex positions. |
| `colors` | `Color[]` | Per-vertex colors. Empty = use header `color`. |
| `indices` | `uint32[]` | Optional index buffer to avoid vertex duplication. |

| Topology | GL equivalent | Use case |
|----------|---------------|----------|
| `POINTS` | `GL_POINTS` | Scatter plots, waypoints |
| `LINE_LIST` | `GL_LINES` | Disconnected segments (pairs: 0-1, 2-3, ...) |
| `LINE_STRIP` | `GL_LINE_STRIP` | Connected path (0-1, 1-2, ..., n−1-n) |
| `LINE_LOOP` | `GL_LINE_LOOP` | Closed polygon outline (like LINE_STRIP + closing n-0) |

A rectangle is `LINE_LOOP` with 4 points. A polygon outline is `LINE_LOOP` with N points.

### Payload: MeshData

| Field | Type | Notes |
|-------|------|-------|
| `points` | `Point3[]` | Vertex positions. |
| `colors` | `Color[]` | Per-vertex colors. Empty = use header `color`. |
| `indices` | `uint32[]` | Triangle index buffer. Triples: (0,1,2), (3,4,5), ... |

### Payload: TextData

| Field | Type | Notes |
|-------|------|-------|
| `billboard` | `bool` | If true, always faces the camera. |
| `font_size` | `double` | Height of one line of text. |
| `scale_invariant` | `bool` | If true, `font_size` is in screen pixels. |
| `text` | `string` | |

### Payload: ModelData

| Field | Type | Notes |
|-------|------|-------|
| `scale` | `Vector3` | Scale factor per axis. |
| `override_color` | `bool` | If true, use header `color` instead of embedded materials. |
| `media_type` | `string` | MIME type (e.g. `model/gltf-binary`). |
| `data` | `bytes` | Embedded model data. |

---

## 12. Image Annotations

Overlays rendered in image pixel coordinates. These reference an image topic and draw
on top of it. They are not part of the 3D scene graph.

> **Authoritative wire-format spec and type catalog** live in
> `plotjuggler_sdk/pj_base/include/pj_base/builtin/`. The canonical
> `ImageAnnotations` schema and the `foxglove.ImageAnnotations` Protobuf wire
> codec (writer + reader) are part of `pj_base/builtin`; pj_scene2D exposes
> renderer-local aliases through `pj_scene2d_core/scene_frame.h`. Plugin
> authors that *produce* or *consume* markers should use the canonical SDK
> types; this section covers only how pj_scene2D renders the decoded
> primitives.
>
> **Source-format conversion happens loader-side**, not in pj_scene2D. A loader reads its
> source format (CDR `vision_msgs/msg/Detection2DArray`, `yolo_msgs/msg/DetectionArray`,
> CSV, RLDS, etc.), fills an `ImageAnnotation`, and calls
> `PJ::serializeImageAnnotation` before pushing canonical bytes to ObjectStore. PJ4's
> reference adapters live in `pj_scene2D/tests/cdr_*_to_image_annotation.{h,cpp}`
> and `marker_palette.{h,cpp}` (FNV-1a class-id palette + label formatter), exercised
> by `cdr_to_image_annotation_test`.

`MediaViewerWidget` renders **every** `ImageAnnotation` primitive end-to-end through
five QRhi pipelines:

| Pipeline | Topology | Used for |
|---|---|---|
| Image | textured quad | YUV420P or RGB base frame |
| Marker (1 px) | `Lines` | `PointsAnnotation` and circle outlines with `thickness ≤ 1.5` (QRhi `Lines` is fixed-width 1 px on most backends — that's the threshold) |
| Points | `Triangles` | `kPoints` quads, `kLineLoop` fills, circle fills |
| Thick lines | `Triangles` | `PointsAnnotation` and circle outlines with `thickness > 1.5`, expanded CPU-side to perpendicular rectangles |
| Text | `Triangles` (textured) | `TextAnnotation` — one quad per label, glyph mask painted by `QPainter` to a `QImage::Format_Alpha8` and uploaded as a `QRhiTexture::R8`. Per-vertex colour acts as a tint over the alpha mask, so two labels with the same text+size but different colours share the same texture (cache key is `(text, font_size_q)`). |

Draw order is `image → fills → 1 px lines → thick lines → text`, so strokes always
render on top of fills and text on top of everything. Per-vertex colour
(`PointsAnnotation.colors[]`) is honoured when its size matches `points.size()`,
otherwise `color` is splatted across all vertices. `LineLoop` fill (`fill_color.a > 0`)
is a triangle fan from `points[0]` — convex polygons only.

**Limitations**: thick lines have no miter joins (adjacent segments butt-join with a
possible visible gap at sharp angles); polygon fills are convex-only; text is rasterised
once per `(text, font_size)` and uses Qt's default font selection (no fallback for
missing glyphs). The text cache is cleared in `releaseResources()`; for now there is
no LRU eviction.

For the schema field tables (`ImageAnnotations`, `PointsAnnotation`,
`CircleAnnotation`, `TextAnnotation`), see
`plotjuggler_sdk/pj_base/include/pj_base/builtin/image_annotations.hpp`.

---

## 13. Grid (Occupancy / Costmap)

| Field | Type | Notes |
|-------|------|-------|
| `timestamp` | `int64` | Nanoseconds since epoch. |
| `frame_id` | `string` | Coordinate frame. |
| `pose` | `Pose` | Origin of grid cell (0,0). |
| `columns` | `uint32` | Number of columns. |
| `rows` | `uint32` | Number of rows. |
| `cell_size` | `Vector2` | Size of each cell in meters `{ x, y }`. |
| `cell_stride` | `uint32` | Bytes per cell in `data`. |
| `row_stride` | `uint32` | Bytes per row in `data`. |
| `fields` | `FieldDescriptor[]` | Per-cell channel layout (same as PointCloud). |
| `data` | `bytes` | Packed cell data. |

`Vector2`: `{ double x, y }`.

---

## 14. Summary — Complete Type List

| # | Type | Category | Self-contained? | Blob-like? |
|---|------|----------|-----------------|------------|
| 1 | FrameTransform | Transform | Yes | No |
| 2 | Image | 2D | Yes | Yes (pixel data) |
| 3 | DepthImage | 2D | Yes | Yes (pixel data). `depth_scale` required. |
| 4 | SegmentationImage | 2D | Yes | Yes (class-ID pixels). References ClassRegistry. |
| 5 | ClassRegistry | 2D support | Yes | No |
| 6 | VideoFrame | 2D | No (GOP deps) | Yes (encoded frame) |
| 7 | CameraCalibration | 2D | Yes | No |
| 8 | ImageAnnotation | 2D overlay | Yes | No |
| 9 | PointCloud | 3D | Yes | Yes (packed points) |
| 10 | ScenePrimitive (+ optional SceneUpdate) | 3D markers | Yes | No |
| 11 | Grid | 2D/3D | Yes | Yes (packed cells) |

ScenePrimitive payload variants:
ArrowData, CubeData, SphereData, CylinderData, MarkersData, MeshData, TextData,
ModelData. pj_scene2D's current `SceneEntities2DDecoder` projects 7 of the 9
realised Foxglove-style primitive lists (arrows, cubes, spheres, lines,
triangles, texts, axes); cylinders and models are not projected.

The four image-family types — Image, DepthImage, SegmentationImage, VideoFrame —
share a family criterion described in §1: split when the *decoded value* has a
different meaning, or when the *consumption rules* differ. They are unified at the
encoding level (no separate `CompressedImage`) but split at the semantic level.

---

## 15. What We Chose Not To Include (and why)

| Omitted type | Reason |
|--------------|--------|
| Separate `CompressedImage` / `RawImage` | Unified into Image with encoding field. Decoded result is the same kind of thing in both cases. |
| Separate `Points3D` / `Points2D` | MarkersData with `topology = POINTS`. |
| Separate `LineStrips2D` / `Boxes2D` / `Arrows2D` | 3D primitives with z = 0. |
| Encoded depth video (`DepthVideoFrame`) | Real but specialized (RealSense H.264-mono16). Deferred until demand justifies a schema change; producers convert to per-frame DepthImage in the meantime. |
| Instance / panoptic segmentation as a distinct type | Expressible as multiple SegmentationImage topics with disjoint registries. Future schema extension if needed. |
| `Tensor` | Out of scope — not a scene type. Can be revisited. |
| `BarChart` / `SeriesLines` | Belongs in the time-series/plotting layer, not scene types. |
| `JointState` | Plottable as time-series (scalars). Robot model visualization uses SceneEntity. |
| `VoxelGrid` | Deferred. Can be added later as a Grid variant or new type. |
| `Asset3D` (standalone) | Originally covered by ModelPrimitive inside SceneEntity; the SDK has since added a standalone `sdk::Mesh3D` binary mesh asset (`pj_base/builtin/mesh3d.hpp`, `kMesh3D = 9`). Host-side consumption is landing with pj_scene3D's URDF/mesh work (PR #164); not yet on main. |
| `AssetVideo` (whole file) | Deferred for the host. The SDK ships the `sdk::AssetVideo` struct + codec (`pj_base/builtin/asset_video.hpp`, `kAssetVideo = 12`), but the host has no decode path for it; per-frame `VideoFrame` (§8) is the canonical video model. |
| `LaserScan` | Converted to PointCloud at ingest time (polar → cartesian). |
| `GraphNodes` / `GraphEdges` | Rerun-specific. Deferred. |
| `Log` | Realised since: `sdk::Log` (`pj_base/builtin/log.hpp`, `kLog = 16`) — textual log messages; not a pj_scene2D rendering concern. |
| `GeoPoint` / `GeoJSON` | Deferred. Geospatial types can be added later. |
