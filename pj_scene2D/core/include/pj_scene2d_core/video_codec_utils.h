#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

struct AVCodecParameters;

namespace PJ {

/// Codec-generic helpers for the streaming VideoFrame decode path. The codec is
/// carried on the wire as sdk::VideoFrame::format (lowercase Foxglove-style:
/// "h264", "h265", "vp9", "av1"); these map it to FFmpeg and answer the
/// per-frame keyframe question without a demuxer (raw Annex-B / OBU entries).

/// Resolve a VideoFrame.format string to an FFmpeg AVCodecID (returned as int to
/// keep this header FFmpeg-free) for the codecs this decoder FULLY supports —
/// "h264", "h265"/"hevc", "av1" (the ones with a keyframe oracle below). Aliases
/// "h265" to FFmpeg's "hevc" and prefers software "libdav1d" for "av1". Any other
/// format returns AV_CODEC_ID_NONE (0) — including codecs this FFmpeg could decode
/// but cannot seek (e.g. "vp9": a decoder exists, but there is no keyframe oracle,
/// so the keyframe index would stay empty and every scrub would fail). The caller
/// surfaces a clear "unsupported codec" error rather than half-decoding such a
/// stream or mis-decoding an unknown one as H.264.
int videoCodecIdFromFormat(std::string_view format);

/// True if `data` (the Annex-B NAL stream for H.264/HEVC, or the OBU temporal
/// unit for AV1, of exactly one frame) is a keyframe / random-access point, for
/// the given AVCodecID. Codec-dispatched: H.264 IDR (NAL type 5), HEVC IRAP (NAL
/// types 16..21), AV1 (carries a Sequence-Header OBU). Returns false for codecs
/// without a keyframe oracle (so they degrade to "no keyframe found").
bool isVideoKeyframe(int codec_id, const uint8_t* data, size_t size);

/// AVCodecParameters{codec_type = VIDEO, codec_id}. No extradata is set here;
/// callers that have the first keyframe's bytes should prime it via
/// primeKeyframeParamSets() before opening. Returns nullptr on alloc failure or
/// codec_id == AV_CODEC_ID_NONE. Caller frees with avcodec_parameters_free(&p).
AVCodecParameters* makeVideoCodecParams(int codec_id);

/// Copy the parameter-set NALs out of an Annex-B keyframe (H.264 SPS/PPS, HEVC
/// VPS/SPS/PPS) into `params->extradata`, so the decoder opens fully configured —
/// exactly as if a demuxer had provided the codec config. Returns true when at
/// least one parameter set was installed; a no-op for AV1 (OBU stream — libdav1d
/// consumes the in-band sequence header reliably) and for keyframes carrying no
/// parameter sets.
///
/// Why this matters: opening from the codec id alone ("paramsets are in-band
/// anyway") is NOT equivalent. libavcodec's H.264 decoder, configured only by
/// the in-band SPS/PPS of the first access unit, can silently DROP the first
/// B-frame(s) of the stream — observed with screen-recorder output whose
/// keyframe AU carries duplicated parameter sets (SPS PPS SEI SPS PPS IDR).
/// The dropped frame makes the presentation-ordered serve fail with "forward
/// decode produced no frame" for that timestamp. Priming extradata removes the
/// drop entirely.
bool primeKeyframeParamSets(AVCodecParameters* params, const uint8_t* data, size_t size);

}  // namespace PJ
