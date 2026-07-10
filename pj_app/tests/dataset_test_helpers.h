// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

// Shared gtest fixtures for the pj_app dataset-identity tests
// (multi_dataset_rebind_test, pending_display_binder_test): create a dataset and
// append a two-sample scalar topic through the live AppSession, then refresh the
// catalog. Failures are signaled with ADD_FAILURE (not ASSERT_*) so the helpers
// stay usable from value-returning call sites; the caller checks the returned id
// (0 = failure) where it cares.

#include <gtest/gtest.h>

#include <string_view>

#include "pj_datastore/writer.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"

namespace pj_test {

// Creates a dataset with the given raw source_name (defaults to "test" for the
// single-dataset tests that don't care about the label). Returns 0 on failure.
inline PJ::DatasetId createDataset(PJ::AppSession& app_session, std::string_view source_name = "test") {
  auto dataset = app_session.sessionManager().dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = std::string(source_name)});
  if (!dataset.has_value()) {
    ADD_FAILURE() << dataset.error();
    return 0;
  }
  return *dataset;
}

// Appends a two-sample float64 scalar series named `topic_name` to `dataset_id`,
// commits it, and rebuilds the catalog. Returns the new topic's id (0 on failure).
inline PJ::TopicId addScalarTopic(PJ::AppSession& app_session, PJ::DatasetId dataset_id, std::string_view topic_name) {
  PJ::DataWriter writer = app_session.sessionManager().dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  if (!handle.has_value()) {
    ADD_FAILURE() << handle.error();
    return 0;
  }
  writer.appendScalar(*handle, 100, 1.0);
  writer.appendScalar(*handle, 200, 2.0);
  if (app_session.sessionManager().commitChunks(writer.flushAll()).empty()) {
    ADD_FAILURE() << "commit produced no changed topics";
    return 0;
  }
  app_session.catalogModel().rebuildFromDatastore();
  return handle->topic_id;
}

}  // namespace pj_test
