// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/ffmpeg_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <cstring>
#include <string>

#include "pj_base/sdk/platform.hpp"

namespace PJ {

namespace {

// Open an FFmpeg hardware-device context for `type`, or nullptr if none works.
//
// VAAPI is special: its device IS a specific DRM render node. Passing nullptr
// opens the *default* node (/dev/dri/renderD128), which on a multi-GPU machine
// is often a GPU with no VAAPI driver (e.g. an NVIDIA dGPU on the proprietary
// driver) while the VAAPI-capable GPU sits on renderD129. So for VAAPI we honor
// an explicit PJ_VAAPI_DEVICE override, then walk the render nodes and take the
// first that initialises — making HW decode "just work" on iGPU+dGPU laptops
// regardless of vendor or enumeration order. Every other backend (CUDA,
// D3D11VA, VideoToolbox, …) has no node concept and uses the library default.
// The render-node walk is Linux-only and guarded accordingly.
AVBufferRef* tryHwDevice(AVHWDeviceType type) {
  AVBufferRef* ctx = nullptr;
#ifdef __linux__
  if (type == AV_HWDEVICE_TYPE_VAAPI) {
    if (const auto device = sdk::getEnv("PJ_VAAPI_DEVICE"); device) {
      if (av_hwdevice_ctx_create(&ctx, type, device->c_str(), nullptr, 0) >= 0) {
        return ctx;
      }
      ctx = nullptr;
    }
    // DRM render nodes are numbered from 128. Probe a generous range and take
    // the first node whose VAAPI driver initialises. tryHwDevice runs once per
    // stream (at decoder open), so the walk is not on any hot path.
    for (int node = 128; node < 192; ++node) {
      const std::string path = "/dev/dri/renderD" + std::to_string(node);
      if (av_hwdevice_ctx_create(&ctx, type, path.c_str(), nullptr, 0) >= 0) {
        return ctx;
      }
      ctx = nullptr;
    }
    return nullptr;
  }
#endif
  if (av_hwdevice_ctx_create(&ctx, type, nullptr, nullptr, 0) >= 0) {
    return ctx;
  }
  return nullptr;
}

// The hardware surface pixel format this codec exposes for `type` (e.g.
// AV_PIX_FMT_VAAPI), or AV_PIX_FMT_NONE if the codec has no HW config for it —
// e.g. a profile the GPU can't decode, in which case we stay on software.
AVPixelFormat hwPixelFormatFor(const AVCodec* codec, AVHWDeviceType type) {
  for (int i = 0;; ++i) {
    const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
    if (config == nullptr) {
      return AV_PIX_FMT_NONE;
    }
    if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 && config->device_type == type) {
      return config->pix_fmt;
    }
  }
}

// get_format callback: deterministically pick the hardware surface format when
// the decoder offers it, otherwise defer to FFmpeg's default (software) choice
// so the stream still decodes on machines/codecs without that HW path. The
// wanted HW pixel format is carried in codec_ctx->opaque (set in open()), which
// keeps this a free function so ffmpeg_decoder.h stays free of FFmpeg includes.
AVPixelFormat pickHwFormat(AVCodecContext* ctx, const AVPixelFormat* formats) {
  const auto wanted = static_cast<AVPixelFormat>(reinterpret_cast<std::intptr_t>(ctx->opaque));
  for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
    if (*p == wanted) {
      return *p;
    }
  }
  return avcodec_default_get_format(ctx, formats);
}

}  // namespace

FfmpegDecoder::FfmpegDecoder() = default;

FfmpegDecoder::~FfmpegDecoder() {
  if (sws_ctx_ != nullptr) {
    sws_freeContext(sws_ctx_);
  }
  if (codec_ctx_ != nullptr) {
    avcodec_free_context(&codec_ctx_);
  }
  if (hw_device_ctx_ != nullptr) {
    av_buffer_unref(&hw_device_ctx_);
  }
}

