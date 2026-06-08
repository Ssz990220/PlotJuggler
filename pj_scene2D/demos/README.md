# pj_scene2D demos

The Qt demos require Qt 6.8+ and are built only when the `pj_scene2d_widgets` target is available. The `extract_frame` CLI tool has no Qt dependency and is always built (it links only pj_scene2d_core, pj_datastore, mcap, and libjpeg-turbo).
Video demos additionally require FFmpeg (libavcodec, libavformat).

## Building

```bash
./build.sh            # RelWithDebInfo
```

## Running

### Image viewer (MCAP)

Browse JPEG images from an MCAP file with play/scrub controls.

```bash
./build/pj_scene2D/demos/mcap_image_viewer pj_scene2D/testdata/test_images.mcap
```

### Multi-channel viewer

Display multiple image topics side-by-side (e.g. RGB + depth).

```bash
./build/pj_scene2D/demos/multi_channel_viewer pj_scene2D/testdata/test_images.mcap
```

### Simulated stream

Live 30 Hz synthetic JPEG stream with 3-second retention buffer.
Toggle between live mode and scrub mode.

```bash
./build/pj_scene2D/demos/simulated_stream
```

### MP4 video player

File-based video playback with slider scrub via FileVideoSource.

```bash
./build/pj_scene2D/demos/mp4_video_viewer pj_scene2D/testdata/test_480p.mp4
./build/pj_scene2D/demos/mp4_video_viewer pj_scene2D/testdata/test_1080p.mp4
```

### Video stream demo

Simulates live H.264 streaming by demuxing an MP4 and pushing
packets into ObjectStore at real-time rate. Live/scrub toggle.

```bash
./build/pj_scene2D/demos/video_stream_demo pj_scene2D/testdata/test_480p.mp4
```

### Extract frame (CLI)

Extract a single frame from an MCAP file and save as PPM.

```bash
./build/pj_scene2D/demos/extract_frame pj_scene2D/testdata/test_images.mcap [frame_index]
```
