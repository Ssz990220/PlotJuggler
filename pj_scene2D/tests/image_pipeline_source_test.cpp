#include "pj_scene2d_core/image_pipeline_source.h"

#include <gtest/gtest.h>
#include <png.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "pj_base/builtin/image.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"

namespace {

struct PngWriteCtx {
  std::vector<uint8_t> bytes;
};

void pngWriteCallback(png_structp png, png_bytep data, png_size_t length) {
  auto* ctx = static_cast<PngWriteCtx*>(png_get_io_ptr(png));
  const auto* first = reinterpret_cast<const uint8_t*>(data);
  ctx->bytes.insert(ctx->bytes.end(), first, first + length);
}

void pngFlushCallback(png_structp /*png*/) {}

std::vector<uint8_t> makeMono16Png(int width, int height, const std::vector<uint16_t>& values) {
  PngWriteCtx ctx;
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  EXPECT_NE(png, nullptr);
  png_infop info = png_create_info_struct(png);
  EXPECT_NE(info, nullptr);

  if (setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    return {};
  }

  png_set_write_fn(png, &ctx, pngWriteCallback, pngFlushCallback);
  png_set_IHDR(
      png, info, static_cast<png_uint_32>(width), static_cast<png_uint_32>(height), 16, PNG_COLOR_TYPE_GRAY,
      PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

  png_write_info(png, info);
  std::vector<uint8_t> big_endian_samples(values.size() * 2);
  for (size_t i = 0; i < values.size(); ++i) {
    big_endian_samples[i * 2 + 0] = static_cast<uint8_t>((values[i] >> 8) & 0xFF);
    big_endian_samples[i * 2 + 1] = static_cast<uint8_t>(values[i] & 0xFF);
  }
  std::vector<png_bytep> rows(static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    rows[static_cast<size_t>(y)] = big_endian_samples.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 2;
  }
  png_write_image(png, rows.data());
  png_write_end(png, info);
  png_destroy_write_struct(&png, &info);
  return ctx.bytes;
}

class CountingStage final : public PJ::CodecStage {
 public:
  explicit CountingStage(int& calls) : calls_(calls) {}

  PJ::Expected<PJ::DecodedFrame> decode(const PJ::DecodedFrame& input) const override {
    ++calls_;
    PJ::DecodedFrame frame;
    frame.width = 1;
    frame.height = 1;
    frame.format = PJ::PixelFormat::kRGBA8888;
    frame.pixels = std::make_shared<std::vector<uint8_t>>(4, input.pixels->empty() ? 0 : input.pixels->front());
    return frame;
  }

 private:
  int& calls_;
};

std::unique_ptr<PJ::CodecPipeline> makeCountingPipeline(int& calls) {
  auto pipeline = std::make_unique<PJ::CodecPipeline>();
  pipeline->addStage(std::make_unique<CountingStage>(calls));
  return pipeline;
}

class CanonicalRgbParser final : public PJ::MessageParserPluginBase {
 public:
  CanonicalRgbParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kImage;
    handler.parse_object = [](PJ::Timestamp ts, PJ::sdk::PayloadView payload) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::Image{
              .width = 1,
              .height = 1,
              .encoding = "rgb8",
              .row_step = 3,
              .is_bigendian = false,
              .data = payload.bytes,
              .anchor = payload.anchor,
              .compressed_depth_min = std::nullopt,
              .compressed_depth_max = std::nullopt,
              .timestamp_ns = ts,
          }}};
    };
    registerSchemaHandler("image", std::move(handler));
  }
};

class CanonicalCompressedDepthParser final : public PJ::MessageParserPluginBase {
 public:
  CanonicalCompressedDepthParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kImage;
    handler.parse_object = [](PJ::Timestamp ts, PJ::sdk::PayloadView payload) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::Image{
              .width = 0,
              .height = 0,
              .encoding = "compressedDepth",
              .row_step = 0,
              .is_bigendian = false,
              .data = payload.bytes,
              .anchor = payload.anchor,
              .compressed_depth_min = 0.0f,
              .compressed_depth_max = 1.0f,
              .timestamp_ns = ts,
          }}};
    };
    registerSchemaHandler("depth", std::move(handler));
  }
};

