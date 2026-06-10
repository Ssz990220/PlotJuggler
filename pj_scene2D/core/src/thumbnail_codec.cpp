// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_core/thumbnail_codec.h"

#include <turbojpeg.h>

#include <cstddef>
#include <cstdint>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace PJ {

namespace {
constexpr int evenDown(int v) {
  return v & ~1;
}
}  // namespace

JpegThumbnail encodeThumbnailJpeg(const DecodedFrame& src, int max_width, int quality) {
  JpegThumbnail out;
  if (src.format != PixelFormat::kYUV420P || src.isNull() || src.width <= 0 || src.height <= 0) {
    return out;
  }
  if (max_width <= 0) {
    max_width = kThumbnailMaxWidth;
  }

  // Target dimensions: cap width, preserve aspect, keep even (YUV420 chroma).
  int dst_w = src.width;
  int dst_h = src.height;
  if (dst_w > max_width) {
    dst_h = evenDown(static_cast<int>(static_cast<int64_t>(dst_h) * max_width / dst_w));
    dst_w = evenDown(max_width);
    if (dst_h <= 0) {
      dst_h = 2;
    }
  }

  // YUV420P bytes to compress: the source verbatim (no downscale) or an sws copy.
  const uint8_t* yuv = nullptr;
  std::vector<uint8_t> scaled;
  if (dst_w == src.width && dst_h == src.height) {
    yuv = src.pixels->data();
  } else {
    scaled.resize(expectedBufferSize(dst_w, dst_h, PixelFormat::kYUV420P));
    const int src_uvw = (src.width + 1) / 2;
    const int src_uvh = (src.height + 1) / 2;
    const int dst_uvw = (dst_w + 1) / 2;
    const size_t src_y = static_cast<size_t>(src.width) * static_cast<size_t>(src.height);
    const size_t src_uv = static_cast<size_t>(src_uvw) * static_cast<size_t>(src_uvh);
    const size_t dst_y = static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h);
    const size_t dst_uv = static_cast<size_t>(dst_uvw) * static_cast<size_t>((dst_h + 1) / 2);
    const uint8_t* sbase = src.pixels->data();
    const uint8_t* src_planes[3] = {sbase, sbase + src_y, sbase + src_y + src_uv};
    const int src_strides[3] = {src.width, src_uvw, src_uvw};
    uint8_t* dst_planes[3] = {scaled.data(), scaled.data() + dst_y, scaled.data() + dst_y + dst_uv};
    const int dst_strides[3] = {dst_w, dst_uvw, dst_uvw};

    SwsContext* sws = sws_getContext(
        src.width, src.height, AV_PIX_FMT_YUV420P, dst_w, dst_h, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr,
        nullptr, nullptr);
    if (sws == nullptr) {
      return out;
    }
    sws_scale(sws, src_planes, src_strides, 0, src.height, dst_planes, dst_strides);
    sws_freeContext(sws);
    yuv = scaled.data();
  }

  tjhandle tj = tjInitCompress();
  if (tj == nullptr) {
    return out;
  }
  unsigned char* jpeg_buf = nullptr;
  unsigned long jpeg_size = 0;
  const int rc =
      tjCompressFromYUV(tj, yuv, dst_w, 1, dst_h, TJSAMP_420, &jpeg_buf, &jpeg_size, quality, TJFLAG_FASTDCT);
  if (rc == 0 && jpeg_buf != nullptr) {
    out.jpeg.assign(jpeg_buf, jpeg_buf + jpeg_size);
    out.width = dst_w;
    out.height = dst_h;
  }
  if (jpeg_buf != nullptr) {
    tjFree(jpeg_buf);
  }
  tjDestroy(tj);
  return out;
}

std::optional<DecodedFrame> decodeThumbnailJpeg(const uint8_t* jpeg, size_t jpeg_size, int width, int height) {
  if (jpeg == nullptr || jpeg_size == 0 || width <= 0 || height <= 0) {
    return std::nullopt;
  }
  tjhandle tj = tjInitDecompress();
  if (tj == nullptr) {
    return std::nullopt;
  }
  auto pixels = std::make_shared<std::vector<uint8_t>>(expectedBufferSize(width, height, PixelFormat::kYUV420P));
  const int rc =
      tjDecompressToYUV2(tj, jpeg, static_cast<unsigned long>(jpeg_size), pixels->data(), width, 1, height, 0);
  tjDestroy(tj);
  if (rc != 0) {
    return std::nullopt;
  }
  DecodedFrame frame;
  frame.pixels = std::move(pixels);
  frame.width = width;
  frame.height = height;
  frame.format = PixelFormat::kYUV420P;
  frame.pts = -1;
  return frame;
}

}  // namespace PJ
