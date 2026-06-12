// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QColor>
#include <QString>
#include <QStringList>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pj_datastore/object_store.hpp"
#include "pj_scene3d_core/robot_model.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

namespace PJ {
class MessageParserPluginBase;
}  // namespace PJ

class QWidget;

namespace pj::scene3d {

class MeshLoader;
class MeshRenderPass;
class UrdfPackageResolver;

class RobotModelLayer : public Scene3DLayer {
  Q_OBJECT
 public:
  enum class SourceType { kTopic, kFile, kUrl };
  enum class DisplayMode { kAuto, kVisual, kCollision };

  RobotModelLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~RobotModelLayer() override;

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
  [[nodiscard]] std::optional<AABB> worldBounds() const override;

  QWidget* createConfigWidget(QWidget* parent) override;

  void setPackageResolver(UrdfPackageResolver* resolver);
  void setSourceTopic(PJ::ObjectTopicId topic_id, QString display_name = {});
  void setSourceFile(QString path);
  void setSourceUrl(QString url);
  void setFramePrefix(QString prefix);
  void setDisplayMode(DisplayMode mode);
  void setFallbackColor(QColor color);
  void setIgnoreColladaUpAxis(bool ignore);

  [[nodiscard]] SourceType sourceType() const {
    return source_type_;
  }
  [[nodiscard]] QString sourceValue() const {
    return source_value_;
  }
  [[nodiscard]] QString framePrefix() const {
    return frame_prefix_;
  }
  [[nodiscard]] DisplayMode displayMode() const {
    return display_mode_;
  }
  [[nodiscard]] QColor fallbackColor() const {
    return fallback_color_;
  }
  [[nodiscard]] bool ignoreColladaUpAxis() const {
    return ignore_collada_up_axis_;
  }
  [[nodiscard]] QString statusText() const {
    return status_text_;
  }
  [[nodiscard]] const RobotModel* robotModel() const {
    return model_.has_value() ? &*model_ : nullptr;
  }
  [[nodiscard]] int totalMeshCount() const {
    return total_mesh_count_;
  }
  [[nodiscard]] int unresolvedMeshCount() const {
    return unresolved_mesh_count_;
  }
  [[nodiscard]] QString linkFrameName(const std::string& link_name) const;

 signals:
  void meshLoadStatusChanged(int loaded, int total, QStringList unresolved_packages);
  void unresolvedPackages(QStringList packages);
  void statusTextChanged(const QString& status);

 private:
  struct MeshLoadRecord;

  bool loadFromCurrentSource();
  bool tryLoadTopicDescription();
  bool applyRobotDescription(
      const QString& text, const QString& format, const QString& label, const QString& urdf_dir, bool source_is_url);
  void startMeshLoads();
  void pollMeshLoads();
  [[nodiscard]] MeshLoadRecord* meshLoadForKey(const std::string& key) const;
  [[nodiscard]] bool meshReady(const std::string& key) const;
  void updateMeshCounters();
  void setStatus(QString status);
  [[nodiscard]] QStringList unresolvedPackagesList() const;

  PJ::ObjectTopicId topic_id_;
  PJ::ObjectTopicId source_topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  PJ::MessageParserPluginBase* parser_ = nullptr;
  std::shared_ptr<std::mutex> parser_mutex_;

  SourceType source_type_{SourceType::kTopic};
  QString source_value_;
  QString frame_prefix_;
  DisplayMode display_mode_{DisplayMode::kAuto};
  QColor fallback_color_{178, 178, 178};
  bool ignore_collada_up_axis_{false};
  bool visible_{true};
  QString fixed_frame_;
  QString status_text_;
  bool latch_pending_{false};
  std::chrono::steady_clock::time_point last_latch_retry_{};
  PJ::Timepoint tracker_time_{};

  std::optional<RobotModel> model_;
  int total_mesh_count_{0};
  int unresolved_mesh_count_{0};
  int loaded_mesh_count_{0};

  std::unique_ptr<UrdfPackageResolver> owned_resolver_;
  UrdfPackageResolver* resolver_{nullptr};
  std::unique_ptr<MeshLoader> mesh_loader_;
  std::unique_ptr<MeshRenderPass> mesh_pass_;
  std::vector<std::unique_ptr<MeshLoadRecord>> mesh_loads_;
};

}  // namespace pj::scene3d
