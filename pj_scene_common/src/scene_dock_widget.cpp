// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene_common/scene_dock_widget.h"

#include <QBoxLayout>
#include <QSizePolicy>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pj_runtime/SessionManager.h"

namespace PJ {

namespace {

[[nodiscard]] int64_t topicKey(ObjectTopicId topic_id) {
  return static_cast<int64_t>(topic_id.id);
}

[[nodiscard]] QString objectTypeName(sdk::BuiltinObjectType object_type) {
  const auto name = sdk::name(object_type);
  return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
}

}  // namespace

SceneDockWidget::SceneDockWidget(QWidget* parent) : QWidget(parent) {
  setContentsMargins(0, 0, 0, 0);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  QTimer::singleShot(0, this, [this]() { ensureSceneViewCreated(); });
}

SceneDockWidget::~SceneDockWidget() {
  // clearLayers() must not run here: it reconciles the concrete view through
  // the pure-virtual syncViewLayers(), which cannot be dispatched during
  // base-class destruction. Concrete docks whose view references layers call
  // clearLayers() in their own destructor; by the time we get here that has
  // either happened (this is a no-op) or no view reconciliation was needed.
  destroyLayersUnsynced();
}

QWidget* SceneDockWidget::widget() {
  return this;
}

void SceneDockWidget::setSessionManager(SessionManager* session) {
  session_ = session;
}

bool SceneDockWidget::tryAcceptObjectTopic(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  // No acceptsObjectType() pre-gate: addLayer() consults handleSceneConfigTopic()
  // first (a family may consume a topic as scene-wide config, e.g. TF, without it
  // being a render layer) and rejects unsupported types via createAndAttachLayer().
  return addTopic(topic_id, object_type, title);
}

bool SceneDockWidget::addTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  // Public accept/refuse contract: both "layer added" and "consumed as config"
  // count as accepted. Callers needing the distinction use addLayer().
  return addLayer(topic_id, object_type, title) != AddOutcome::Rejected;
}

SceneDockWidget::AddOutcome SceneDockWidget::addLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  ensureSceneViewCreated();
  const int64_t key = topicKey(topic_id);
  if (layers_.find(key) != layers_.end()) {
    return AddOutcome::Rejected;
  }
  if (handleSceneConfigTopic(topic_id, object_type, title)) {
    return AddOutcome::ConsumedAsConfig;
  }

  std::unique_ptr<ISceneLayer> layer = createAndAttachLayer(topic_id, object_type, title);
  if (layer == nullptr) {
    return AddOutcome::Rejected;
  }
  wireLayerSignals(layer.get(), topic_id);
  registerLayer(key, std::move(layer));

  // Mutate -> reconcile view -> notify: the view already includes the new layer
  // when observers react to layerAdded (a slot may trigger a paint).
  syncViewLayers();
  refreshView();
  emit layerAdded(topic_id);
  return AddOutcome::LayerAdded;
}

std::unique_ptr<ISceneLayer> SceneDockWidget::createAndAttachLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  if (!acceptsObjectType(object_type)) {
    return nullptr;
  }
  std::unique_ptr<ISceneLayer> layer = factory_.create(topic_id, object_type, title);
  if (layer == nullptr) {
    return nullptr;
  }
  std::unique_ptr<SceneLayerContext> context = makeContext();
  SceneLayerContext fallback_context;
  fallback_context.session = session_;
  const SceneLayerContext& attach_context = context != nullptr ? *context : fallback_context;
  if (!layer->attach(attach_context)) {
    return nullptr;
  }
  return layer;
}

void SceneDockWidget::wireLayerSignals(ISceneLayer* layer, ObjectTopicId topic_id) {
  connect(layer, &ISceneLayer::infoChanged, this, [this]() {
    syncViewLayers();
    refreshView();
  });
  connect(layer, &ISceneLayer::visibilityChanged, this, [this, topic_id](bool visible) {
    recordLayerVisibility(topic_id, visible);
    syncViewLayers();
    refreshView();
  });
  connect(layer, &ISceneLayer::repaintRequested, this, [this]() { refreshView(); });
  connect(layer, &ISceneLayer::warningChanged, this, [this, topic_id](bool warn, QString reason) {
    emit layerWarningChanged(topic_id, warn, std::move(reason));
  });
}

