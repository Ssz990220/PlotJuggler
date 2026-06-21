# Rerun 2D Architecture — Research Notes

Analysis of Rerun's (rerun.io) architecture for 2D data visualization,
based on examination of their source code (`rerun-io/rerun`, `main` branch).
Focused on image, video, annotation, and depth rendering — the same
domain as pj_scene2D.

---

## 1. Data Model for 2D

Rerun uses an **archetype/component** system defined via FlatBuffers
schemas. Each archetype is a named bundle of required, recommended,
and optional components.

### Image Archetypes

**`Image`** — raw uncompressed image:
- Required: `ImageBuffer` (raw bytes as `Blob`), `ImageFormat`
  (resolution + pixel format: RGB, RGBA, etc.)
- Optional: `Opacity`, `DrawOrder` (default `-10.0`),
  `MagnificationFilter`
- Buffer is row-major, interleaved-pixel. Stored in an Arrow
  `LargeBinary` array.

**`EncodedImage`** — compressed (JPEG, PNG):
- Required: `Blob` (compressed bytes)
- Recommended: `MediaType` (e.g. `image/jpeg`, `image/png`)
- Optional: `Opacity`, `DrawOrder`, `MagnificationFilter`
- Decoded on the viewer side.

**`DepthImage`** — depth maps:
- Required: `ImageBuffer`, `ImageFormat`
- Optional: `DepthMeter` (meters-per-unit, default `1.0` for float,
  `1000.0` for int), `Colormap` (default Turbo), `ValueRange`,
  `FillRatio`, `DrawOrder` (default `-20.0`), `MagnificationFilter`
- In 3D views, generates a point cloud; in 2D, rendered as a
  colormapped image.

**`SegmentationImage`** — semantic segmentation masks:
- Required: `ImageBuffer`, `ImageFormat`
- Optional: `Opacity` (defaults to `0.5` when other images present,
  `1.0` otherwise), `DrawOrder` (default `0.0`)
- Each pixel is a `ClassId` mapped to color/label via
  `AnnotationContext`.

### Video Archetypes

**`AssetVideo`** — the video file itself (MP4 container only):
- Required: `Blob` (the **entire MP4 file**)
- Recommended: `MediaType` (`video/mp4`)
- Has no visualizer itself — it is a data source referenced by
  `VideoFrameReference`.

**`VideoFrameReference`** — references a specific frame in a video:
- Required: `VideoTimestamp` (timestamp relative to video start; uses
  closest frame, not latest-at)
- Optional: `EntityPath` (reference to entity with `AssetVideo`),
  `Opacity`, `DrawOrder` (default `-15.0`)
- One of these must be logged for each frame you want to display.

**`VideoStream`** (unstable) — raw video stream chunks for live
streaming:
- Required: `VideoCodec` (H264, H265, AV1, etc.)
- Recommended: `VideoSample` (the raw encoded chunk; one sample per
  frame)
- Optional: `Opacity`, `DrawOrder` (default `-15.0`)
- No B-frame support currently. Samples logged on the timeline where
  current timestamp = PTS.

### 2D Annotation Archetypes

**`Boxes2D`** — bounding boxes:
- Required: `HalfSize2D[]`
- Recommended: `Position2D[]` (centers), `Color[]`
- Optional: `Radius[]` (line width), `Text[]` (labels), `ShowLabels`,
  `DrawOrder` (default `10.0`), `ClassId[]`

**`Points2D`** — point clouds in 2D:
- Required: `Position2D[]`
- Recommended: `Radius[]`, `Color[]`
- Optional: `Text[]`, `ShowLabels`, `DrawOrder` (default `30.0`),
  `ClassId[]`, `KeypointId[]`

**`LineStrips2D`** — polylines:
- Required: `LineStrip2D[]`
- Recommended: `Radius[]`, `Color[]`
- Optional: `Text[]`, `ShowLabels`, `DrawOrder` (default `20.0`),
  `ClassId[]`

