// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/AppSession.h"

#include <algorithm>
#include <optional>
#include <tuple>
#include <unordered_set>
#include <utility>

#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/CurveColorRegistry.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

namespace PJ {

namespace {}  // namespace

CurveColorRegistry& AppSession::curveColorRegistry() const {
  return session_manager_->curveColorRegistry();
}

AppSession::AppSession(QObject* parent) : AppSession(QString{}, parent) {}

AppSession::AppSession(QString extensions_dir, QObject* parent)
    : AppSession(std::move(extensions_dir), DiagnosticSink{}, parent) {}

AppSession::AppSession(QString extensions_dir, DiagnosticSink sink, QObject* parent)
    : QObject(parent),
      session_manager_(std::make_unique<SessionManager>()),
      playback_engine_(std::make_unique<PlaybackEngine>()),
      catalog_model_(std::make_unique<CatalogModel>(session_manager_.get())),
      extension_catalog_(std::make_unique<ExtensionCatalogService>(std::move(extensions_dir), std::move(sink))) {
  // Forget remembered curve colors whenever the catalog empties (data cleared
  // or replaced), matching PJ3's per-PlotData COLOR_HINT lifetime so reopening
  // fresh data restarts palette rotation from the first color. The registry is
  // owned by SessionManager; AppSession just wires its session-scoped clear.
  // Re-arm the playback first-seed snap too: after a full clear the next load
  // is a fresh start, so range AND playhead snap to the new data.
  QObject::connect(catalog_model_.get(), &CatalogModel::cleared, this, [this]() {
    session_manager_->curveColorRegistry().clear();
    playback_seeded_ = false;
  });
}

AppSession::~AppSession() {
  catalog_model_.reset();
  session_manager_.reset();
  playback_engine_.reset();
  extension_catalog_.reset();
}

bool AppSession::seedPlaybackFromSession() {
  const DataReader reader = session_manager_->createReader();
  const ObjectStore& object_store = session_manager_->objectStore();

  // Union the bounds in DISPLAY-relative seconds, converting each item with
  // its dataset's OWN offset, so the playback axis matches what the plots
  // render (display_time = raw_time - offset) rather than the absolute epoch.
  // Bounds come from the CATALOG-VISIBLE items at per-topic granularity: the
  // engine retains removed/trashed topics' data (append-only tombstones), and
  // a hidden topic must not stretch the timeline even while sibling topics
  // keep its dataset visible.
  std::optional<DisplaySeconds> new_min;
  std::optional<DisplaySeconds> new_max;

  // Multi-field topics surface one catalog item per field; bound each
  // underlying topic once.
  std::unordered_set<TopicId> seen_topics;
  std::unordered_set<uint32_t> seen_object_topics;
  for (const CatalogItem& item : catalog_model_->items()) {
    Timestamp raw_min = 0;
    Timestamp raw_max = 0;
    if (const ScalarFieldPayload* scalar = asScalarField(item)) {
      if (!seen_topics.insert(scalar->topic_id).second) {
        continue;
      }
      const auto metadata = reader.getMetadata(scalar->topic_id);
      if (!metadata.has_value() || metadata->total_row_count == 0) {
        continue;
      }
      raw_min = metadata->time_range_min;
      raw_max = metadata->time_range_max;
    } else if (const ObjectTopicPayload* object = asObjectTopic(item)) {
      if (!seen_object_topics.insert(object->object_topic_id.id).second) {
        continue;
      }
      if (object_store.entryCount(object->object_topic_id) == 0) {
        continue;
      }
      std::tie(raw_min, raw_max) = object_store.timeRange(object->object_topic_id);
    } else {
      continue;
    }

    const DisplayOffset offset = session_manager_->displayOffset(item.dataset_id);
    const DisplaySeconds ds_min = rawToDisplaySeconds(raw_min, offset);
    const DisplaySeconds ds_max = rawToDisplaySeconds(raw_max, offset);
    new_min = new_min ? std::min(*new_min, ds_min) : ds_min;
    new_max = new_max ? std::max(*new_max, ds_max) : ds_max;
  }

  if (!new_min) {
    // No visible data: keep the current range (nothing better to show). The
    // first-seed snap re-arms through the CatalogModel::cleared() hook.
    return false;
  }

  if (!playback_seeded_) {
    // First load (or first after the catalog emptied): snap range and
    // currentTime to data bounds.
    playback_engine_->setRange(DisplayRange{*new_min, *new_max});
    playback_engine_->setCurrentTime(*new_min);
    playback_seeded_ = true;
    return true;
  }

  // Subsequent loads recompute the range from the visible data — it can grow
  // (additional file) or shrink (dataset removed, shorter reload). setRange
  // re-clamps the playhead; an in-range scrub position is preserved.
  playback_engine_->setRange(DisplayRange{*new_min, *new_max});
  return true;
}

}  // namespace PJ