void SceneDockWidget::registerLayer(int64_t key, std::unique_ptr<ISceneLayer> layer) {
  ISceneLayer* layer_raw = layer.get();
  layer_visibility_cache_[key] = layer_raw->info().visible;
  layers_.emplace(key, std::move(layer));
  draw_order_.push_back(key);

  if (layer_raw->info().visible) {
    const auto range = layer_raw->timeRange();
    const PJ::Timepoint first = range.min;
    const PJ::Timepoint last = range.max;
    if (last_tracker_.has_value()) {
      const PJ::Timepoint seed = (last >= first) ? std::clamp(*last_tracker_, first, last) : *last_tracker_;
      layer_raw->setTrackerTime(seed);
    } else if (last >= first) {
      // No tracker tick yet: deliberately show the layer's first frame instead
      // of fabricating a time (0 is a valid timestamp; absence is explicit).
      layer_raw->setTrackerTime(first);
    }
  }
}

void SceneDockWidget::removeTopic(ObjectTopicId topic_id) {
  const int64_t key = topicKey(topic_id);
  auto it = layers_.find(key);
  if (it == layers_.end()) {
    return;
  }
  if (it->second != nullptr) {
    it->second->detach();
  }
  // Mutate -> reconcile view -> notify. Keep the removed layer alive until after
  // syncViewLayers() repoints the view off this layer's backend and observers are
  // notified, so a paint triggered from a layerRemoved slot never renders a
  // composite borrowing a detached/destroyed source. Destroyed at scope exit.
  std::unique_ptr<ISceneLayer> removed = std::move(it->second);
  layers_.erase(it);
  layer_visibility_cache_.erase(key);
  draw_order_.erase(std::remove(draw_order_.begin(), draw_order_.end(), key), draw_order_.end());
  syncViewLayers();
  refreshView();
  emit layerRemoved(topic_id);
}

bool SceneDockWidget::revalidateObjects() {
  if (session_ == nullptr || layers_.empty()) {
    return !layers_.empty();
  }
  // A live topic carries a name; an evicted one resolves to the empty
  // descriptor. Collect first — removeTopic() mutates layers_ and runs the
  // full removal path (detach, view re-point, layerRemoved notification), so
  // observers like the layer-list panels stay coherent for free.
  ObjectStore& store = session_->objectStore();
  std::vector<ObjectTopicId> dead;
  for (const auto& [key, layer] : layers_) {
    if (layer == nullptr) {
      continue;
    }
    const ObjectTopicId topic_id = layer->info().topic_id;
    if (store.descriptor(topic_id).topic_name.empty()) {
      dead.push_back(topic_id);
    }
  }
  for (const ObjectTopicId topic_id : dead) {
    removeTopic(topic_id);
  }
  return !layers_.empty();
}

void SceneDockWidget::setLayerVisible(ObjectTopicId topic_id, bool visible) {
  ISceneLayer* layer = layerFor(topic_id);
  if (layer == nullptr) {
    return;
  }
  const int64_t key = topicKey(topic_id);
  const auto old_it = layer_visibility_cache_.find(key);
  const bool old_visible = old_it != layer_visibility_cache_.end() ? old_it->second : layer->info().visible;
  layer->setVisible(visible);
  const auto new_it = layer_visibility_cache_.find(key);
  if (new_it == layer_visibility_cache_.end() || new_it->second == old_visible) {
    recordLayerVisibility(topic_id, visible);
  }
  syncViewLayers();
  refreshView();
}

void SceneDockWidget::reorderLayers(const std::vector<ObjectTopicId>& ordered_topic_ids) {
  std::vector<int64_t> ordered;
  ordered.reserve(draw_order_.size());
  std::unordered_set<int64_t> seen;
  for (const ObjectTopicId topic_id : ordered_topic_ids) {
    const int64_t key = topicKey(topic_id);
    if (layers_.find(key) != layers_.end() && seen.insert(key).second) {
      ordered.push_back(key);
    }
  }
  for (const int64_t key : draw_order_) {
    if (seen.insert(key).second) {
      ordered.push_back(key);
    }
  }
  draw_order_ = std::move(ordered);
  syncViewLayers();
  refreshView();
}

std::vector<SceneLayerInfo> SceneDockWidget::layers() const {
  std::vector<SceneLayerInfo> out;
  out.reserve(draw_order_.size());
  for (const int64_t key : draw_order_) {
    const auto it = layers_.find(key);
    if (it != layers_.end() && it->second != nullptr) {
      out.push_back(it->second->info());
    }
  }
  return out;
}

ISceneLayer* SceneDockWidget::layerFor(ObjectTopicId topic_id) const {
  const auto it = layers_.find(topicKey(topic_id));
  return it == layers_.end() ? nullptr : it->second.get();
}

