# pj_scene2D

2D media/scene widget family: decoding and GPU display of images, video,
depth maps, and pixel-space annotation overlays, synchronized with the global
timeline. Ships as two CMake targets with a strict dependency direction:
`pj_scene2d_core` (Qt-free C++20 — decoders, `MediaSource` pipeline sources,
keyframe indexing, compositing) and `pj_scene2d_widgets` (Qt — the
`QRhiWidget`-based `MediaViewerWidget`, the `Scene2DDockWidget` layer stack on
top of `pj_scene_common`). pj_scene2D is a read-only consumer of
`pj_datastore::ObjectStore`; it never writes to storage.

**Depth images** arrive as `sdk::Image` with a depth `encoding` (16UC1 / 32FC1 /
compressedDepth) — there is no `kDepthImage` producer. `Scene2DDockWidget` peeks a
`kImage` topic's first sample and routes depth-encoded images to the colormap
`DepthImageLayer` (per-layer colormap turbo/viridis/plasma/grayscale, invert, and a
manual near-far range) and everything else to the plain `ImageLayer`. Both
`ImagePipelineSource` and `DepthPipelineSource` obtain the `sdk::Image` from a store
entry through one shared seam — `image_resolve.h::resolveImage` (the topic's
MessageParser when present, else the canonical `pj_image_v1` codec). The depth decode
— including the heavy compressedDepth PNG inflate — runs **off the UI thread** on an
`AsyncFrameWorker`, like the image/video sources; `setTimestamp()` posts and returns,
`takeFrame()` polls, and a frame-ready callback drives the repaint.

The colormap is applied **on the GPU**: `DepthPipelineSource` emits a raw float
depth frame (`PixelFormat::kDepthR32F`) plus `DepthColorParams` (near/far/invert/
colormap), and `MediaViewerWidget`'s media shader (`pixelFormat == 5`) normalizes by
[near,far] and looks the result up in a colormap LUT (`u_tex`, built once from
`pj_widgets/Colormap.h::buildColormapLut`). So there is no per-pixel CPU colormap, and
changing colormap/range/invert is a uniform write with no re-decode. The colormap set
+ its math live **once** in `pj_widgets/Colormap.h` (`Colormap` enum, `colorFor()`,
`buildColormapLut()`, and `colormapGlsl()` for the 3D in-shader path), shared with the
3D pointcloud colouring so a scalar maps to the same colour in both views. The Qt-free
core stays decoupled: `DepthPipelineSource::setColormap` takes an opaque `uint8_t`
colormap id (== `Colormap` value == LUT row), not the enum.

## Docs

Read in this order:

- [`docs/REQUIREMENTS.md`](./docs/REQUIREMENTS.md) — the WHAT: scope, use cases, functional requirements, module contract.
- [`docs/ARCHITECTURE.md`](./docs/ARCHITECTURE.md) — the HOW: module structure, data flow, scrub architecture, codec pipeline, threading model, key invariants.
- [`docs/TECHNICAL_NOTES.md`](./docs/TECHNICAL_NOTES.md) — domain background: Qt 6.8/QRhi specifics, codec caveats, HW-accel matrices, lessons learned.
- [`docs/datatypes_2D.md`](./docs/datatypes_2D.md) — canonical scene-type catalog and implementation status.

## Key headers

- `core/include/pj_scene2d_core/media_source.h` — the uniform `setTimestamp`/`takeFrame` frame-delivery contract everything plugs into.
- `core/include/pj_scene2d_core/image_pipeline_source.h`, `streaming_video_source.h`, `depth_pipeline_source.h`, `scene_pipeline_source.h`, `composite_media_source.h` — the concrete `MediaSource` implementations.
- `core/include/pj_scene2d_core/image_resolve.h` — `resolveImage`, the single seam that turns a store entry into an `sdk::Image` (parser or canonical codec); shared by the image and depth pipeline sources.
- `core/include/pj_scene2d_core/streaming_video_decoder.h`, `ffmpeg_decoder.h` — GOP-aware streaming video decode on top of FFmpeg.
- `core/include/pj_scene2d_core/overlay_geometry.h` — backend-agnostic tessellation of annotation overlays (lines/points/fills/circles) into GPU vertex data; stroke width scales with zoom but is **floored at 1px on screen** so edges never go sub-pixel and vanish.
- `widgets/include/pj_scene2d_widgets/media_viewer_widget.h` — the `QRhiWidget` renderer (YUV/RGB pipelines + annotation overlays).
- `widgets/include/pj_scene2d_widgets/Scene2DDockWidget.h` — the dock widget wiring layers into `pj_scene_common`'s `SceneDockWidget`.

## Working conventions

- Run the module tests and make sure they pass before any commit.
- Keep the markdown files in `docs/` in sync with the code you change (see the
  root `CLAUDE.md` freshness discipline); record hard-won lessons in
  `TECHNICAL_NOTES.md`, especially after long debugging sessions.
- Think test-first: prefer automated reproduction over asking the user to run
  the app. When an issue is reported, reproduce it in a test before fixing it.