bool FfmpegDecoder::open(const AVCodecParameters* params) {
  if (codec_ctx_ != nullptr) {
    avcodec_free_context(&codec_ctx_);
    codec_ctx_ = nullptr;
  }
  if (hw_device_ctx_ != nullptr) {
    av_buffer_unref(&hw_device_ctx_);
    hw_device_ctx_ = nullptr;
  }
  if (sws_ctx_ != nullptr) {
    sws_freeContext(sws_ctx_);
    sws_ctx_ = nullptr;
  }

  const AVCodec* codec = avcodec_find_decoder(params->codec_id);
  if (codec == nullptr) {
    return false;
  }

  codec_ctx_ = avcodec_alloc_context3(codec);
  if (codec_ctx_ == nullptr) {
    return false;
  }

  if (avcodec_parameters_to_context(codec_ctx_, params) < 0) {
    avcodec_free_context(&codec_ctx_);
    return false;
  }

  // Try every hardware backend this FFmpeg build exposes, in iteration order,
  // and keep the first that (a) has a HW decode path for this codec and (b)
  // opens a device. Iterating av_hwdevice_iterate_types() rather than a fixed
  // {VAAPI, CUDA} list means the same code transparently covers whatever a given
  // build/platform offers — VAAPI here, CUDA if FFmpeg is built with it, or
  // D3D11VA / VideoToolbox on Windows / macOS — with no edits. Checking the
  // codec's HW config first skips opening devices for backends that can't decode
  // this stream anyway, and lets a backend that opens but lacks the codec config
  // yield to the next one instead of dropping straight to software. If none
  // qualify, hw_device_ctx_ stays null and decoding runs in software.
  AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_NONE;
  AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
  for (AVHWDeviceType type = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE); type != AV_HWDEVICE_TYPE_NONE;
       type = av_hwdevice_iterate_types(type)) {
    const AVPixelFormat pix_fmt = hwPixelFormatFor(codec, type);
    if (pix_fmt == AV_PIX_FMT_NONE) {
      continue;
    }
    hw_device_ctx_ = tryHwDevice(type);
    if (hw_device_ctx_ != nullptr) {
      hw_type = type;
      hw_pix_fmt = pix_fmt;
      codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
      break;
    }
  }

  // Pin the chosen HW surface format via get_format so the hardware path is
  // selected deterministically; software stays the fallback when no device won.
  if (hw_device_ctx_ != nullptr) {
    codec_ctx_->opaque = reinterpret_cast<void*>(static_cast<std::intptr_t>(hw_pix_fmt));
    codec_ctx_->get_format = &pickHwFormat;
  }

  codec_ctx_->thread_count = 4;

  if (hw_device_ctx_ != nullptr) {
    fprintf(stderr, "[FfmpegDecoder] HW accel: %s\n", av_hwdevice_get_type_name(hw_type));
  } else {
    fprintf(stderr, "[FfmpegDecoder] HW accel: none (software decode)\n");
  }

  if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
    avcodec_free_context(&codec_ctx_);
    if (hw_device_ctx_ != nullptr) {
      av_buffer_unref(&hw_device_ctx_);
    }
    return false;
  }

  return true;
}

int FfmpegDecoder::sendPacket(const uint8_t* data, size_t size, int64_t pts, int64_t dts, SendPolicy policy) {
  AVPacket* pkt = av_packet_alloc();
  // We point pkt->data at the caller's buffer and never set pkt->buf, so
  // avcodec_send_packet takes its own (ref-counted) copy of the bytes before it
  // returns. The caller's buffer therefore does NOT need to outlive this call —
  // which is what lets the streaming decoder feed zero-copy spans that alias a
  // transient ObjectStore entry. Do not set pkt->buf to an external ref.
  pkt->data = const_cast<uint8_t*>(data);
  pkt->size = static_cast<int>(size);
  // pts carries the true presentation timestamp (so the decoded frame's pts comes
  // back in presentation order); dts is the decode-order timestamp. They differ
  // for B-frame streams — feeding pts==dts would scramble the output ordering.
  pkt->pts = pts;
  pkt->dts = dts;

  int ret = avcodec_send_packet(codec_ctx_, pkt);
  if (ret == AVERROR(ENOMEM) && policy != SendPolicy::kDecodeSkip) {
    // Surface pool exhaustion — flush and retry once
    avcodec_flush_buffers(codec_ctx_);
    ret = avcodec_send_packet(codec_ctx_, pkt);
  }
  if (ret == AVERROR(EAGAIN) && policy == SendPolicy::kCombinedDecode) {
    // Legacy combined calls drain-and-discard on EAGAIN before retrying.
    AVFrame* drain_frame = av_frame_alloc();
    avcodec_receive_frame(codec_ctx_, drain_frame);
    av_frame_free(&drain_frame);
    ret = avcodec_send_packet(codec_ctx_, pkt);
  }
  av_packet_free(&pkt);
  return ret;
}