void SceneDockWidget::onTrackerTime(double time) {
  // IDataWidget delivers tracker time as seconds in a bare double; convert via
  // chrono instead of a hand-rolled 1e9 factor. NaN/inf carry no position and are
  // UB to cast, so drop them; saturate finite out-of-range values before casting.
  const double ns_d = std::chrono::duration<double, std::nano>(std::chrono::duration<double>(time)).count();
  if (!std::isfinite(ns_d)) {
    return;
  }
  int64_t raw_ns = 0;
  if (ns_d >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
    raw_ns = std::numeric_limits<int64_t>::max();
  } else if (ns_d <= static_cast<double>(std::numeric_limits<int64_t>::lowest())) {
    raw_ns = std::numeric_limits<int64_t>::lowest();
  } else {
    raw_ns = static_cast<int64_t>(ns_d);
  }
  const PJ::Timepoint clamped = clampToLayerRange(PJ::fromRaw(raw_ns));
  last_tracker_ = clamped;
  for (auto& [key, layer] : layers_) {
    if (layer != nullptr && layer->info().visible) {
      layer->setTrackerTime(clamped);
    }
  }
  refreshView();
}

QDomElement SceneDockWidget::xmlSaveState(QDomDocument& doc) const {
  QDomElement root = doc.createElement(xmlTag());
  root.setAttribute(QStringLiteral("version"), QStringLiteral("1"));

  if (session_ == nullptr) {
    return root;
  }

  for (const int64_t key : draw_order_) {
    const auto it = layers_.find(key);
    if (it == layers_.end() || it->second == nullptr) {
      continue;
    }
    const auto info = it->second->info();
    const auto& desc = session_->objectStore().descriptor(info.topic_id);

    QDomElement layer_el = doc.createElement(QStringLiteral("layer"));
    layer_el.setAttribute(QStringLiteral("dataset_id"), QString::number(desc.dataset_id));
    layer_el.setAttribute(QStringLiteral("topic_name"), QString::fromStdString(desc.topic_name));
    layer_el.setAttribute(QStringLiteral("object_type"), objectTypeName(info.object_type));
    layer_el.setAttribute(QStringLiteral("display_name"), info.display_name);
    layer_el.setAttribute(QStringLiteral("visible"), info.visible ? QStringLiteral("true") : QStringLiteral("false"));

    QDomElement payload = it->second->xmlSaveState(doc);
    if (!payload.isNull()) {
      layer_el.appendChild(payload);
    }
    root.appendChild(layer_el);
  }
  return root;
}

bool SceneDockWidget::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != xmlTag()) {
    return false;
  }
  clearLayers();
  if (session_ == nullptr) {
    syncViewLayers();
    refreshView();
    return true;
  }

  for (QDomElement layer_el = element.firstChildElement(QStringLiteral("layer")); !layer_el.isNull();
       layer_el = layer_el.nextSiblingElement(QStringLiteral("layer"))) {
    bool dataset_ok = false;
    const auto dataset_value = layer_el.attribute(QStringLiteral("dataset_id")).toULongLong(&dataset_ok);
    if (!dataset_ok || dataset_value > std::numeric_limits<uint32_t>::max()) {
      continue;
    }
    const auto dataset_id = static_cast<DatasetId>(dataset_value);
    const QString topic_name = layer_el.attribute(QStringLiteral("topic_name"));
    const QString object_type_str = layer_el.attribute(QStringLiteral("object_type"));
    const QString display_name = layer_el.attribute(QStringLiteral("display_name"));
    const bool visible =
        layer_el.attribute(QStringLiteral("visible"), QStringLiteral("true")) == QStringLiteral("true");

    const auto object_type_opt = sdk::parseBuiltinObjectType(object_type_str.toStdString());
    if (!object_type_opt.has_value()) {
      continue;
    }
    const auto topic_id_opt = session_->objectStore().findTopic(dataset_id, topic_name.toStdString());
    if (!topic_id_opt.has_value()) {
      continue;
    }
    if (addLayer(*topic_id_opt, *object_type_opt, display_name) != AddOutcome::LayerAdded) {
      continue;  // rejected, or consumed as a scene-config topic: no layer to restore
    }
    if (ISceneLayer* layer = layerFor(*topic_id_opt); layer != nullptr) {
      const QDomElement payload = layer_el.firstChildElement();
      if (!payload.isNull()) {
        layer->xmlLoadState(payload);
      }
    }
    if (!visible) {
      setLayerVisible(*topic_id_opt, false);
    }
  }
  syncViewLayers();
  refreshView();
  return true;
}

LayerFactory& SceneDockWidget::layerFactory() {
  return factory_;
}

const LayerFactory& SceneDockWidget::layerFactory() const {
  return factory_;
}

