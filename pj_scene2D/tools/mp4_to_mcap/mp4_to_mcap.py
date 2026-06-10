#!/usr/bin/env python3
# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: Apache-2.0
"""Portable MP4 -> MCAP(foxglove.CompressedVideo) converter.

Splits a video file's compressed stream into one ``foxglove.CompressedVideo``
protobuf message per frame, written to an MCAP file that PlotJuggler's
streaming-video pipeline (and Foxglove Studio) consumes.

    python3 -m venv .venv && . .venv/bin/activate
    pip install av mcap mcap-protobuf-support foxglove-schemas-protobuf protobuf
    python mp4_to_mcap.py input.mp4 output.mcap [--topic /camera/video] [--frame-id camera]

``foxglove.CompressedVideo`` fields: ``timestamp`` (google.protobuf.Timestamp),
``frame_id`` (string), ``data`` (bytes), ``format`` (string). The mcap-protobuf
writer registers the schema (a FileDescriptorSet) automatically, so the output
is a self-describing protobuf MCAP — `message_encoding=protobuf`, schema name
``foxglove.CompressedVideo``, which PlotJuggler routes through parser_protobuf.

H.264/H.265 packets are converted to Annex-B (the ``data`` payload); AV1/VP9 are
passed through. Frames are written in decode (DTS) order with timestamps rebased
to zero (MCAP logTime is unsigned).
"""
from __future__ import annotations

import argparse
from fractions import Fraction

NS_PER_S = 1_000_000_000

# av codec name -> (CompressedVideo.format, Annex-B bitstream filter or None)
_CODEC_MAP = {
    "h264": ("h264", "h264_mp4toannexb"),
    "hevc": ("h265", "hevc_mp4toannexb"),
    "av1": ("av1", None),
    "vp9": ("vp9", None),
}


def demux_frames(in_path: str) -> tuple[str, list[tuple[int, int, bytes]]]:
    """Return ``(format, [(dts_ns, pts_ns, data_bytes), ...])`` in decode order.

    H.264/H.265 are converted to Annex-B via a bitstream filter; AV1/VP9 pass
    through. Timestamps are in nanoseconds (not yet rebased).
    """
    import av
    from av.bitstream import BitStreamFilterContext

    with av.open(in_path) as container:
        streams = [s for s in container.streams if s.type == "video"]
        if not streams:
            raise SystemExit(f"no video stream in {in_path}")
        stream = streams[0]
        codec_name = stream.codec_context.name
        if codec_name not in _CODEC_MAP:
            raise SystemExit(f"unsupported video codec: {codec_name} (need h264/h265/av1/vp9)")
        fmt, bsf_name = _CODEC_MAP[codec_name]
        time_base = stream.time_base or Fraction(1, NS_PER_S)

        def to_ns(units):
            return None if units is None else int(Fraction(units) * time_base * NS_PER_S)

        frames: list[tuple[int, int, bytes]] = []

        def collect(pkt):
            data = bytes(pkt)
            if not data:  # flush/sentinel packets carry no payload
                return
            pts = to_ns(pkt.pts if pkt.pts is not None else pkt.dts) or 0
            dts = to_ns(pkt.dts if pkt.dts is not None else pkt.pts) or 0
            frames.append((dts, pts, data))

        bsf = BitStreamFilterContext(bsf_name, stream) if bsf_name else None
        for packet in container.demux(stream):
            if bsf is not None:
                for out_pkt in bsf.filter(packet) or ():
                    collect(out_pkt)
            elif packet.size:
                collect(packet)
        if bsf is not None:
            for out_pkt in bsf.flush() or ():  # flush() may return None in PyAV 17
                collect(out_pkt)

    return fmt, frames


def convert(in_path: str, out_path: str, topic: str, frame_id: str) -> int:
    """Convert ``in_path`` -> ``out_path``; returns the number of frames written."""
    from foxglove_schemas_protobuf.CompressedVideo_pb2 import CompressedVideo
    from mcap_protobuf.writer import Writer

    fmt, frames = demux_frames(in_path)
    if not frames:
        raise SystemExit("no frames extracted")

    # Rebase timestamps to the first DTS: keeps decode order monotonic and
    # non-negative (MCAP logTime is unsigned).
    base = frames[0][0]
    with open(out_path, "wb") as fp:
        writer = Writer(fp)
        for i, (dts, pts, data) in enumerate(frames):
            log_time = max(0, dts - base)
            pub_time = max(0, pts - base)
            msg = CompressedVideo(frame_id=frame_id, data=data, format=fmt)
            msg.timestamp.FromNanoseconds(pub_time)
            writer.write_message(
                topic=topic,
                message=msg,
                log_time=log_time,
                publish_time=pub_time,
                sequence=i,
            )
        writer.finish()
    return len(frames)


def main() -> int:
    parser = argparse.ArgumentParser(description="MP4 -> MCAP(foxglove.CompressedVideo) converter")
    parser.add_argument("input", help="input video file (mp4/mkv/...)")
    parser.add_argument("output", help="output .mcap file")
    parser.add_argument("--topic", default="/camera/video", help="channel / topic name")
    parser.add_argument("--frame-id", default="camera", help="CompressedVideo.frame_id")
    args = parser.parse_args()

    n = convert(args.input, args.output, args.topic, args.frame_id)
    print(f"wrote {n} foxglove.CompressedVideo frames to {args.output} (topic '{args.topic}')")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