Expected<DecodedFrame> FfmpegDecoder::receiveFrame(const std::function<bool(int64_t)>& want) {
  AVFrame* frame = av_frame_alloc();
  int ret = avcodec_receive_frame(codec_ctx_, frame);
  if (ret < 0) {
    av_frame_free(&frame);
    if (ret == AVERROR(EAGAIN)) {
      return unexpected("need more packets");
    }
    return unexpected("avcodec_receive_frame failed");
  }
  if (want && !want(frame->pts)) {
    av_frame_free(&frame);
    return DecodedFrame{};  // isNull() == true: state advanced, no pixels paid
  }
  auto result = avFrameToDecodedFrame(frame);
  if (result.has_value()) {
    result->pts = frame->pts;
  }
  av_frame_free(&frame);
  return result;
}

Expected<DecodedFrame> FfmpegDecoder::receiveFiltered(const std::function<bool(int64_t)>& want) {
  if (codec_ctx_ == nullptr) {
    return unexpected("decoder not open");
  }
  return receiveFrame(want);
}

bool FfmpegDecoder::sendOnly(const uint8_t* data, size_t size, int64_t pts, int64_t dts) {
  if (codec_ctx_ == nullptr) {
    return false;
  }
  // Streaming callers pre-drain the output queue; preserve ENOMEM retry only.
  int ret = sendPacket(data, size, pts, dts, SendPolicy::kStreamingPump);
  return ret >= 0;
}

Expected<DecodedFrame> FfmpegDecoder::decodePacket(
    const uint8_t* data, size_t size, int64_t pts, int64_t dts, const std::function<bool(int64_t)>& want,
    const CancelTokenPtr& cancel) {
  if (codec_ctx_ == nullptr) {
    return unexpected("decoder not open");
  }

  if (sendPacket(data, size, pts, dts, SendPolicy::kCombinedDecode) < 0) {
    return unexpected("avcodec_send_packet failed");
  }

  if (cancel != nullptr && cancel->isCancelled()) {
    return unexpected("cancelled");
  }

  return receiveFrame(want);
}

Expected<DecodedFrame> FfmpegDecoder::decode(
    const uint8_t* data, size_t size, int64_t pts, int64_t dts, const CancelTokenPtr& cancel) {
  return decodePacket(data, size, pts, dts, std::function<bool(int64_t)>{}, cancel);
}

Expected<DecodedFrame> FfmpegDecoder::decodeFiltered(
    const uint8_t* data, size_t size, int64_t pts, int64_t dts, const std::function<bool(int64_t)>& want) {
  return decodePacket(data, size, pts, dts, want, CancelTokenPtr{});
}

int64_t FfmpegDecoder::decodeSkip(const uint8_t* data, size_t size, int64_t pts, int64_t dts) {
  if (codec_ctx_ == nullptr) {
    return -1;
  }

  // Preserve decodeSkip's historical no-retry send path, including EAGAIN.
  int ret = sendPacket(data, size, pts, dts, SendPolicy::kDecodeSkip);
  if (ret < 0 && ret != AVERROR(EAGAIN)) {
    return -1;
  }

  AVFrame* frame = av_frame_alloc();
  ret = avcodec_receive_frame(codec_ctx_, frame);
  int64_t result_pts = (ret >= 0) ? frame->pts : -1;
  av_frame_free(&frame);
  return result_pts;
}

void FfmpegDecoder::flush() {
  if (codec_ctx_ != nullptr) {
    avcodec_flush_buffers(codec_ctx_);
  }
}

std::vector<DecodedFrame> FfmpegDecoder::drain() {
  std::vector<DecodedFrame> frames;
  if (codec_ctx_ == nullptr) {
    return frames;
  }

  // Send NULL packet to drain
  avcodec_send_packet(codec_ctx_, nullptr);

  AVFrame* frame = av_frame_alloc();
  while (avcodec_receive_frame(codec_ctx_, frame) >= 0) {
    auto result = avFrameToDecodedFrame(frame);
    if (result.has_value()) {
      result->pts = frame->pts;
      frames.push_back(std::move(*result));
    }
    av_frame_unref(frame);
  }
  av_frame_free(&frame);
  return frames;
}