**`Arrows2D`** — arrows:
- Required: `Vector2D[]`
- Recommended: `Position2D[]` (origins)
- Optional: `Radius[]`, `Color[]`, `Text[]`, `ShowLabels`,
  `DrawOrder`, `ClassId[]`

### Layer Compositing via DrawOrder

All 2D archetypes have a `DrawOrder` component (float). Default values
create a natural stacking order:

| Layer | DrawOrder |
|-------|-----------|
| Depth images | `-20.0` |
| Video frames | `-15.0` |
| Regular images | `-10.0` |
| Segmentation | `0.0` |
| Boxes | `+10.0` |
| Lines | `+20.0` |
| Points | `+30.0` |

`DrawOrder` values are converted to integer depth offsets via
`EntityDepthOffsets` (in `re_view_spatial/src/contexts/depth_offsets.rs`)
and applied to the wgpu depth buffer during rendering. This enables
correct painter's-algorithm compositing without an explicit compositor
class.

---

## 2. Storage and Chunking

### Chunk-Based Columnar Store

Rerun uses a custom chunk-based columnar store built on **Apache Arrow**.

**`re_chunk::Chunk`** (`crates/store/re_chunk/src/chunk.rs`):
- A chunk is an Arrow record batch containing time columns + component
  columns.
- Components stored as `ArrowListArray` (list-of-structs layout) keyed
  by `ComponentIdentifier`.
- Chunks carry metadata: `ChunkId`, `RowId`, entity path, timeline
  columns, sorted status.

**`re_chunk_store::ChunkStore`** (`crates/store/re_chunk_store/`):
- In-memory time-series database indexed by entity path and timeline.
- Two query modes: **LatestAt** (most-recent value at a timestamp) and
  **Range** (all values within a time interval).
- Chunks are compacted (merged) up to configurable thresholds:
  - `chunk_max_bytes`: ~384 KB (default)
  - `chunk_max_rows`: 4096 (sorted), 1024 (unsorted)
- These defaults are intentionally modest — the Rerun team found that
  "a few megabytes turned out to be way too costly to concatenate in
  real-time."

### Handling Large Blobs

Images are stored as `Blob` components (Arrow `LargeBinary` arrays).
Each image frame is a row in a chunk. Since images are large (e.g.
1920x1080x3 = ~6 MB), each image frame typically creates its own chunk
(exceeding the `chunk_max_bytes` threshold).

For `AssetVideo`, the **entire MP4 file** is stored as a single `Blob`.
Video stream samples (`VideoSample`) are individual encoded frame blobs
logged per-timestamp. The chunk store holds references via `Tuid`
identifiers; the video player retrieves them on demand via a
`get_video_buffer` callback that resolves `Tuid → &[u8]` through the
store.

### Garbage Collection

`re_chunk_store/src/gc.rs`:
- **Target modes**: `DropAtLeastBytes`, `DropAtLeastFraction`,
  `Everything`
- **Strategy**: Priority to chunks **furthest from a given timestamp**;
  falls back to `RowId` ordering (client wallclock). This is
  proximity-based eviction, not LRU — the same pattern recommended in
  pj_scene2D's REQUIREMENTS.md §4.2.
- **Protections**: Can protect latest N values per component per
  timeline, protect specific time ranges, protect specific chunk IDs.
