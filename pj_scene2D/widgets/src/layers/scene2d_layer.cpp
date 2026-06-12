// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/layers/scene2d_layer.h"

#include <QMetaObject>
#include <QPointer>
#include <QWidget>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"  // PJ::fromRaw, PJ::toRaw
#include "pj_scene2d_core/media_source.h"

namespace PJ {

Scene2DLayer::Scene2DLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, QString display_name, QString family_name,
    QObject* parent)
    : ISceneLayer(parent) {
  info_.topic_id = topic_id;
  info_.object_type = object_type;
  info_.display_name = std::move(display_name);
  info_.family_name = std::move(family_name);
  info_.visible = true;
}

Scene2DLayer::~Scene2DLayer() = default;

SceneLayerInfo Scene2DLayer::info() const {
  return info_;
}

PJ::Range<PJ::Timepoint> Scene2DLayer::timeRange() const {
  return PJ::liveTopicTimeRange(store_, info_.topic_id);
}

bool Scene2DLayer::attach(const SceneLayerContext& ctx) {
  session_ = ctx.session;
  if (session_ == nullptr) {
    emit warningChanged(true, tr("No active session"));
    return false;
  }
  store_ = &session_->objectStore();
  source_ = createMediaSource(ctx);
  if (source_ == nullptr) {
    store_ = nullptr;
    session_ = nullptr;
    emit warningChanged(true, tr("Layer source could not be created"));
    return false;
  }
  emit warningChanged(false, {});
  onAfterAttach();
  return true;
}

void Scene2DLayer::detach() {
  onBeforeDetach();
  source_.reset();
  store_ = nullptr;
  session_ = nullptr;
  last_tracker_time_ns_.reset();
}

void Scene2DLayer::setTrackerTime(PJ::Timepoint time) {
  last_tracker_time_ns_ = PJ::toRaw(time);
  if (source_ != nullptr) {
    source_->setTimestamp(PJ::toRaw(time));
  }
}

void Scene2DLayer::setVisible(bool visible) {
  if (info_.visible == visible) {
    return;
  }
  info_.visible = visible;
  emit visibilityChanged(visible);
}

QWidget* Scene2DLayer::createConfigWidget(QWidget* parent) {
  return new QWidget(parent);
}

QDomElement Scene2DLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement element = doc.createElement(QStringLiteral("scene2d_layer"));
  saveOptions(element);
  return element;
}

bool Scene2DLayer::xmlLoadState(const QDomElement& element) {
  if (element.isNull()) {
    return true;
  }
  return loadOptions(element);
}

std::optional<int64_t> Scene2DLayer::lastTrackerTimeNs() const noexcept {
  return last_tracker_time_ns_;
}

ObjectStore* Scene2DLayer::objectStore() const noexcept {
  return store_;
}

SessionManager* Scene2DLayer::sessionManager() const noexcept {
  return session_;
}

ObjectTopicId Scene2DLayer::topicId() const noexcept {
  return info_.topic_id;
}

sdk::BuiltinObjectType Scene2DLayer::objectType() const noexcept {
  return info_.object_type;
}

QString Scene2DLayer::displayName() const {
  return info_.display_name;
}

void Scene2DLayer::setSource(std::unique_ptr<MediaSource> source) {
  source_ = std::move(source);
}

MediaSource* Scene2DLayer::borrowedSource() const noexcept {
  return source_.get();
}

std::function<void()> Scene2DLayer::makeQueuedRepaintCallback() {
  return [qp = QPointer<Scene2DLayer>(this)]() {
    if (!qp) {
      return;
    }
    QMetaObject::invokeMethod(
        qp.data(),
        [qp]() {
          if (qp) {
            emit qp->repaintRequested();
          }
        },
        Qt::QueuedConnection);
  };
}

void Scene2DLayer::onAfterAttach() {}

void Scene2DLayer::onBeforeDetach() {}

void Scene2DLayer::saveOptions(QDomElement& /*element*/) const {}

bool Scene2DLayer::loadOptions(const QDomElement& /*element*/) {
  return true;
}

}  // namespace PJ