int FfmpegDecoder::width() const {
  return codec_ctx_ != nullptr ? codec_ctx_->width : 0;
}

int FfmpegDecoder::height() const {
  return codec_ctx_ != nullptr ? codec_ctx_->height : 0;
}

Expected<DecodedFrame> FfmpegDecoder::avFrameToDecodedFrame(AVFrame* frame) {
  AVFrame* sw_frame = frame;
  AVFrame* tmp_frame = nullptr;

  // Transfer HW frame to CPU if needed
  if (frame->hw_frames_ctx != nullptr) {
    tmp_frame = av_frame_alloc();
    if (av_hwframe_transfer_data(tmp_frame, frame, 0) < 0) {
      av_frame_free(&tmp_frame);
      return unexpected("HW frame transfer failed");
    }
    sw_frame = tmp_frame;
  }

  int w = sw_frame->width;
  int h = sw_frame->height;
  auto src_fmt = static_cast<AVPixelFormat>(sw_frame->format);

  // Output YUV420P — no color conversion on CPU.
  // The GPU shader handles YUV→RGB with the correct BT.709 matrix.
  if (src_fmt == AV_PIX_FMT_YUV420P) {
    // Already YUV420P — just copy planes to contiguous buffer
    int uv_w = (w + 1) / 2;
    int uv_h = (h + 1) / 2;
    auto buf_size = expectedBufferSize(w, h, PixelFormat::kYUV420P);
    auto pixels = std::make_shared<std::vector<uint8_t>>(buf_size);
    uint8_t* dst = pixels->data();
    int y_size = w * h;
    int uv_size = uv_w * uv_h;

    // Y plane
    for (int row = 0; row < h; ++row) {
      std::memcpy(dst + row * w, sw_frame->data[0] + row * sw_frame->linesize[0], static_cast<size_t>(w));
    }
    // U plane
    for (int row = 0; row < uv_h; ++row) {
      std::memcpy(
          dst + y_size + row * uv_w, sw_frame->data[1] + row * sw_frame->linesize[1], static_cast<size_t>(uv_w));
    }
    // V plane
    for (int row = 0; row < uv_h; ++row) {
      std::memcpy(
          dst + y_size + uv_size + row * uv_w, sw_frame->data[2] + row * sw_frame->linesize[2],
          static_cast<size_t>(uv_w));
    }

    if (tmp_frame != nullptr) {
      av_frame_free(&tmp_frame);
    }

    DecodedFrame result;
    result.pixels = std::move(pixels);
    result.width = w;
    result.height = h;
    result.format = PixelFormat::kYUV420P;
    return result;
  }

  // Non-YUV420P (NV12, etc.) — convert to YUV420P via sws_scale
  if (sws_ctx_ == nullptr || w != sws_src_w_ || h != sws_src_h_ || static_cast<int>(src_fmt) != sws_src_fmt_) {
    if (sws_ctx_ != nullptr) {
      sws_freeContext(sws_ctx_);
    }
    sws_ctx_ = sws_getContext(w, h, src_fmt, w, h, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    sws_src_w_ = w;
    sws_src_h_ = h;
    sws_src_fmt_ = static_cast<int>(src_fmt);
  }

  if (sws_ctx_ == nullptr) {
    if (tmp_frame != nullptr) {
      av_frame_free(&tmp_frame);
    }
    return unexpected("sws_getContext failed for pixel format conversion");
  }

  int uv_w = (w + 1) / 2;
  int uv_h = (h + 1) / 2;
  int y_size = w * h;
  int uv_size = uv_w * uv_h;
  auto pixels = std::make_shared<std::vector<uint8_t>>(expectedBufferSize(w, h, PixelFormat::kYUV420P));
  uint8_t* dst_planes[3] = {pixels->data(), pixels->data() + y_size, pixels->data() + y_size + uv_size};
  int dst_linesize[3] = {w, uv_w, uv_w};
  sws_scale(sws_ctx_, sw_frame->data, sw_frame->linesize, 0, h, dst_planes, dst_linesize);

  if (tmp_frame != nullptr) {
    av_frame_free(&tmp_frame);
  }

  DecodedFrame result;
  result.pixels = std::move(pixels);
  result.width = w;
  result.height = h;
  result.format = PixelFormat::kYUV420P;
  return result;
}

}  // namespace PJ
