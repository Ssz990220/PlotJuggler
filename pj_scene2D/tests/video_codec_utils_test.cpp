// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Unit tests for the codec-generic helpers that let StreamingVideoDecoder handle
// more than H.264. The format->codec_id map and the per-codec keyframe oracles
// are pure functions over small byte buffers, so they are validated here on
// hand-built synthetic NAL/OBU streams — there are no HEVC/AV1 sample clips in
// this environment and the linked FFmpeg has no encoder to synthesize one, so
// end-to-end decode of those codecs is necessarily a manual / fixture-gated step.

#include "pj_scene2d_core/video_codec_utils.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

using PJ::isVideoKeyframe;
using PJ::makeVideoCodecParams;
using PJ::videoCodecIdFromFormat;

bool keyframe(int codec_id, const std::vector<uint8_t>& bytes) {
  return isVideoKeyframe(codec_id, bytes.data(), bytes.size());
}

// --- format -> AVCodecID ----------------------------------------------------

TEST(VideoCodecUtils, MapsKnownFormatsToCodecIds) {
  EXPECT_EQ(videoCodecIdFromFormat("h264"), AV_CODEC_ID_H264);
  // Canonical Foxglove "h265" and the FFmpeg name "hevc" both resolve to HEVC.
  EXPECT_EQ(videoCodecIdFromFormat("h265"), AV_CODEC_ID_HEVC);
  EXPECT_EQ(videoCodecIdFromFormat("hevc"), AV_CODEC_ID_HEVC);
  EXPECT_EQ(videoCodecIdFromFormat("av1"), AV_CODEC_ID_AV1);
}

TEST(VideoCodecUtils, FormatMatchIsCaseInsensitive) {
  EXPECT_EQ(videoCodecIdFromFormat("H264"), AV_CODEC_ID_H264);
  EXPECT_EQ(videoCodecIdFromFormat("H265"), AV_CODEC_ID_HEVC);
  EXPECT_EQ(videoCodecIdFromFormat("AV1"), AV_CODEC_ID_AV1);
}

TEST(VideoCodecUtils, UnknownOrEmptyFormatIsNone) {
  EXPECT_EQ(videoCodecIdFromFormat(""), AV_CODEC_ID_NONE);
  EXPECT_EQ(videoCodecIdFromFormat("totally-not-a-codec"), AV_CODEC_ID_NONE);
}

TEST(VideoCodecUtils, NonWhitelistedCodecsAreNone) {
  // VP9/VP8/MJPEG have decoders in this FFmpeg build but no keyframe oracle, so
  // they are intentionally NOT resolved — a seekless stream is worse than a clear
  // "unsupported" error. Only h264/hevc/av1 (decoder + oracle) resolve.
  EXPECT_EQ(videoCodecIdFromFormat("vp9"), AV_CODEC_ID_NONE);
  EXPECT_EQ(videoCodecIdFromFormat("vp8"), AV_CODEC_ID_NONE);
  EXPECT_EQ(videoCodecIdFromFormat("mjpeg"), AV_CODEC_ID_NONE);
}

// --- makeVideoCodecParams ---------------------------------------------------

TEST(VideoCodecUtils, MakeParamsSetsCodecIdAndNoExtradata) {
  AVCodecParameters* params = makeVideoCodecParams(AV_CODEC_ID_H264);
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(params->codec_type, AVMEDIA_TYPE_VIDEO);
  EXPECT_EQ(params->codec_id, AV_CODEC_ID_H264);
  EXPECT_EQ(params->extradata, nullptr);
  EXPECT_EQ(params->extradata_size, 0);
  avcodec_parameters_free(&params);
}

TEST(VideoCodecUtils, MakeParamsRejectsNoneCodec) {
  EXPECT_EQ(makeVideoCodecParams(AV_CODEC_ID_NONE), nullptr);
}

// --- H.264 keyframe oracle (Annex-B, 1-byte NAL header, IDR = type 5) --------

TEST(VideoCodecUtils, H264IdrIsKeyframe) {
  // 4-byte start code + NAL header 0x65 (nal_ref_idc=3, nal_unit_type=5 IDR).
  EXPECT_TRUE(keyframe(AV_CODEC_ID_H264, {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x80}));
}

TEST(VideoCodecUtils, H264NonIdrSliceIsNotKeyframe) {
  // NAL header 0x41 (nal_unit_type=1, non-IDR coded slice).
  EXPECT_FALSE(keyframe(AV_CODEC_ID_H264, {0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x00}));
}

// --- HEVC keyframe oracle (Annex-B, 2-byte NAL header, IRAP = types 16..21) --

TEST(VideoCodecUtils, HevcIrapIsKeyframe) {
  // NAL header byte0 0x26 -> nal_unit_type = (0x26>>1)&0x3F = 19 (IDR_W_RADL, an
  // IRAP picture); byte1 0x01 = layer_id 0 / temporal_id_plus1 1.
  EXPECT_TRUE(keyframe(AV_CODEC_ID_HEVC, {0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xaf}));
}

TEST(VideoCodecUtils, HevcTrailSliceIsNotKeyframe) {
  // byte0 0x02 -> type = (0x02>>1)&0x3F = 1 (TRAIL_R, a non-IRAP slice).
  EXPECT_FALSE(keyframe(AV_CODEC_ID_HEVC, {0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0xaf}));
}

// --- AV1 keyframe oracle (OBU stream; keyframe carries a Sequence-Header OBU) -

TEST(VideoCodecUtils, Av1SequenceHeaderObuIsKeyframe) {
  // OBU header 0x0a: obu_type=(0x0a>>3)&0xf=1 (SEQUENCE_HEADER), has_size_field=1;
  // LEB128 size 0x01; one payload byte.
  EXPECT_TRUE(keyframe(AV_CODEC_ID_AV1, {0x0a, 0x01, 0xff}));
}

TEST(VideoCodecUtils, Av1TemporalDelimiterThenSequenceHeaderIsKeyframe) {
  // Temporal-delimiter OBU (type 2, empty) must be walked over to reach the
  // sequence header OBU that follows — exercises the LEB128 OBU skip.
  EXPECT_TRUE(keyframe(AV_CODEC_ID_AV1, {0x12, 0x00, 0x0a, 0x01, 0xff}));
}

TEST(VideoCodecUtils, Av1FrameObuWithoutSequenceHeaderIsNotKeyframe) {
  // OBU header 0x32: obu_type=(0x32>>3)&0xf=6 (FRAME), has_size_field=1.
  EXPECT_FALSE(keyframe(AV_CODEC_ID_AV1, {0x32, 0x01, 0xff}));
}

// --- codecs without an oracle / unsupported ---------------------------------

TEST(VideoCodecUtils, UnknownCodecHasNoKeyframeOracle) {
  // A codec we have no oracle for must report "not a keyframe" (degrades to
  // "no keyframe found") rather than mis-classifying bytes.
  EXPECT_FALSE(keyframe(AV_CODEC_ID_NONE, {0x00, 0x00, 0x00, 0x01, 0x65}));
  EXPECT_FALSE(keyframe(AV_CODEC_ID_VP9, {0x00, 0x00, 0x00, 0x01, 0x65}));
}

}  // namespace
