#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QDomDocument>
#include <QDomElement>
#include <QObject>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"            // PJ::ObjectTopicId, PJ::SequentialUID
#include "pj_scene3d_core/poses_in_frame_render.h"  // PoseTriadInstance
#include "pj_scene3d_widgets/passes/poses_render_pass.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

class QWidget;

namespace pj::scene3d {

// Scene3DLayer for a PJ.PosesInFrame topic (geometry_msgs/PoseArray equivalent,
// kPosesInFrame). At each tracker tick it decodes the pose set valid at that time,
// expands every pose into a coordinate-axis triad (buildPoseTriadInstances), and
// hands the arms to a GPU-instanced PosesRenderPass — so an AMCL-scale pose cloud
// stays one draw call. The single shared frame_id is resolved to the fixed frame
// at render time via the FrameContext.
//
// The per-layer params (arrow length, opacity, X-arrow-only, and a color
// override) are viewer style, never in the message; they persist via
// xmlSaveState/xmlLoadState (which also makes them copy/paste/apply-to-family-able
// through pj_scene_common's serializeLayerParams).
class PosesInFrameLayer : public Scene3DLayer {
  Q_OBJECT
 public:
  PosesInFrameLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~PosesInFrameLayer() override;

  // ISceneLayer / Scene3DLayer
  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  [[nodiscard]] QStringList fallbackFrames() const override;
  [[nodiscard]] QString sourceFrame() const override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  bool attach(const PJ::SceneLayerContext& ctx) override;
  void detach() override;

  void setFixedFrame(const QString& frame) override;
  void setTrackerTime(PJ::Timepoint time) override;
  void setVisible(bool visible) override;

  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;

  QWidget* createConfigWidget(QWidget* parent) override;

  // Per-layer display params, driven by the config widget + XML. Size is the
  // triad arm length in meters; opacity (0..1) is the gizmo alpha.
  void setGizmoSize(float meters);
  void setGizmoOpacity(float opacity);
  [[nodiscard]] float gizmoSize() const {
    return gizmo_size_;
  }
  [[nodiscard]] float gizmoOpacity() const {
    return gizmo_opacity_;
  }

  // X-arrow-only mode: draw a single X-axis arrow per pose instead of the full
  // XYZ triad. Geometry only — orthogonal to the color override below.
  void setXArrowOnly(bool x_arrow_only);
  [[nodiscard]] bool xArrowOnly() const {
    return x_arrow_only_;
  }

  // Color override: when enabled, every produced arm (the single X arm OR all
  // three triad arms) is recolored with one shared color instead of the natural
  // per-axis R/G/B. Works in both X-only and full-triad mode.
  void setOverrideColorEnabled(bool enabled);
  void setOverrideColor(QColor color);
  [[nodiscard]] bool overrideColorEnabled() const {
    return override_color_enabled_;
  }
  [[nodiscard]] QColor overrideColor() const {
    return override_color_;
  }

#ifdef PJ_SCENE3D_TEST_HOOKS
  // Decode + expand at a chosen time with no GL context (the dock normally drives
  // this from render() at the tracker time).
  void renderAtForTest(int64_t time_ns) {
    renderAt(time_ns);
  }
  // The arms staged by the last renderAt (frame-local; TF is applied at render).
  [[nodiscard]] const std::vector<PoseTriadInstance>& instancesForTest() const {
    return instances_;
  }
#endif

 private:
  // Decode the first sample at attach to learn the source frame before render.
  bool bootstrap();
  // Decode the sample valid at time_ns, expand it, and stage it into the pass.
  // No-ops when the same sample is already staged with the current style.
  void renderAt(int64_t time_ns);
  // Drop staged geometry + the replay cursors. Called on attach/detach: a prior
  // attachment's UID/frame may belong to a timeline that no longer exists
  // (dataset replace), so it must never anchor or skip the new generation.
  void resetReplayState();
  // Track the (possibly changing) source frame_id; notify the dock on change.
  void updateSourceFrame(const std::string& frame_id);

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  // Parsers are deliberately NOT cached: every decode resolves a fresh
  // ParserBinding through ctx_.session (parseLocked()), so a file reload that
  // re-registers the topic's parser slot can never leave a dangling pointer.

  std::string source_frame_;
  bool visible_ = true;
  // Set by setTrackerTime / a style edit; drained by render() so a fast scrub or
  // slider drag re-expands only the final state, once per painted frame.
  bool tracker_dirty_ = false;

  // Viewer style baked into the instance buffer; defaults match the TF "Frames"
  // gizmos. A change bumps style_revision_ so renderAt re-expands the current
  // sample instead of skipping it as unchanged.
  float gizmo_size_ = 0.15f;
  float gizmo_opacity_ = 1.0f;
  bool x_arrow_only_ = false;
  bool override_color_enabled_ = false;
  QColor override_color_{255, 0, 0};  // red, the panel-wide override/solid default
  int style_revision_ = 0;

  // Identity of the sample + style currently staged, so a scrub within one
  // message (or an unchanged style) skips the parse/expand/upload.
  PJ::SequentialUID staged_uid_{};
  int staged_revision_ = -1;

  std::vector<PoseTriadInstance> instances_;  // last expansion; also the test view
  PosesRenderPass pass_;
};

}  // namespace pj::scene3d
