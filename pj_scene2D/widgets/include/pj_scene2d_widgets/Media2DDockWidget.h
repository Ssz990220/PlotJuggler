#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QWidget>
#include <memory>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/IDataWidget.h"
#include "pj_runtime/IObjectViewer.h"

namespace PJ {

class CodecPipeline;
class FileVideoSource;
class ImagePipelineSource;
class MediaSource;
class MediaViewerWidget;
class SessionManager;

// Dock content for 2D media/object topics. The widget is driven by the
// application's playback tracker and reads image bytes from ObjectStore.
class Media2DDockWidget : public QWidget, public IDataWidget, public IObjectViewer {
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

  // IObjectViewer: drop layers whose object topic was evicted; return whether any
  // live layer remains (false => shell resets the dock to the placeholder).
  bool revalidateObjects() override;

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
  // One object topic = one layer, owning its polymorphic source
  // (ImagePipelineSource for image/depth/annotation, FileVideoSource for file
  // video). Only a single base layer is populated today; the vector keeps the
  // viewer N-layer-ready for future composites.
  struct Layer {
    ObjectTopicId topic_id;
    std::unique_ptr<MediaSource> source;
  };

  // Appends a layer and rebinds the viewer.
  void addLayer(ObjectTopicId topic_id, std::unique_ptr<MediaSource> source);
  // Points the viewer at the current layers.
  void rebindViewer();
  // The source currently driving the viewer (nullptr when no layers): today the
  // single source. TODO(multi-layer): return a CompositeMediaSource once ≥2 exist.
  [[nodiscard]] MediaSource* boundSource() const;

  MediaViewerWidget* bootstrap_ = nullptr;
  MediaViewerWidget* viewer_ = nullptr;
  std::vector<Layer> layers_;
  SessionManager* session_ = nullptr;
  // Live-streaming nudge: object topics advance on ingest, not the playback clock,
  // so we follow the ObjectStore tip via SessionManager::samplesIngested (see
  // setImageTopic). One connection per dock, re-armed on each (re)bind.
  QMetaObject::Connection live_samples_conn_;
};

}  // namespace PJ