SessionManager* SceneDockWidget::sessionManager() const {
  return session_;
}

QString SceneDockWidget::xmlTag() const {
  return QStringLiteral("scene");
}

bool SceneDockWidget::handleSceneConfigTopic(
    ObjectTopicId /*topic_id*/, sdk::BuiltinObjectType /*object_type*/, const QString& /*title*/) {
  return false;
}

void SceneDockWidget::refreshView() {}

void SceneDockWidget::ensureSceneViewCreated() {
  if (scene_view_ != nullptr) {
    return;
  }
  auto* layout = qobject_cast<QVBoxLayout*>(this->layout());
  if (layout == nullptr) {
    layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
  }
  scene_view_ = createSceneView();
  if (scene_view_ == nullptr) {
    scene_view_ = new QWidget(this);
  }
  scene_view_->setParent(this);
  scene_view_->setContentsMargins(0, 0, 0, 0);
  scene_view_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  layout->addWidget(scene_view_);
}

PJ::Timepoint SceneDockWidget::clampToLayerRange(PJ::Timepoint time) const {
  // Latched-layer rule (semantics ported from pj_scene3d_core's
  // clampTrackerTimeToRanges, which the 3D dock used before migrating onto this
  // base): a *spanning* layer (first < last) bounds both ends; a *latched /
  // one-shot* layer (first == last, e.g. a map pinned to the recording start)
  // is valid from its stamp ONWARD — it lowers `lo` but must NOT cap `hi`, or
  // its lone early stamp would drag the live playhead backwards and hide
  // everything keyed to "now" (the old "TF doesn't render unless another topic
  // is present" bug). With no spanning layer there is no upper bound; with no
  // usable range the time passes through unchanged. `lo`/`hi` are guarded by
  // have_lo/have_hi — never a default Timepoint (which is epoch, not a bound).
  bool have_lo = false;
  bool have_hi = false;
  PJ::Timepoint lo{};
  PJ::Timepoint hi{};
  for (const auto& [key, layer] : layers_) {
    if (layer == nullptr) {
      continue;
    }
    const auto range = layer->timeRange();
    const PJ::Timepoint first = range.min;
    const PJ::Timepoint last = range.max;
    if (last < first) {
      continue;  // inverted: no data
    }
    if (!have_lo || first < lo) {
      lo = first;
      have_lo = true;
    }
    if (last > first && (!have_hi || last > hi)) {
      hi = last;
      have_hi = true;
    }
  }
  if (!have_lo) {
    return time;
  }
  if (time < lo) {
    return lo;
  }
  if (have_hi && time > hi) {
    return hi;
  }
  return time;
}

std::vector<ISceneLayer*> SceneDockWidget::orderedLayerPtrs() const {
  std::vector<ISceneLayer*> ordered;
  ordered.reserve(draw_order_.size());
  for (const int64_t key : draw_order_) {
    const auto it = layers_.find(key);
    if (it != layers_.end() && it->second != nullptr) {
      ordered.push_back(it->second.get());
    }
  }
  return ordered;
}

void SceneDockWidget::syncViewLayers() {
  syncViewLayers(orderedLayerPtrs());
}

void SceneDockWidget::clearLayers() {
  if (layers_.empty()) {
    return;
  }
  // Mirror removeTopic()'s ordering for the clear-all case: re-point the view
  // off every layer (a sync with an empty draw order) while the layers are
  // still alive, so a view holding raw layer pointers can drop and release
  // them safely (the 3D view releases each layer's GL resources here). Only
  // then detach; the retired layers are destroyed at scope exit.
  auto retired = std::move(layers_);
  layers_.clear();
  draw_order_.clear();
  layer_visibility_cache_.clear();
  syncViewLayers();
  refreshView();
  for (auto& [key, layer] : retired) {
    if (layer != nullptr) {
      layer->detach();
    }
  }
}

void SceneDockWidget::destroyLayersUnsynced() {
  for (auto& [key, layer] : layers_) {
    if (layer != nullptr) {
      layer->detach();
    }
  }
  layers_.clear();
  draw_order_.clear();
  layer_visibility_cache_.clear();
}

void SceneDockWidget::recordLayerVisibility(ObjectTopicId topic_id, bool visible) {
  const int64_t key = topicKey(topic_id);
  auto it = layer_visibility_cache_.find(key);
  if (it != layer_visibility_cache_.end() && it->second == visible) {
    return;
  }
  layer_visibility_cache_[key] = visible;
  emit layerVisibilityChanged(topic_id, visible);
}

}  // namespace PJ
