// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Verifies the parser-less pipeline dispatch used by the 2D scene layers.
//
// Background: C5 in the dfaconti/image-runtime review flagged that
// kDepthImage with no parser silently failed. We deliberately do NOT restore
// a depth fallback here — production depth topics must come from a parser
// that produces canonical sdk::DepthImage. This test pins the dispatch table
// so the deliberate gap doesn't regress without notice and so the (currently
// supported) kImage path keeps returning a usable pipeline.

#include <gtest/gtest.h>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_scene2d_core/codec_pipeline.h"
#include "pj_scene2d_widgets/Scene2DDockWidget.h"
#include "pj_scene2d_widgets/scene2d_pipelines.h"

namespace {

TEST(Scene2DDockDispatch, ImageReturnsBuiltInJpegPipeline) {
  auto pipeline = PJ::makeScene2DPipelineFor(PJ::sdk::BuiltinObjectType::kImage);
  EXPECT_NE(pipeline, nullptr);
}

TEST(Scene2DDockDispatch, DepthImageHasNoBuiltInPipeline) {
  // Deliberate gap: depth topics must come from a parser producing canonical
  // sdk::DepthImage. Re-introducing a generic depth fallback here would put
  // source-format/codec repair logic back into the widget — which the
  // canonical-object refactor explicitly moved out.
  auto pipeline = PJ::makeScene2DPipelineFor(PJ::sdk::BuiltinObjectType::kDepthImage);
  EXPECT_EQ(pipeline, nullptr);
}

TEST(Scene2DDockDispatch, NonImageTypesHaveNoBuiltInPipeline) {
  // Every non-image canonical type returns nullptr so the caller can fall
  // through to the parser path (or refuse the drop with a loud warning).
  EXPECT_EQ(PJ::makeScene2DPipelineFor(PJ::sdk::BuiltinObjectType::kPointCloud), nullptr);
  EXPECT_EQ(PJ::makeScene2DPipelineFor(PJ::sdk::BuiltinObjectType::kImageAnnotations), nullptr);
  EXPECT_EQ(PJ::makeScene2DPipelineFor(PJ::sdk::BuiltinObjectType::kFrameTransforms), nullptr);
  EXPECT_EQ(PJ::makeScene2DPipelineFor(PJ::sdk::BuiltinObjectType::kNone), nullptr);
}

TEST(Scene2DDockDispatch, VideoFrameTopicIsHandled) {
  // A per-frame canonical video topic (PJ.VideoFrame / Foxglove CompressedVideo)
  // must be accepted by the 2D dock so it can be dropped and rendered through a
  // VideoLayer whenever the FFmpeg-backed video path is built.
#ifdef PJ_HAS_FFMPEG
  EXPECT_TRUE(PJ::Scene2DDockWidget::handlesObjectType(PJ::sdk::BuiltinObjectType::kVideoFrame));
#else
  EXPECT_FALSE(PJ::Scene2DDockWidget::handlesObjectType(PJ::sdk::BuiltinObjectType::kVideoFrame));
#endif
}

TEST(Scene2DDockDispatch, VideoFrameHasNoBuiltInPipeline) {
  // Video is decoded by StreamingVideoSource (FFmpeg) on a worker thread, not a
  // CodecPipeline, so makeScene2DPipelineFor() stays nullptr for kVideoFrame — the
  // VideoLayer builds the source directly.
  EXPECT_EQ(PJ::makeScene2DPipelineFor(PJ::sdk::BuiltinObjectType::kVideoFrame), nullptr);
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