// Mirrors the real-world failure mode where a MessageParser keeps internal
// scratch (fastcdr offset, dictionaries) and two ImagePipelineSource workers
// sharing the same parser pointer enter parseObject concurrently. The atomic
// `in_flight_` counter catches concurrent entry deterministically; a
// non-atomic scratch field gives TSan a second signal when the suite is
// rebuilt with PJ_ENABLE_TSAN=ON.
class RacyImageParser final : public PJ::MessageParserPluginBase {
 public:
  RacyImageParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kImage;
    handler.parse_object = [this](
                               PJ::Timestamp ts, PJ::sdk::PayloadView payload) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      const int prev = in_flight_.fetch_add(1, std::memory_order_acq_rel);
      if (prev > 0) {
        race_observed_.store(true, std::memory_order_release);
      }
      // Force a deterministic window: without the sleep, the two workers may
      // serialise by chance on a fast machine and hide the race.
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      scratch_ = static_cast<int>(ts & 0xFF);  // intentionally non-atomic
      PJ::sdk::ObjectRecord out{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::Image{
              .width = 1,
              .height = 1,
              .encoding = "rgb8",
              .row_step = 3,
              .is_bigendian = false,
              .data = payload.bytes,
              .anchor = payload.anchor,
              .compressed_depth_min = std::nullopt,
              .compressed_depth_max = std::nullopt,
              .timestamp_ns = ts,
          }},
      };
      in_flight_.fetch_sub(1, std::memory_order_acq_rel);
      return out;
    };
    registerSchemaHandler("image", std::move(handler));
  }

  bool raceObserved() const {
    return race_observed_.load(std::memory_order_acquire);
  }

 private:
  std::atomic<int> in_flight_{0};
  std::atomic<bool> race_observed_{false};
  int scratch_ = 0;
};

// Bridges the source's frame-ready callback (fired from the worker thread)
// into a condition variable test code can wait on. Each waitReady() consumes
// at most one ready notification; back-to-back fires while no waiter is
// listening collapse to a single pending ready (matches the single-slot
// result_frame_ semantics of the source).
struct FrameSync {
  std::mutex mutex;
  std::condition_variable cv;
  bool ready = false;

  void install(PJ::ImagePipelineSource& source) {
    source.setFrameReadyCallback([this]() {
      std::lock_guard lock(mutex);
      ready = true;
      cv.notify_all();
    });
  }

  bool waitReady(std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    std::unique_lock lock(mutex);
    if (cv.wait_for(lock, timeout, [this] { return ready; })) {
      ready = false;
      return true;
    }
    return false;
  }
};

// Brief idle wait. Used to confirm a request that takes the dedup-skip path
// has been consumed by the worker (which never fires the frame-ready callback
// because no frame was produced). 100 ms is generous for an in-memory store.
constexpr std::chrono::milliseconds kIdleSettleTime{100};

}  // namespace

