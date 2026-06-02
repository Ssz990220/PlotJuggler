// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include <cstdint>

#include "RasterIpcProtocol.h"

namespace {

using namespace pj_raster;

TEST(RasterIpcProtocol, MessageRoundTripsEveryType) {
  for (MsgType t : {MsgType::kNone, MsgType::kFrameReady, MsgType::kKeyDown, MsgType::kKeyUp, MsgType::kBye}) {
    const Message in{t, 0xDEADBEEFu};
    uint8_t bytes[kMessageBytes];
    encodeMessage(in, bytes);
    const Message out = decodeMessage(bytes);
    EXPECT_EQ(static_cast<int>(out.type), static_cast<int>(t));
    EXPECT_EQ(out.arg, 0xDEADBEEFu);
  }
}

TEST(RasterIpcProtocol, ShmSizeAndOffsetsAreConsistent) {
  const uint32_t w = 640, h = 400, bpp = 4;
  EXPECT_EQ(shmTotalSize(w, h, bpp), sizeof(ShmHeader) + kBufferCount * w * h * bpp);
  EXPECT_EQ(bufferOffset(0, w, h, bpp), sizeof(ShmHeader));
  EXPECT_EQ(bufferOffset(1, w, h, bpp), sizeof(ShmHeader) + (w * h * bpp));
}

}  // namespace
