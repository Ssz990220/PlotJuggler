#include "pj_runtime/AppSession.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"

namespace PJ {

namespace {

constexpr double kNanosecondsPerSecond = 1.0e9;

}  // namespace

AppSession::AppSession(QObject* parent) : AppSession(QString{}, parent) {}

AppSession::AppSession(QString extensions_dir, QObject* parent)
    : AppSession(std::move(extensions_dir), DiagnosticSink{}, parent) {}

AppSession::AppSession(QString extensions_dir, DiagnosticSink sink, QObject* parent)
    : QObject(parent),
      session_manager_(std::make_unique<SessionManager>()),
      playback_engine_(std::make_unique<PlaybackEngine>()),
      catalog_model_(std::make_unique<CatalogModel>(session_manager_.get())),
      extension_catalog_(std::make_unique<ExtensionCatalogService>(std::move(extensions_dir), std::move(sink))) {}

AppSession::~AppSession() {
  catalog_model_.reset();
  session_manager_.reset();
  playback_engine_.reset();
  extension_catalog_.reset();
}

bool AppSession::seedPlaybackFromSession() {
  const DataReader reader = session_manager_->createReader();
  const ObjectStore& object_store = session_manager_->objectStore();

  Timestamp t_min = std::numeric_limits<Timestamp>::max();
  Timestamp t_max = std::numeric_limits<Timestamp>::min();
  bool found = false;

  for (const DatasetId dataset_id : reader.listDatasets()) {
    for (const TopicId topic_id : reader.listTopics(dataset_id)) {
      const auto metadata = reader.getMetadata(topic_id);
      if (!metadata.has_value() || metadata->total_row_count == 0) {
        continue;
      }
      t_min = std::min(t_min, metadata->time_range_min);
      t_max = std::max(t_max, metadata->time_range_max);
      found = true;
    }

    for (const ObjectTopicId object_topic_id : object_store.listTopics(dataset_id)) {
      if (object_store.entryCount(object_topic_id) == 0) {
        continue;
      }
      const auto [object_min, object_max] = object_store.timeRange(object_topic_id);
      t_min = std::min(t_min, object_min);
      t_max = std::max(t_max, object_max);
      found = true;
    }
  }

  if (!found) {
    return false;
  }

  const double new_min_sec = static_cast<double>(t_min) / kNanosecondsPerSecond;
  const double new_max_sec = static_cast<double>(t_max) / kNanosecondsPerSecond;

  if (!playback_seeded_) {
    // First load: snap range and currentTime to data bounds.
    playback_engine_->setRange(new_min_sec, new_max_sec);
    playback_engine_->setCurrentTime(new_min_sec);
    playback_seeded_ = true;
    return true;
  }

  // Subsequent loads: expand monotonically; keep the user's current playhead
  // unless it is now outside the union range (PlaybackEngine::setRange will
  // clamp via setCurrentTime if needed).
  const double union_min = std::min(playback_engine_->rangeMin(), new_min_sec);
  const double union_max = std::max(playback_engine_->rangeMax(), new_max_sec);
  playback_engine_->setRange(union_min, union_max);
  return true;
}

}  // namespace PJ
