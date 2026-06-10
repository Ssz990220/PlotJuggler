#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/// Shared test helper: extract per-frame compressed video packets from an MP4.
/// H.264/HEVC are converted to Annex-B (the right *_mp4toannexb filter is picked
/// by codec, and it also prepends in-band parameter sets to keyframes); other
/// codecs (e.g. AV1 OBUs) pass through verbatim. Shared by the video test suites.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

#include <cstdint>
#include <string>
#include <vector>

#include "pj_base/types.hpp"

namespace PJ::test {

struct AnnexBPacket {
  std::vector<uint8_t> data;
  Timestamp timestamp = 0;  // PTS in nanoseconds (presentation order)
  Timestamp dts = 0;        // DTS in nanoseconds (decode order, always monotonic)
  bool keyframe = false;    // from demuxer's AV_PKT_FLAG_KEY
};

/// Extract all video packets from an MP4 file, converting to annex-B format.
/// Timestamps are converted to nanoseconds.
inline std::vector<AnnexBPacket> extractAnnexBPackets(const std::string& path) {
  std::vector<AnnexBPacket> packets;

  // Quiet FFmpeg's stderr chatter in test output. Also load-bearing for tests
  // that use no other libavutil symbol: the direct av_log_* reference keeps the
  // linker (--as-needed) from dropping the binary's libavutil DT_NEEDED — without
  // it, libavformat's transitive avutil dependency fails to resolve through the
  // executable's RUNPATH on CI (RUNPATH does not apply to grandchild deps). Do
  // not remove even if linkage currently survives via another object's avutil use.
  av_log_set_level(AV_LOG_ERROR);

  AVFormatContext* fmt_ctx = nullptr;
  if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) < 0) {
    return packets;
  }
  avformat_find_stream_info(fmt_ctx, nullptr);

  int video_idx = -1;
  for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_idx = static_cast<int>(i);
      break;
    }
  }
  if (video_idx < 0) {
    avformat_close_input(&fmt_ctx);
    return packets;
  }

  double time_base = av_q2d(fmt_ctx->streams[video_idx]->time_base);

  // Pick the Annex-B bitstream filter by codec. H.264/HEVC in MP4 carry
  // length-prefixed NAL units; the *_mp4toannexb filter rewrites them to
  // start-code Annex-B and prepends the in-band parameter sets to keyframes.
  // Codecs without an Annex-B form (AV1 OBUs) are fed through verbatim.
  const AVCodecID codec_id = fmt_ctx->streams[video_idx]->codecpar->codec_id;
  const char* bsf_name = nullptr;
  if (codec_id == AV_CODEC_ID_H264) {
    bsf_name = "h264_mp4toannexb";
  } else if (codec_id == AV_CODEC_ID_HEVC) {
    bsf_name = "hevc_mp4toannexb";
  }

  // AV1 has no Annex-B form: the mov demuxer keeps its sequence header in the
  // av1C config record (extradata = a 4-byte header + the configOBUs), not in the
  // samples. Re-prepend those configOBUs to each keyframe so it is a self-
  // contained temporal unit, as the streaming VideoFrame (LOBF) contract requires.
  const uint8_t* av1_seq_hdr = nullptr;
  int av1_seq_hdr_size = 0;
  if (codec_id == AV_CODEC_ID_AV1) {
    const AVCodecParameters* par = fmt_ctx->streams[video_idx]->codecpar;
    if (par->extradata != nullptr && par->extradata_size > 4) {
      av1_seq_hdr = par->extradata + 4;
      av1_seq_hdr_size = par->extradata_size - 4;
    }
  }

  AVBSFContext* bsf_ctx = nullptr;
  if (bsf_name != nullptr) {
    const AVBitStreamFilter* bsf = av_bsf_get_by_name(bsf_name);
    av_bsf_alloc(bsf, &bsf_ctx);
    avcodec_parameters_copy(bsf_ctx->par_in, fmt_ctx->streams[video_idx]->codecpar);
    bsf_ctx->time_base_in = fmt_ctx->streams[video_idx]->time_base;
    av_bsf_init(bsf_ctx);
  }

  AVPacket* pkt = av_packet_alloc();
  AVPacket* filtered = av_packet_alloc();

  auto emit = [&](const uint8_t* data, int size, int64_t pts, int64_t dts, bool is_key) {
    AnnexBPacket ap;
    ap.data.assign(data, data + size);
    ap.timestamp = static_cast<Timestamp>(static_cast<double>(pts) * time_base * 1'000'000'000.0);
    ap.dts = static_cast<Timestamp>(static_cast<double>(dts) * time_base * 1'000'000'000.0);
    ap.keyframe = is_key;
    packets.push_back(std::move(ap));
  };

  while (av_read_frame(fmt_ctx, pkt) >= 0) {
    if (pkt->stream_index != video_idx) {
      av_packet_unref(pkt);
      continue;
    }

    bool is_key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
    int64_t pts = pkt->pts;
    int64_t pkt_dts = pkt->dts;

    if (bsf_ctx != nullptr) {
      if (av_bsf_send_packet(bsf_ctx, pkt) >= 0) {
        while (av_bsf_receive_packet(bsf_ctx, filtered) >= 0) {
          emit(filtered->data, filtered->size, pts, pkt_dts, is_key);
          av_packet_unref(filtered);
        }
      }
    } else if (av1_seq_hdr != nullptr && is_key) {
      std::vector<uint8_t> temporal_unit;
      temporal_unit.reserve(static_cast<size_t>(av1_seq_hdr_size) + static_cast<size_t>(pkt->size));
      temporal_unit.insert(temporal_unit.end(), av1_seq_hdr, av1_seq_hdr + av1_seq_hdr_size);
      temporal_unit.insert(temporal_unit.end(), pkt->data, pkt->data + pkt->size);
      emit(temporal_unit.data(), static_cast<int>(temporal_unit.size()), pts, pkt_dts, is_key);
    } else {
      emit(pkt->data, pkt->size, pts, pkt_dts, is_key);
    }
    av_packet_unref(pkt);
  }

  av_packet_free(&pkt);
  av_packet_free(&filtered);
  if (bsf_ctx != nullptr) {
    av_bsf_free(&bsf_ctx);
  }
  avformat_close_input(&fmt_ctx);

  return packets;
}

/// The video stream's codec as a lowercase VideoFrame.format token
/// ("h264"/"h265"/"av1"), or "" if the file has no video / an unmapped codec.
/// Lets a test tell StreamingVideoDecoder which codec the extracted bytes are.
inline std::string mp4VideoFormat(const std::string& path) {
  AVFormatContext* fmt_ctx = nullptr;
  if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) < 0) {
    return "";
  }
  avformat_find_stream_info(fmt_ctx, nullptr);
  std::string format;
  for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
    if (fmt_ctx->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
      continue;
    }
    switch (fmt_ctx->streams[i]->codecpar->codec_id) {
      case AV_CODEC_ID_H264:
        format = "h264";
        break;
      case AV_CODEC_ID_HEVC:
        format = "h265";
        break;
      case AV_CODEC_ID_AV1:
        format = "av1";
        break;
      default:
        break;
    }
    break;
  }
  avformat_close_input(&fmt_ctx);
  return format;
}

}  // namespace PJ::test
