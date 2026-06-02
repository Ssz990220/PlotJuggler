// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include <QImage>
#include <cstdint>
#include <cstring>
#include <vector>

#include "RasterIpcProtocol.h"
#include "pj_widgets/RasterFrame.h"

namespace {

using namespace pj_raster;

TEST(RasterFrame, ReadsActiveBufferAsImage) {
  const uint32_t w = 2, h = 1, bpp = 4;
  std::vector<uint8_t> seg(shmTotalSize(w, h, bpp), 0);
  auto* hdr = reinterpret_cast<ShmHeader*>(seg.data());
  hdr->magic = kShmMagic;
  hdr->version = kShmVersion;
  hdr->width = w;
  hdr->height = h;
  hdr->bytes_per_pixel = bpp;
  hdr->buffer_count = kBufferCount;
  hdr->active_index = 1;
  hdr->frame_seq = 7;
  uint8_t* buf1 = seg.data() + bufferOffset(1, w, h, bpp);
  buf1[0] = 0x00;
  buf1[1] = 0x00;
  buf1[2] = 0xFF;
  buf1[3] = 0xFF;  // red (B,G,R,A)
  buf1[4] = 0x00;
  buf1[5] = 0xFF;
  buf1[6] = 0x00;
  buf1[7] = 0xFF;  // green

  const QImage img = PJ::imageForActiveFrame(seg.data(), static_cast<int>(seg.size()));
  ASSERT_FALSE(img.isNull());
  EXPECT_EQ(img.width(), 2);
  EXPECT_EQ(img.height(), 1);
  EXPECT_EQ(img.pixel(0, 0), qRgb(0xFF, 0x00, 0x00));
  EXPECT_EQ(img.pixel(1, 0), qRgb(0x00, 0xFF, 0x00));
}

TEST(RasterFrame, RejectsBadMagicOrSize) {
  std::vector<uint8_t> tiny(4, 0);
  EXPECT_TRUE(PJ::imageForActiveFrame(tiny.data(), static_cast<int>(tiny.size())).isNull());
}

}  // namespace
