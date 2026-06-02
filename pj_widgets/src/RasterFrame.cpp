// SPDX-License-Identifier: MPL-2.0
#include "pj_widgets/RasterFrame.h"

#include <cstdint>

#include "RasterIpcProtocol.h"

namespace PJ {

QImage imageForActiveFrame(const unsigned char* data, int size_bytes) {
  using namespace pj_raster;
  if (data == nullptr || size_bytes < static_cast<int>(sizeof(ShmHeader))) {
    return {};
  }
  const auto* hdr = reinterpret_cast<const ShmHeader*>(data);
  if (hdr->magic != kShmMagic || hdr->version != kShmVersion || hdr->bytes_per_pixel != kBytesPerPixel ||
      hdr->width == 0 || hdr->height == 0 || hdr->active_index >= hdr->buffer_count) {
    return {};
  }
  const uint32_t need = shmTotalSize(hdr->width, hdr->height, hdr->bytes_per_pixel);
  if (size_bytes < static_cast<int>(need)) {
    return {};
  }
  const uint32_t off = bufferOffset(hdr->active_index, hdr->width, hdr->height, hdr->bytes_per_pixel);
  const uchar* pixels = data + off;
  return QImage(
      pixels, static_cast<int>(hdr->width), static_cast<int>(hdr->height),
      static_cast<int>(hdr->width * hdr->bytes_per_pixel), QImage::Format_RGB32);
}

}  // namespace PJ
