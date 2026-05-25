// Verifies the parser-less pipeline dispatch in Media2DDockWidget.
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
#include "pj_scene2d_widgets/Media2DDockWidget.h"

namespace {

TEST(Media2DDockDispatch, ImageReturnsBuiltInJpegPipeline) {
  auto pipeline = PJ::Media2DDockWidget::makePipelineFor(PJ::sdk::BuiltinObjectType::kImage);
  EXPECT_NE(pipeline, nullptr);
}

TEST(Media2DDockDispatch, DepthImageHasNoBuiltInPipeline) {
  // Deliberate gap: depth topics must come from a parser producing canonical
  // sdk::DepthImage. Re-introducing a generic depth fallback here would put
  // source-format/codec repair logic back into the widget — which the
  // canonical-object refactor explicitly moved out.
  auto pipeline = PJ::Media2DDockWidget::makePipelineFor(PJ::sdk::BuiltinObjectType::kDepthImage);
  EXPECT_EQ(pipeline, nullptr);
}

TEST(Media2DDockDispatch, NonImageTypesHaveNoBuiltInPipeline) {
  // Every non-image canonical type returns nullptr so the caller can fall
  // through to the parser path (or refuse the drop with a loud warning).
  EXPECT_EQ(PJ::Media2DDockWidget::makePipelineFor(PJ::sdk::BuiltinObjectType::kPointCloud), nullptr);
  EXPECT_EQ(PJ::Media2DDockWidget::makePipelineFor(PJ::sdk::BuiltinObjectType::kImageAnnotations), nullptr);
  EXPECT_EQ(PJ::Media2DDockWidget::makePipelineFor(PJ::sdk::BuiltinObjectType::kFrameTransforms), nullptr);
  EXPECT_EQ(PJ::Media2DDockWidget::makePipelineFor(PJ::sdk::BuiltinObjectType::kNone), nullptr);
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
