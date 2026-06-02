#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QWidget>
#include <memory>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/IDataWidget.h"

namespace PJ {

class CodecPipeline;
class FileVideoSource;
class ImagePipelineSource;
class MediaSource;
class MediaViewerWidget;
class SessionManager;

// Dock content for 2D media/object topics. The widget is driven by the
// application's playback tracker and reads image bytes from ObjectStore.
class Media2DDockWidget : public QWidget, public IDataWidget {
  Q_OBJECT
 public:
  explicit Media2DDockWidget(QWidget* parent = nullptr);
  ~Media2DDockWidget() override;

  void setSessionManager(SessionManager* session);
  bool setImageTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);
  void setPointInspectorEnabled(bool enabled);
  [[nodiscard]] bool pointInspectorEnabled() const noexcept;

  QWidget* widget() override {
    return this;
  }
  void onTrackerTime(double time) override;

  // Dispatch table for built-in (parser-less) decode pipelines, keyed by
  // canonical object type. Public static so unit tests can verify the dispatch
  // independently of constructing the widget (which would require an RHI).
  // Returns nullptr for object types that must come from a parser
  // (e.g. kDepthImage — see C5 in dfaconti/image-runtime review).
  [[nodiscard]] static std::unique_ptr<CodecPipeline> makePipelineFor(sdk::BuiltinObjectType object_type);

 private slots:
  // Invoked on the GUI thread (via QueuedConnection) whenever the source's
  // worker thread reports a fresh decoded frame is available.
  void pollPendingFrame();

 private:
  MediaViewerWidget* bootstrap_ = nullptr;
  MediaViewerWidget* viewer_ = nullptr;
  // Polymorphic source: ImagePipelineSource for image / depth / annotation
  // topics, FileVideoSource for file-backed video (sdk::AssetVideo).
  std::unique_ptr<MediaSource> media_topic_source_;
  SessionManager* session_ = nullptr;
  ObjectTopicId topic_id_{};
  QMetaObject::Connection live_samples_conn_;
};

}  // namespace PJ
