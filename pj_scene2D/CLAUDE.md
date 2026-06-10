# pj_scene2D

2D media/scene widget family: decoding and GPU display of images, video,
depth maps, and pixel-space annotation overlays, synchronized with the global
timeline. Ships as two CMake targets with a strict dependency direction:
`pj_scene2d_core` (Qt-free C++20 — decoders, `MediaSource` pipeline sources,
keyframe indexing, compositing) and `pj_scene2d_widgets` (Qt — the
`QRhiWidget`-based `MediaViewerWidget`, the `Scene2DDockWidget` layer stack on
top of `pj_scene_common`). pj_scene2D is a read-only consumer of
`pj_datastore::ObjectStore`; it never writes to storage.

## Docs

Read in this order:

- [`docs/REQUIREMENTS.md`](./docs/REQUIREMENTS.md) — the WHAT: scope, use cases, functional requirements, module contract.
- [`docs/ARCHITECTURE.md`](./docs/ARCHITECTURE.md) — the HOW: module structure, data flow, scrub architecture, codec pipeline, threading model, key invariants.
- [`docs/TECHNICAL_NOTES.md`](./docs/TECHNICAL_NOTES.md) — domain background: Qt 6.8/QRhi specifics, codec caveats, HW-accel matrices, lessons learned.
- [`docs/datatypes_2D.md`](./docs/datatypes_2D.md) — canonical scene-type catalog and implementation status.

## Key headers

- `core/include/pj_scene2d_core/media_source.h` — the uniform `setTimestamp`/`takeFrame` frame-delivery contract everything plugs into.
- `core/include/pj_scene2d_core/image_pipeline_source.h`, `streaming_video_source.h`, `depth_pipeline_source.h`, `scene_pipeline_source.h`, `composite_media_source.h` — the concrete `MediaSource` implementations.
- `core/include/pj_scene2d_core/streaming_video_decoder.h`, `ffmpeg_decoder.h` — GOP-aware streaming video decode on top of FFmpeg.
- `widgets/include/pj_scene2d_widgets/media_viewer_widget.h` — the `QRhiWidget` renderer (YUV/RGB pipelines + annotation overlays).
- `widgets/include/pj_scene2d_widgets/Scene2DDockWidget.h` — the dock widget wiring layers into `pj_scene_common`'s `SceneDockWidget`.

## Working conventions

- Run the module tests and make sure they pass before any commit.
- Keep the markdown files in `docs/` in sync with the code you change (see the
  root `CLAUDE.md` freshness discipline); record hard-won lessons in
  `TECHNICAL_NOTES.md`, especially after long debugging sessions.
- Think test-first: prefer automated reproduction over asking the user to run
  the app. When an issue is reported, reproduce it in a test before fixing it.
