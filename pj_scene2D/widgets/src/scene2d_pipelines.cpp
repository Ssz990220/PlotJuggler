// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/scene2d_pipelines.h"

#include "pj_scene2d_core/codecs.h"

namespace PJ {

std::unique_ptr<CodecPipeline> makeScene2DPipelineFor(sdk::BuiltinObjectType object_type) {
  switch (object_type) {
    case sdk::BuiltinObjectType::kImage:
      return makeJpegPipeline();
    case sdk::BuiltinObjectType::kDepthImage:
      return nullptr;
    case sdk::BuiltinObjectType::kNone:
    case sdk::BuiltinObjectType::kPointCloud:
    case sdk::BuiltinObjectType::kImageAnnotations:
    case sdk::BuiltinObjectType::kFrameTransforms:
    case sdk::BuiltinObjectType::kOccupancyGrid:
    case sdk::BuiltinObjectType::kCompressedPointCloud:
    case sdk::BuiltinObjectType::kMesh3D:
    case sdk::BuiltinObjectType::kVideoFrame:
    case sdk::BuiltinObjectType::kSceneEntities:
    // kAssetVideo is a reserved SDK enum slot with no host decode path; listed
    // to keep this exhaustive switch -Werror=switch clean.
    case sdk::BuiltinObjectType::kAssetVideo:
    case sdk::BuiltinObjectType::kRobotDescription:
    case sdk::BuiltinObjectType::kCameraInfo:
    case sdk::BuiltinObjectType::kOccupancyGridUpdate:
    case sdk::BuiltinObjectType::kLog:
      return nullptr;
  }
  return nullptr;
}

}  // namespace PJ