TEST(ImagePipelineSourceTest, DeduplicatesResolvedEntryTimestampBeforeResolvingLazyPayload) {
  PJ::ObjectStore store;
  auto topic = store.registerTopic({PJ::DatasetId{1}, "/camera/image", "{}"});
  ASSERT_TRUE(topic.has_value());

  int fetch_calls = 0;
  ASSERT_TRUE(store.pushLazy(*topic, 1'000, [&fetch_calls]() -> PJ::sdk::PayloadView {
    ++fetch_calls;
    return PJ::sdk::makePayloadView({1});
  }));
  ASSERT_TRUE(store.pushLazy(*topic, 2'000, [&fetch_calls]() -> PJ::sdk::PayloadView {
    ++fetch_calls;
    return PJ::sdk::makePayloadView({2});
  }));

  int decode_calls = 0;
  PJ::ImagePipelineSource source(&store, *topic, makeCountingPipeline(decode_calls));
  FrameSync sync;
  sync.install(source);

  source.setTimestamp(1'000);
  ASSERT_TRUE(sync.waitReady());
  EXPECT_EQ(fetch_calls, 1);
  EXPECT_EQ(decode_calls, 1);
  ASSERT_TRUE(source.takeFrame().has_value());

  // Both timestamps resolve to entry 0 (timestamp 1'000), which the worker
  // already decoded — dedup must short-circuit BEFORE invoking the lazy
  // fetcher or the pipeline stage. No callback fires for a dedup-skip.
  source.setTimestamp(1'200);
  source.setTimestamp(1'900);
  std::this_thread::sleep_for(kIdleSettleTime);
  EXPECT_EQ(fetch_calls, 1);
  EXPECT_EQ(decode_calls, 1);
  EXPECT_FALSE(source.takeFrame().has_value());

  source.setTimestamp(2'000);
  ASSERT_TRUE(sync.waitReady());
  EXPECT_EQ(fetch_calls, 2);
  EXPECT_EQ(decode_calls, 2);
  ASSERT_TRUE(source.takeFrame().has_value());
}

TEST(ImagePipelineSourceTest, ParserDrivenPathConsumesCanonicalImage) {
  PJ::ObjectStore store;
  auto topic = store.registerTopic({PJ::DatasetId{1}, "/camera/image", "{}"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(store.pushOwned(*topic, 1'000, std::vector<uint8_t>{10, 20, 30}));

  CanonicalRgbParser parser;
  ASSERT_TRUE(parser.bindSchema("image", PJ::Span<const uint8_t>{}));
  PJ::ImagePipelineSource source(&store, *topic, &parser);
  FrameSync sync;
  sync.install(source);

  source.setTimestamp(1'000);
  ASSERT_TRUE(sync.waitReady());
  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());

  EXPECT_EQ(frame->base->width, 1);
  EXPECT_EQ(frame->base->height, 1);
  EXPECT_EQ(frame->base->format, PJ::PixelFormat::kRGB888);
  ASSERT_NE(frame->base->pixels, nullptr);
  ASSERT_EQ(*frame->base->pixels, (std::vector<uint8_t>{10, 20, 30}));
}

TEST(ImagePipelineSourceTest, ParserDrivenCompressedDepthDecodesPngThenNormalizesMono16) {
  const std::vector<uint8_t> png = makeMono16Png(2, 2, {0, 1000, 2000, 3000});
  ASSERT_FALSE(png.empty());

  PJ::ObjectStore store;
  auto topic = store.registerTopic({PJ::DatasetId{1}, "/camera/depth", "{}"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(store.pushOwned(*topic, 1'000, png));

  CanonicalCompressedDepthParser parser;
  ASSERT_TRUE(parser.bindSchema("depth", PJ::Span<const uint8_t>{}));
  PJ::ImagePipelineSource source(&store, *topic, &parser);
  FrameSync sync;
  sync.install(source);

  source.setTimestamp(1'000);
  ASSERT_TRUE(sync.waitReady());
  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());

  EXPECT_EQ(frame->base->width, 2);
  EXPECT_EQ(frame->base->height, 2);
  EXPECT_EQ(frame->base->format, PJ::PixelFormat::kRGB888);
  ASSERT_NE(frame->base->pixels, nullptr);
  ASSERT_EQ(frame->base->pixels->size(), 12U);
  EXPECT_EQ((*frame->base->pixels)[0], 0);
  EXPECT_EQ((*frame->base->pixels)[3], 0);
  EXPECT_EQ((*frame->base->pixels)[6], 127);
  EXPECT_EQ((*frame->base->pixels)[9], 255);
}

TEST(ImagePipelineSourceTest, ParserDrivenCompressedDepthRepairsChunkStreamStartingAtIhdr) {
  const std::vector<uint8_t> png = makeMono16Png(2, 2, {0, 1000, 2000, 3000});
  ASSERT_GT(png.size(), 12U);
  ASSERT_EQ(png[12], 'I');
  ASSERT_EQ(png[13], 'H');
  ASSERT_EQ(png[14], 'D');
  ASSERT_EQ(png[15], 'R');

  std::vector<uint8_t> chunk_stream(png.begin() + 12, png.end());

  PJ::ObjectStore store;
  auto topic = store.registerTopic({PJ::DatasetId{1}, "/camera/depth", "{}"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(store.pushOwned(*topic, 1'000, chunk_stream));

  CanonicalCompressedDepthParser parser;
  ASSERT_TRUE(parser.bindSchema("depth", PJ::Span<const uint8_t>{}));
  PJ::ImagePipelineSource source(&store, *topic, &parser);
  FrameSync sync;
  sync.install(source);

  source.setTimestamp(1'000);
  ASSERT_TRUE(sync.waitReady());
  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());

  EXPECT_EQ(frame->base->width, 2);
  EXPECT_EQ(frame->base->height, 2);
  EXPECT_EQ(frame->base->format, PJ::PixelFormat::kRGB888);
  ASSERT_NE(frame->base->pixels, nullptr);
  ASSERT_EQ(frame->base->pixels->size(), 12U);
  EXPECT_EQ((*frame->base->pixels)[0], 0);
  EXPECT_EQ((*frame->base->pixels)[3], 0);
  EXPECT_EQ((*frame->base->pixels)[6], 127);
  EXPECT_EQ((*frame->base->pixels)[9], 255);
}

TEST(ImagePipelineSourceTest, CoalescesBurstOfSetTimestampToSingleDecode) {
  // Three distinct entries; burst-fire 100 setTimestamp calls that resolve
  // across all three. Worker must skip intermediate values via the in-decode
  // re-check and only decode the LATEST target (plus possibly one earlier
  // target if it captured a request before the burst finished).
  PJ::ObjectStore store;
  auto topic = store.registerTopic({PJ::DatasetId{1}, "/camera/image", "{}"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(store.pushOwned(*topic, 1'000, std::vector<uint8_t>{1, 2, 3}));
  ASSERT_TRUE(store.pushOwned(*topic, 2'000, std::vector<uint8_t>{4, 5, 6}));
  ASSERT_TRUE(store.pushOwned(*topic, 3'000, std::vector<uint8_t>{7, 8, 9}));

  CanonicalRgbParser parser;
  ASSERT_TRUE(parser.bindSchema("image", PJ::Span<const uint8_t>{}));
  PJ::ImagePipelineSource source(&store, *topic, &parser);
  FrameSync sync;
  sync.install(source);

  for (int i = 0; i < 100; ++i) {
    source.setTimestamp(1'000 + (i % 3) * 1'000);
  }
  // Explicit final target so the worker has a deterministic "latest" — the
  // burst-loop modulo arithmetic ends on i=99 → t=1'000, which would race
  // with whichever decode the worker happens to capture.
  source.setTimestamp(3'000);

  // Worker should converge on entry 2 (timestamp 3'000) and stop. Wait long
  // enough for any pending decodes to drain.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());
  // Latest target was 3'000 → entry 2 → pixel value 7.
  ASSERT_NE(frame->base->pixels, nullptr);
  EXPECT_EQ((*frame->base->pixels)[0], 7U);
}

TEST(ImagePipelineSourceTest, DestructorJoinsWorkerEvenWithPendingRequest) {
  // Stress destruction race: construct + destroy in a tight loop with a fresh
  // setTimestamp posted each iteration. Each destructor must cleanly stop the
  // worker (no use-after-free, no hang). Run under ASAN/TSAN this is the
  // signal that destruction ordering is correct.
  for (int i = 0; i < 50; ++i) {
    PJ::ObjectStore store;
    auto topic = store.registerTopic({PJ::DatasetId{1}, "/camera/image", "{}"});
    ASSERT_TRUE(topic.has_value());
    ASSERT_TRUE(store.pushOwned(*topic, 1'000, std::vector<uint8_t>{10, 20, 30}));

    CanonicalRgbParser parser;
    ASSERT_TRUE(parser.bindSchema("image", PJ::Span<const uint8_t>{}));

    auto source = std::make_unique<PJ::ImagePipelineSource>(&store, *topic, &parser);
    source->setTimestamp(1'000);
    // Destroy immediately — may be mid-decode. Destructor must join cleanly.
    source.reset();
  }
}

TEST(ImagePipelineSourceTest, SharedParserMutexSerializesConcurrentSources) {
  // Two ImagePipelineSource instances sharing the same parser pointer (the
  // SessionManager singleton, in production) must never enter parseObject
  // concurrently — fastcdr et al. keep stateful scratch and corrupt under
  // a race, manifesting as bogus payload sizes and segfaults. Passing a
  // shared mutex collapses concurrent decodes onto a single critical section.
  PJ::ObjectStore store;
  auto topic = store.registerTopic({PJ::DatasetId{1}, "/camera/image", "{}"});
  ASSERT_TRUE(topic.has_value());
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(store.pushOwned(*topic, 1'000 + i * 100, std::vector<uint8_t>{static_cast<uint8_t>(i), 20, 30}));
  }

  RacyImageParser parser;
  ASSERT_TRUE(parser.bindSchema("image", PJ::Span<const uint8_t>{}));

  auto parser_mutex = std::make_shared<std::mutex>();
  PJ::ImagePipelineSource source_a(&store, *topic, &parser, parser_mutex);
  PJ::ImagePipelineSource source_b(&store, *topic, &parser, parser_mutex);
  FrameSync sync_a;
  FrameSync sync_b;
  sync_a.install(source_a);
  sync_b.install(source_b);

  // Burst-fire both sources at distinct timestamps so they cannot dedup against
  // their own previous request. Worker threads will pile onto parseObject and,
  // without the shared mutex, race.
  for (int i = 0; i < 8; ++i) {
    source_a.setTimestamp(1'000 + i * 100);
    source_b.setTimestamp(1'000 + i * 100);
  }
  // Wait long enough for both workers to drain — 5 ms per parse × 8 frames
  // × 2 workers, serialized = ~80 ms; double it for safety.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_FALSE(parser.raceObserved()) << "parseObject was entered concurrently — shared parser_mutex did not "
                                         "serialise consumers (this is the bug the fix addresses).";
}
