// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/AppSession.h"

#include <algorithm>
#include <limits>
#include <optional>
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
  QObject::connect(
      catalog_model_.get(), &CatalogModel::cleared, this, [this]() { session_manager_->curveColorRegistry().clear(); });
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

  // Union the bounds in DISPLAY-relative seconds, converting each dataset with
  // its OWN offset, so the playback axis matches what the plots render
  // (display_time = raw_time - offset) rather than the absolute epoch.
  std::optional<DisplaySeconds> new_min;
  std::optional<DisplaySeconds> new_max;

  for (const DatasetId dataset_id : reader.listDatasets()) {
    const DisplayOffset offset = session_manager_->displayOffset(dataset_id);

    Timestamp raw_min = std::numeric_limits<Timestamp>::max();
    Timestamp raw_max = std::numeric_limits<Timestamp>::min();
    bool found = false;

    for (const TopicId topic_id : reader.listTopics(dataset_id)) {
      const auto metadata = reader.getMetadata(topic_id);
      if (!metadata.has_value() || metadata->total_row_count == 0) {
        continue;
      }
      raw_min = std::min(raw_min, metadata->time_range_min);
      raw_max = std::max(raw_max, metadata->time_range_max);
      found = true;
    }

    for (const ObjectTopicId object_topic_id : object_store.listTopics(dataset_id)) {
      if (object_store.entryCount(object_topic_id) == 0) {
        continue;
      }
      const auto [object_min, object_max] = object_store.timeRange(object_topic_id);
      raw_min = std::min(raw_min, object_min);
      raw_max = std::max(raw_max, object_max);
      found = true;
    }

    if (!found) {
      continue;
    }

    const DisplaySeconds ds_min = rawToDisplaySeconds(raw_min, offset);
    const DisplaySeconds ds_max = rawToDisplaySeconds(raw_max, offset);
    new_min = new_min ? std::min(*new_min, ds_min) : ds_min;
    new_max = new_max ? std::max(*new_max, ds_max) : ds_max;
  }

  if (!new_min) {
    return false;
  }

  if (!playback_seeded_) {
    // First load: snap range and currentTime to data bounds.
    playback_engine_->setRange(DisplayRange{*new_min, *new_max});
    playback_engine_->setCurrentTime(*new_min);
    playback_seeded_ = true;
    return true;
  }

  // Subsequent loads expand the range monotonically; setRange re-clamps the playhead.
  const DisplaySeconds union_min = std::min(playback_engine_->rangeMin(), *new_min);
  const DisplaySeconds union_max = std::max(playback_engine_->rangeMax(), *new_max);
  playback_engine_->setRange(DisplayRange{union_min, union_max});
  return true;
}

}  // namespace PJ