- **Time budget**: Configurable (default 3.5 ms in the viewer).
- GC operates at chunk granularity — cannot partially evict a chunk.
- For video streams, evicted samples are marked as "unloaded" (metadata
  retained for seeking, data GC'd from store).

---

## 3. Video Decoding Pipeline

### Architecture Overview

```
AssetVideo/VideoStream (data)
  → VideoDataDescription (demuxed metadata + keyframe index)
    → VideoPlayer (seeking/scheduling)
      → AsyncDecoder (background thread)
        → Frame (decoded pixels)
          → GPU texture upload (re_renderer)
```

Key source files:
- `crates/utils/re_video/src/demux/mod.rs`
- `crates/utils/re_video/src/player/mod.rs`
- `crates/utils/re_video/src/decode/mod.rs`

### Demuxing

**`VideoDataDescription`** is the core metadata structure:
- `codec: VideoCodec` (H264, H265, AV1, VP8, VP9, ImageSequence)
- `encoding_details: Option<VideoEncodingDetails>` (dimensions, bit
  depth, chroma subsampling, SPS/PPS for H264)
- `timescale: Option<Timescale>` (time units per second)
- `samples: StableIndexDeque<SampleMetadataState>` — sorted by DTS,
  each with PTS/DTS/duration/is_sync/source_id
- `keyframe_indices: Vec<SampleIndex>` — sorted list of keyframe
  sample indices
- `delivery_method: VideoDeliveryMethod` — `Static { duration }` or
  `Stream { last_time_updated_samples }`

MP4 files are demuxed via the `re_mp4` crate. For video streams, GOP
detection (`gop_detection.rs`) parses raw NALUs to identify keyframes
by inspecting SPS/PPS NALs (H.264), VPS/SPS/PPS (H.265), or OBU
headers (AV1).

### Decoder Backends

Rerun has **four** decoder backends:

**1. FFmpeg CLI** (`decode/ffmpeg_cli/ffmpeg.rs`) — native, H.264/H.265:
- Spawns an **FFmpeg child process** via `ffmpeg_sidecar`.
- Feeds raw NALUs to FFmpeg's stdin (Annex B format).
- Reads decoded frames from FFmpeg's stdout as raw pixel data.
- Three threads: write thread (stdin), listen thread (stdout), main.
- Uses `re_quota_channel` for backpressure (512 MB max in-flight).
- Requests YUV output when chroma subsampling is known, falls back to
  RGBA.
- Forces BT.709 matrix coefficients and full range via FFmpeg flags.

**2. dav1d** (`decode/av1.rs`) — native AV1:
- Uses the `dav1d` Rust crate (wrapper around the C dav1d library).
- Wrapped in `AsyncDecoderWrapper` for async operation.
- Not available on Linux ARM64.

**3. WebCodecs** (`decode/webcodecs.rs`) — web/WASM only:
- Uses browser's WebCodecs API (`VideoDecoder`).
- Handles quirks: Firefox/Safari may output frames in decode order
  instead of presentation order.
- Supports hardware acceleration hints.

**4. Image decoder** — for `ImageSequence` codec:
- Decodes JPEG/PNG sequences as "video".
- On web, uses browser's image decoding.

### Async Decoder Interface

```rust
pub trait AsyncDecoder: Send + Sync {
    fn submit_chunk(&mut self, chunk: Chunk) -> Result<()>;
    fn end_of_video(&mut self) -> Result<()>;
    fn reset(&mut self, video_descr: &VideoDataDescription) -> Result<()>;
    fn min_num_samples_to_enqueue_ahead(&self) -> usize;
}
```

`AsyncDecoderWrapper` runs a `SyncDecoder` on a dedicated background
thread, communicating via channels with backpressure. Commands (Chunk,
Reset, Stop) are sent via `Sender<Command>`. Decoded `FrameResult`s
are sent back on `Sender<FrameResult>`.

### Seeking and Keyframe Management

**`VideoPlayer::frame_at()`** is the central method:

1. Finds the sample whose PTS is closest to the requested timestamp
   via `latest_sample_index_at_presentation_timestamp()`.
2. Calls `request_keyframe_before()` which walks backward from the
   requested sample to find the nearest keyframe (using
   `keyframe_indices`).
3. If the keyframe is from a different GOP than what the decoder has,
   issues a `reset()` on the decoder.
4. Enqueues all samples from the keyframe up to the requested sample
   (in DTS order) via `submit_chunk()`.
5. Polls the `frame_receiver` for decoded frames, keeping only the
   latest frame at or before the requested PTS.
6. Implements a **decoder delay state machine** with states:
   - `UpToDate`
   - `UpToDateWithinTolerance` (3-frame tolerance)
   - `UpToDateToleratedEdgeOfLiveStream`
   - `Behind`
7. Has a **400ms grace delay** before reporting errors (prevents
   flicker during fast scrubbing).

For backward seeks, the decoder is fully reset and re-primed from the
nearest keyframe. For forward seeks within the same GOP, existing
samples are just enqueued incrementally.

### Frame Output Format

Decoded frames (`FrameContent`) contain:
- Native: `data: Vec<u8>`, `width`, `height`, `format: PixelFormat`
- Web: `web_sys::VideoFrame` (opaque browser handle)

`PixelFormat` variants: `L8`, `L16`, `Rgb8Unorm`, `Rgba8Unorm`,
`Yuv { layout, range, coefficients }`

---

## 4. Rendering Pipeline for 2D

### GPU Stack

Rerun uses **wgpu** for all rendering, via their custom `re_renderer`
crate. The renderer runs on Vulkan, Metal, D3D12, or WebGPU (with
WebGL fallback on web). The UI uses **egui** (immediate-mode GUI).

### Image-to-GPU Path

1. Image data arrives as `ImageInfo` (buffer + format + kind).
2. `gpu_bridge::image_to_gpu()` converts to `ColormappedTexture`:
   - Creates/reuses a `GpuTexture2D` via `texture_manager_2d`.
   - Transfers data via `transfer_image_data_to_texture()`.
3. Image is rendered as a `TexturedRect` — a GPU quad with:
   - Position/extent vectors (z=0 for 2D)
   - `ColormappedTexture` (texture + range + gamma + color mapper +
     alpha mode)
   - `RectangleOptions` (filter modes, depth offset, tint, outline)
4. `RectangleRenderer` issues **one draw call per textured rectangle**
   (bindless textures not widely available).

### YUV-to-RGB Conversion

`YuvConverter` (`resource_managers/yuv_converter.rs`):
- Supports planar: `Y_U_V444`, `Y_U_V422`, `Y_U_V420`
- Semi-planar: `Y_UV420`
- Interleaved: `YUYV422`
- Uses compute or fragment shaders (`yuv_converter.wgsl`)
- Applies correct matrix coefficients (BT.601 vs BT.709) and range
  (limited vs full)
- YUV data uploaded as a single-channel data texture; shader samples
  from appropriate offsets for each plane.

### Video Frame-to-GPU Path

1. `Video::frame_at()` calls `VideoPlayer::frame_at()` with a closure
   `update_output`.
2. The closure calls `chunk_decoder::update_video_texture_with_frame()`.
3. Allocates/reuses a `GpuTexture2D` (format: `Rgba8Unorm`, with
   `COPY_DST | COPY_SRC | TEXTURE_BINDING | RENDER_ATTACHMENT`).
4. Dispatch on pixel format:
   - `Rgb8Unorm`: pad to RGBA, then upload
   - `Rgba8Unorm`: direct texture upload
   - `Yuv { .. }`: upload raw data + schedule GPU conversion
   - `L8`/`L16`: upload as single-channel texture
5. Web: `copy_external_image_to_texture()` transfers the browser's
   `VideoFrame` directly to the GPU texture (zero-copy when possible).

### Immediate Mode Architecture

The entire viewer is **immediate-mode**: every frame, the viewer
re-queries the chunk store, massages results, and feeds them to the
renderer. There is no retained scene graph. This eliminates state
synchronization bugs but requires relentless optimization (and they
note plans to add query/render caching for large datasets in the
future).

---

## 5. Timeline and Scrubbing

### Time Control

`re_time_panel` provides the timeline UI:
- `TimeControl` manages current time cursor, playback state, and
  selected timeline.
- Supports multiple timelines (sequence-based like frame number, or
  time-based like wall clock).
- Provides loop/selection ranges.

### Query Model

When the user scrubs the time cursor:
1. A `LatestAtQuery(timeline, timestamp)` is constructed.
2. Each visualizer executes this query against the chunk store.
3. The chunk store returns the most relevant chunk(s) via binary
   search through temporal indices.
4. For video: the query timestamp is converted to a video-relative PTS
   and passed to `VideoPlayer::frame_at()`.

### Video Scrubbing

1. `VideoFrameReferenceVisualizer` reads `VideoTimestamp` at the
   current cursor time.
2. Passes to `Video::frame_at()` → `VideoPlayer`.
3. Player determines if a seek is needed:
   - Forward within same GOP: enqueue missing samples.
   - Backward or large jump: reset decoder, seek to keyframe,
     re-decode.
4. Maintains `last_requested` sample index to detect direction changes.
5. During seeking, the last-good frame is shown (fading out slightly)
   with an optional loading indicator.
6. 400ms grace delay prevents flickering during fast scrubbing.

### Multiple Simultaneous Streams

A single `Video` can have multiple `VideoPlayer` instances keyed by
`VideoPlayerStreamId`. This allows displaying the same video at
different timestamps simultaneously (e.g., different views).

---

## 6. Streaming Architecture

### Live Data Ingestion

Data reaches the viewer via:
- **gRPC**: streaming over network (primary live path)
- **`.rrd` files**: sequential log messages appended to a file
- **SDK direct**: `rr.init(..., spawn=True)` launches viewer and
  streams data

The logging SDKs encode data as Apache Arrow and send chunks via
these transport layers.

### Video Streaming

`VideoStream` archetype (unstable) enables live video:
- `VideoCodec` is logged once (static).
- `VideoSample` blobs are logged per-frame on the timeline.
- Viewer builds `VideoDataDescription` incrementally:
  - Samples accumulate in `StableIndexDeque` (supports front removal
    for retention).
  - GOP detection runs on each incoming sample (NAL parsing).
  - When samples are added, `Video::handle_sample_insertion()`
    determines if active players need reset.

### Retention and Eviction

Managed at multiple levels:

1. **Chunk Store GC**: Evicts oldest chunks when memory pressure
   triggers GC. Uses `furthest_from` timestamp to prioritize eviction.
   `protect_latest` keeps the most recent N values per component.

2. **Video Data Description**: Evicted samples transition to
   `SampleMetadataState::Unloaded { source_id }` — metadata retained
   for seeking, data GC'd. The player calls `get_buffer(source_id)`
   which returns empty for unloaded samples, triggering "unloaded
   sample" errors.

3. **Video Player Memory**: `re_quota_channel` provides backpressure
   with 512 MB cap. Decoded frames in `frames_by_pts: BTreeMap` are
   pruned — only frames >= requested PTS are kept.

4. **GPU Textures**: `GpuTexturePool` manages lifetime. Unused video
   player entries are purged per-frame via `used_last_frame` flag.
   `texture_manager_2d` uses hash-based caching with deduplication.

### Edge-of-Stream Handling

- `DecoderDelayState::UpToDateToleratedEdgeOfLiveStream` — tolerates
  being behind without showing a loading indicator.
- `time_until_video_assumed_ended` (250ms): if no new samples arrive,
  signals end-of-video to flush remaining frames.
- FFmpeg decoder requires stdin close + process restart when
  transitioning from end-of-video back to active streaming.

---

## 7. Comparison with pj_scene2D

| Aspect | Rerun | pj_scene2D |
|--------|-------|----------|
| **Video storage** | Whole MP4 as blob; per-frame `VideoSample` for streaming | Per-frame in ObjectStore (streaming); direct file access (MP4) |
| **Decoder linkage** | FFmpeg as **child process** (stdin/stdout IPC) | FFmpeg **linked** (libavcodec API) |
| **GPU API** | wgpu (Vulkan/Metal/D3D12/WebGPU) | QRhi (Qt 6.8: Vulkan/Metal/D3D11/OpenGL) |
| **YUV conversion** | GPU compute/fragment shader (BT.601/BT.709 aware) | GPU fragment shader, **colorspace-aware** as of 2026-06-21 (BT.601/709 + limited/full range, selected per-stream from the codec's signalled colorimetry; see §8) |
| **Pixel upload** | Per-format (RGB pad, RGBA direct, YUV planes, NV12 two-plane) | RGB/RGBA/Mono8/BGRA/YUV420P + **native NV12 two-plane** (RG8 UV; CPU deinterleave fallback) as of 2026-06-21 |
| **Magnification filter** | Per-image nearest/linear (`MagnificationFilter`) | Per-image-layer nearest/linear toggle as of 2026-06-21 |
| **Frame delivery** | Immediate-mode re-query every render frame | Pull-based `MediaSource::takeFrame()` |
| **Layer compositing** | `DrawOrder` float → depth buffer offsets | `CompositeMediaSource` (shipped): fans `setTimestamp()` to N owned layers and fuses their `MediaFrame`s (opacity-folded pixel layers stacked bottom-to-top in add order, plus concatenated overlays); wired into `Scene2DDockWidget` |
| **Keyframe index** | Built from MP4 demux or NAL parsing | Inline in decoder today — `StreamingVideoDecoder` keyframe vector (streaming) / `FfmpegBackend` FFmpeg seek index (file); a `MediaIndexRegistry` sidechannel is designed for a future file-backed ObjectStore path but not yet implemented (no header exists) |
| **Scrub strategy** | Keyframe seek + decode forward; 400ms grace delay | Keyframe seek + decode forward; direction-aware partials + thumbnail cache |
| **Retention/GC** | Chunk-level GC, furthest-from-cursor priority | ObjectStore time/memory budget per topic |
| **Backward scrub** | Show last-good frame (fading) | Publish keyframe instantly, then refine to target; thumbnail cache for sub-keyframe positions |
| **B-frame support** | Not supported in streaming; tolerated in MP4 | Full support (DTS-keyed storage, decoder reorder) |
| **Scene graph** | None (immediate-mode) | None (pull-based per-widget) |
| **Threading model** | Async decoder on background thread + channels | FfmpegBackend owns decode thread; StreamingVideoSource owns worker thread |
| **Language** | Rust | C++20 |

### Design Insights

**DrawOrder compositing**: Rerun's approach of a single float per
archetype that maps to depth buffer offsets is simpler than a dedicated
compositor class. pj_scene2D's shipped `CompositeMediaSource` instead
composites CPU-side (opacity-folded layers in add order); Rerun's
depth-offset pattern remains worth considering if GPU compositing is
ever needed — each `MediaSource` could carry a `draw_order` and the
widget could composite via depth testing rather than explicit
CPU-side blending.

**FFmpeg as subprocess**: Avoids LGPL linking concerns and provides
crash isolation (decoder crash doesn't kill the viewer), but adds IPC
overhead (~1ms per frame for stdout pipe reads). pj_scene2D's direct
linking is the right call for a desktop application where LGPL
compliance is manageable and the IPC overhead would hurt scrub
responsiveness.

**Immediate-mode re-query**: Simpler architecturally (no stale state),
but inefficient for expensive decodes. pj_scene2D's `MediaSource` pull
model is better for PlotJuggler's use case where video decode is the
bottleneck, not query overhead.

**Proximity-based eviction**: Both Rerun and pj_scene2D use
furthest-from-cursor eviction (not LRU). This is the correct strategy
for scrub workloads where temporal locality matters.

**400ms grace delay**: Rerun prevents flicker during fast scrub by
showing the last good frame for up to 400ms before reporting decode
errors. pj_scene2D achieves the same via the `FrameSlot` latest-wins
pattern (stale frame persists until replaced) and the `ThumbnailCache`
for instant backward feedback.

**Whole-MP4-as-blob**: Storing the entire MP4 in the chunk store is
conceptually simpler but doesn't scale to large files (GBs). pj_scene2D's
approach of accessing the file directly via `FfmpegBackend` avoids this
— the file is its own random-access store. For streaming,
per-frame entries in ObjectStore with retention are the right model.

---

## 8. Adoption decisions (2026-06-21)

This document was re-audited against the as-built `pj_scene2D` on 2026-06-21. The
big architectural calls in §7 still hold (linked libavcodec over subprocess IPC;
pull-based `MediaSource` over immediate-mode re-query; file-as-its-own-store over
whole-MP4-blob). What the comparison table had treated as parity but wasn't is the
fine-grained **rendering correctness** — that's where the remaining good Rerun ideas
were. The following were decided and (where adopted) shipped on
`feat/scene2d-render-correctness`.

### Adopted

- **Colorspace-aware YUV→RGB (BT.601/709 + limited/full range).** This was a latent
  *bug*, not a missing feature: `FfmpegDecoder` decoded through libavcodec (whose
  `AVFrame` carries `colorspace` + `color_range`) but dropped that metadata and the
  shader hardcoded full-range BT.709 — so SD content shifted hue and the common
  limited-range H.264/HEVC rendered with washed-out blacks/whites. Fix: carry
  `YuvColorSpace`/`YuvColorRange` on `DecodedFrame` (mapped from the `AVFrame`, with
  the standard SD-height→601 / unspecified-range→limited heuristics), and build the
  GPU color matrix per-frame via `pj_scene2d_core/video_color.h::buildYuvMatrix`. The
  matrix is a single affine 4×4 that bakes range scale+offset into the constant
  column, so the **shader is unchanged** (`rgb = M·vec4(y, u-0.5, v-0.5, 1)`); BT.709
  + full reduces to the exact historical matrix, so full-range content is byte-for-
  byte unaffected. Mirrors Rerun's `yuv_converter.rs`. Unit-tested in `video_color_test`.

- **Depth Auto-fit (on demand).** The depth layer kept a hardcoded 0–4 m default
  because the team's earlier attempt at continuous auto-range flickered (it used a
  naive per-frame min/max). Adopted Rerun's robust-percentile idea, but as a one-shot
  the user triggers: an **"Auto-fit range"** button snaps Near/Far to the current
  frame's 2nd/98th depth percentiles (`pj_scene2d_core/depth_range.h`, unit-tested;
  `DepthPipelineSource::autoRange` over the cached last frame). Manual remains the
  default — zero behaviour change until the button is pressed — which is why this is
  safe where continuous auto was not.

- **Per-image magnification filter (nearest/linear).** Mirrors Rerun's
  `MagnificationFilter`. A per-image-layer "Pixelated (nearest)" toggle (persisted in
  layout XML) carried on `DecodedFrame::mag_filter`; the widget keeps both samplers
  and rebinds on change. Useful for pixel inspection. Depth always samples its
  R32F y-plane nearest regardless (never blend the no-data sentinel), so the toggle
  is image-only.

- **Native NV12 two-plane upload (perf).** Mirrors Rerun's per-format upload. VAAPI
  hardware decode downloads NV12; previously that was repacked to YUV420P with a
  full-frame `sws_scale` every frame. Now `FfmpegDecoder` emits `kNV12` natively and
  the widget uploads Y (R8) + interleaved UV (RG8), skipping the repack. A CPU
  deinterleave fallback (`nv12ToYuv420p`) covers the rare no-RG8 backend and the
  thumbnail encoder (which stays planar). **Caveat:** the native two-plane GPU path
  is exercised only on hardware decode, which CI's software decoders don't hit — so
  it is unit-tested at the buffer-layout level (`decoded_frame_test`) and verified
  live on VAAPI, but not pixel-verified in CI.

### Deferred (good ideas, larger or lower-value — not in this pass)

- **SegmentationImage + AnnotationContext (ClassId→color/label).** High value for the
  ML/robotics audience and the existing depth GPU-LUT path is a near-perfect template
  for the render side (class-id → palette-LUT lookup). Deferred because the *valuable*
  part — a shared `AnnotationContext` giving consistent colors/labels across frames —
  is a cross-repo change: a new canonical type/concept in the `plotjuggler_sdk`
  submodule, a parser change in `pj-official-plugins`, and the viewer render path.
  That is several coordinated PRs and an SDK ABI consideration, not a single in-repo
  change. Worth doing next as its own effort.

- **Decoder-delay / error-debounce during fast scrub** (Rerun's 400 ms grace +
  UpToDate/Behind state machine). Low value here: pj_scene2D already shows the
  last-good frame via the latest-wins mailbox and instant backward feedback via the
  `ThumbnailCache`, and live/scrub mutual-exclusion means a scrub-time keyframe can't
  be evicted mid-seek (the main source of decode errors Rerun's grace delay hides).
  Not worth the added state machine.

### Skipped (deliberate — do not re-open without a new reason)

- **DrawOrder → GPU depth-buffer compositing.** pj_scene2D composites a small,
  explicitly-ordered, user-reorderable layer stack CPU-side with alpha-over. Rerun's
  float-DrawOrder→depth-offset shines with many archetypes at arbitrary orders; with
  PJ's handful of layers it adds a depth-buffer pass and an ordering convention for no
  real gain. (This was §7's one "worth considering" — considered, declined.)
- **VP8 / VP9 decode.** Deliberately unsupported: no usable keyframe oracle for
  seeking (the codec whitelist rejects them). Correct call.
- **ImageSequence-as-video.** Redundant — image topics already seek frame-by-frame
  through ObjectStore `latestAt`; there is no separate "video" wrapper to gain.
- **Multiple `VideoPlayer`s per topic** (same video at two timestamps at once). Niche;
  one decoder per topic is the right default.
- **Decoded-frame BTreeMap cache for backward scrub.** PJ's `ThumbnailCache` +
  re-decode-from-keyframe is leaner and already gives instant backward feedback;
  Rerun's full decoded-frame cache trades a lot of memory for marginal benefit here.
- **Proximity-from-cursor eviction / protect-latest-N in ObjectStore.** Eviction is
  front-drop FIFO; live/scrub mutual-exclusion means eviction never runs during a
  scrub, so cursor-aware eviction would optimise a case that can't occur.
- **Immediate-mode re-query, whole-MP4-as-blob, 512 MB backpressure quota.** Already
  rejected in §7 and still correct; PJ's decoded-frame memory is naturally bounded by
  the latest-wins mailbox + bounded reorder buffer.

---

## Source File Reference

| Area | Path in `rerun-io/rerun` |
|------|--------------------------|
| Architecture overview | `ARCHITECTURE.md` |
| 2D archetype definitions | `crates/store/re_sdk_types/definitions/rerun/archetypes/{image,depth_image,...}.fbs` |
| Chunk data model | `crates/store/re_chunk/src/chunk.rs` |
| Chunk store (queries, GC) | `crates/store/re_chunk_store/src/{store,query,gc}.rs` |
| Video demux/metadata | `crates/utils/re_video/src/demux/mod.rs` |
| Video player (seeking) | `crates/utils/re_video/src/player/mod.rs` |
| Decoder abstraction | `crates/utils/re_video/src/decode/mod.rs` |
| FFmpeg CLI decoder | `crates/utils/re_video/src/decode/ffmpeg_cli/ffmpeg.rs` |
| dav1d AV1 decoder | `crates/utils/re_video/src/decode/av1.rs` |
| Async decoder wrapper | `crates/utils/re_video/src/decode/async_decoder_wrapper.rs` |
| GOP detection | `crates/utils/re_video/src/gop_detection.rs` |
| GPU video texture | `crates/viewer/re_renderer/src/video/{mod,chunk_decoder}.rs` |
| YUV conversion | `crates/viewer/re_renderer/src/resource_managers/yuv_converter.rs` |
| Rectangle renderer | `crates/viewer/re_renderer/src/renderer/rectangles.rs` |
| Image visualizer | `crates/viewer/re_view_spatial/src/visualizers/images.rs` |
| Video visualizers | `crates/viewer/re_view_spatial/src/visualizers/video/{mod,...}.rs` |
| 2D spatial view | `crates/viewer/re_view_spatial/src/{view_2d,ui_2d}.rs` |
| DrawOrder/depth offsets | `crates/viewer/re_view_spatial/src/contexts/depth_offsets.rs` |
| Image-to-GPU bridge | `crates/viewer/re_renderer/src/resource_managers/image_data_to_texture.rs` |
