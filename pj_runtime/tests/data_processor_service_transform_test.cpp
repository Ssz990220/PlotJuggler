// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// M4.2 Task 4 — the DataProcessorService *transform* API (plugin-created,
// named, owned, persisted nodes), distinct from the per-curve filter path.
// Transforms share the eager DerivedEngine substrate but live in their own
// recipe map keyed by "<plugin_id>/<id>".

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/DataProcessorService.h"

namespace PJ {
namespace {

// A self-contained Luau transform class. First line is the backend directive the
// host dispatches on (data-only ABI contract). `id` must match the transform id.
constexpr const char* kNegate = R"LUAU(-- pj-script: luau
return { id="negate", name="Negate", output="same",
  create = function(p) return { calculate = function(t, v) return -v end } end }
)LUAU";

constexpr const char* kTimes10 = R"LUAU(-- pj-script: luau
return { id="negate", name="Negate", output="same",
  create = function(p) return { calculate = function(t, v) return v * 10.0 end } end }
)LUAU";

// A class whose calculate() raises a runtime error on the first sample.
constexpr const char* kBoom = R"LUAU(-- pj-script: luau
return { id="boom", name="Boom",
  create = function(p) return { calculate = function(t, v) error("boom") end } end }
)LUAU";

// A 1-input / 2-output MIMO class: returns (value, -value).
constexpr const char* kSplit = R"LUAU(-- pj-script: luau
return { id="split", name="Split",
  create = function(p) return { calculate = function(t, v) return v, -v end } end }
)LUAU";

// Read one column of a topic as doubles (readValues above is column-0 only).
std::vector<double> readColumn(DataEngine& engine, TopicId tid, std::size_t col) {
  std::vector<double> out;
  const TopicStorage* storage = engine.getTopicStorage(tid);
  if (!storage) {
    return out;
  }
  auto cursor = rangeQuery(storage->sealedChunks(), 0, std::numeric_limits<Timestamp>::max());
  cursor.forEach([&](const SampleRow& row) { out.push_back(row.chunk->readNumericAsDouble(col, row.row_index)); });
  return out;
}

std::vector<double> readValues(DataEngine& engine, TopicId tid) {
  std::vector<double> out;
  const TopicStorage* storage = engine.getTopicStorage(tid);
  if (!storage) {
    return out;
  }
  auto cursor = rangeQuery(storage->sealedChunks(), 0, std::numeric_limits<Timestamp>::max());
  cursor.forEach([&](const SampleRow& row) { out.push_back(row.chunk->readNumericAsDouble(0, row.row_index)); });
  return out;
}

// Build a single float64 series named `name` in a fresh dataset and commit the
// given samples (one per second). Returns the dataset id and input topic id.
struct Fixture {
  DataEngine engine;
  DataProcessorService service{engine};
  DatasetId ds = 0;
  TopicId input = 0;

  void seed(const char* name, const std::vector<double>& values) {
    ds = *engine.createDataset(DatasetDescriptor{.source_name = "s", .time_domain_id = 0});
    DataWriter writer = engine.createWriter();
    auto handle = writer.registerScalarSeries(ds, name, NumericType::kFloat64);
    input = handle->topic_id;
    for (std::size_t i = 0; i < values.size(); ++i) {
      writer.appendScalar(*handle, static_cast<Timestamp>(i) * 1'000'000'000LL, values[i]);
    }
    engine.commitChunks(writer.flushAll());
  }

  // Build a schemaless (schema_id == 0) multi-column topic — the shape a JSON
  // scalar-only ingest produces — with the given named columns. `field_paths` are the
  // stored column descriptors (dotted for nested JSON, e.g. "orientation.w"); each
  // `columns[c]` feeds field_paths[c]. Returns via `input`/`ds`.
  void seedMultiColumn(
      const char* topic_name, const std::vector<std::string>& field_paths,
      const std::vector<std::vector<double>>& columns) {
    ds = *engine.createDataset(DatasetDescriptor{.source_name = "s", .time_domain_id = 0});
    DataWriter writer = engine.createWriter();
    input = *writer.registerTopic(ds, TopicDescriptor{.name = topic_name, .schema_id = 0, .dataset_id = ds});
    for (const auto& path : field_paths) {
      (void)writer.ensureColumn(input, path, PrimitiveType::kFloat64);
    }
    const std::size_t rows = columns.empty() ? 0 : columns.front().size();
    for (std::size_t r = 0; r < rows; ++r) {
      EXPECT_TRUE(writer.beginRow(input, static_cast<Timestamp>(r) * 1'000'000'000LL).has_value());
      for (std::size_t c = 0; c < field_paths.size(); ++c) {
        writer.set<double>(input, c, columns[c][r]);
      }
      EXPECT_TRUE(writer.finishRow(input).has_value());
    }
    engine.commitChunks(writer.flushAll());
  }

  [[nodiscard]] bool listed(TopicId t) const {
    const auto v = engine.listTopics(ds);
    return std::find(v.begin(), v.end(), t) != v.end();
  }
};

TEST(DataProcessorTransformTest, UpsertCreatesNamedTopic) {
  Fixture fx;
  fx.seed("speed", {-1.0, 2.0, -3.0});

  const auto rec = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"speed/negated"}, kNegate, "{}");
  ASSERT_TRUE(rec.has_value()) << rec.error();
  EXPECT_EQ(rec->key, "pluginA/negate");
  EXPECT_EQ(rec->owner_plugin, "pluginA");
  EXPECT_EQ(rec->user_id, "negate");
  EXPECT_EQ(rec->backend, "luau");
  EXPECT_EQ(rec->dataset_id, fx.ds);  // output dataset = the input topic's dataset
  ASSERT_EQ(rec->output_topic_ids.size(), 1u);

  const TopicId out = rec->output_topic_ids.front();
  EXPECT_TRUE(fx.listed(out));
  EXPECT_EQ(readValues(fx.engine, out), (std::vector<double>{1.0, -2.0, 3.0}));
}

TEST(DataProcessorTransformTest, UpsertReplacesExistingNode) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});

  const auto first = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"speed/out"}, kNegate, "{}");
  ASSERT_TRUE(first.has_value()) << first.error();
  EXPECT_EQ(readValues(fx.engine, first->output_topic_ids.front()), (std::vector<double>{-1.0, -2.0}));

  // Same key, new script -> replace in place; one recipe, new behaviour.
  const auto second = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"speed/out"}, kTimes10, "{}");
  ASSERT_TRUE(second.has_value()) << second.error();
  EXPECT_EQ(fx.service.transformRecipes().size(), 1u);
  EXPECT_EQ(readValues(fx.engine, second->output_topic_ids.front()), (std::vector<double>{10.0, 20.0}));
}

TEST(DataProcessorTransformTest, RemoveRetiresTopic) {
  Fixture fx;
  fx.seed("speed", {1.0});
  const auto rec = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"speed/out"}, kNegate, "{}");
  ASSERT_TRUE(rec.has_value()) << rec.error();
  const TopicId out = rec->output_topic_ids.front();
  ASSERT_TRUE(fx.listed(out));

  ASSERT_TRUE(fx.service.removeTransform("pluginA/negate").has_value());
  EXPECT_FALSE(fx.listed(out));  // retired -> dropped from the catalog
  EXPECT_TRUE(fx.service.transformRecipes().empty());

  // Removing an unknown key is an error.
  EXPECT_FALSE(fx.service.removeTransform("pluginA/ghost").has_value());
}

TEST(DataProcessorTransformTest, RollbackLeavesNoZombieTopic) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});

  // A runtime-erroring script must fail the upsert AND leave the output name free.
  EXPECT_FALSE(fx.service.upsertTransform("pluginA", "boom", {"speed"}, {"speed/out"}, kBoom, "{}").has_value());
  EXPECT_TRUE(fx.service.transformRecipes().empty());

  // A healthy transform can re-use the freed name.
  const auto ok = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"speed/out"}, kNegate, "{}");
  EXPECT_TRUE(ok.has_value()) << ok.error();
}

TEST(DataProcessorTransformTest, OutputNameCollisionRejected) {
  Fixture fx;
  fx.seed("speed", {1.0});
  const auto a = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"speed/out"}, kNegate, "{}");
  ASSERT_TRUE(a.has_value()) << a.error();

  // A different transform claiming the same output name is rejected, not clobbered.
  const auto b = fx.service.upsertTransform("pluginB", "other", {"speed"}, {"speed/out"}, kNegate, "{}");
  EXPECT_FALSE(b.has_value());
  // A's output is untouched and A's recipe survives.
  EXPECT_EQ(fx.service.transformRecipes().size(), 1u);
  EXPECT_TRUE(fx.listed(a->output_topic_ids.front()));
}

// Field-level input binding on a schemaless topic: a "<topic>/<field>" input must
// resolve to the named column (separator normalized '/'->'.'), not topic column 0.
// Regression: schema_id == 0 topics (JSON scalar-only ingest) were unresolvable by
// field name, and nested fields failed on the '/' vs '.' separator mismatch.
TEST(DataProcessorTransformTest, ResolvesSchemalessNestedFieldInput) {
  Fixture fx;
  // /imu with dotted nested columns; "orientation.w" is the LAST column (index 3),
  // so resolving to column 0 instead would negate the wrong series.
  fx.seedMultiColumn(
      "/imu", {"orientation.x", "orientation.y", "orientation.z", "orientation.w"},
      {{5.0, 5.0}, {0.0, 0.0}, {0.0, 0.0}, {1.0, 2.0}});

  const auto rec = fx.service.upsertTransform("pluginA", "negate", {"/imu/orientation/w"}, {"roll"}, kNegate, "{}");
  ASSERT_TRUE(rec.has_value()) << rec.error();
  ASSERT_EQ(rec->output_topic_ids.size(), 1u);
  // Negated "orientation.w" {1,2} -> {-1,-2}; if it read column 0 it would be {-5,-5}.
  EXPECT_EQ(readValues(fx.engine, rec->output_topic_ids.front()), (std::vector<double>{-1.0, -2.0}));
}

// A single structured "topic:f1,f2" output collapses into ONE topic with those
// named columns (single-topic parity), instead of N separate scalar topics.
TEST(DataProcessorTransformTest, GroupsStructuredOutputIntoOneTopic) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0, 3.0});

  const auto rec = fx.service.upsertTransform("pluginA", "split", {"speed"}, {"rpy:a,b"}, kSplit, "{}");
  ASSERT_TRUE(rec.has_value()) << rec.error();
  // Grouped: exactly ONE output topic, not two.
  ASSERT_EQ(rec->output_topic_ids.size(), 1u);
  const TopicId out = rec->output_topic_ids.front();

  const auto lock = fx.engine.lockEngine();
  const TopicStorage* st = fx.engine.getTopicStorage(out);
  ASSERT_NE(st, nullptr);
  EXPECT_EQ(st->descriptor().name, "rpy");
  // The topic's struct schema carries the two suffixes as named leaf columns, in order.
  const PJ::SchemaId schema_id = st->descriptor().schema_id;
  ASSERT_NE(schema_id, 0u);
  const PJ::TypeTreeNode* root = fx.engine.typeRegistry().lookup(schema_id);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(PJ::flattenFieldPaths(*root), (std::vector<std::string>{"a", "b"}));
  // Column 0 = value, column 1 = -value.
  EXPECT_EQ(readColumn(fx.engine, out, 0), (std::vector<double>{1.0, 2.0, 3.0}));
  EXPECT_EQ(readColumn(fx.engine, out, 1), (std::vector<double>{-1.0, -2.0, -3.0}));
}

// Plain (non-structured) output names stay one topic per name (back-compat: the
// transform editor's multi-output path is untouched).
TEST(DataProcessorTransformTest, PlainMimoOutputsStaySeparateTopics) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});

  const auto rec = fx.service.upsertTransform("pluginA", "split", {"speed"}, {"alpha", "beta"}, kSplit, "{}");
  ASSERT_TRUE(rec.has_value()) << rec.error();
  EXPECT_EQ(rec->output_topic_ids.size(), 2u);  // two separate scalar topics
}

TEST(DataProcessorTransformTest, InputNameNotFoundRejected) {
  Fixture fx;
  fx.seed("speed", {1.0});
  const auto rec = fx.service.upsertTransform("pluginA", "negate", {"nonexistent"}, {"out"}, kNegate, "{}");
  EXPECT_FALSE(rec.has_value());
}

TEST(DataProcessorTransformTest, ClearForPluginRemovesOnlyThatPlugin) {
  Fixture fx;
  fx.seed("speed", {1.0});
  // Same user_id under two plugins -> distinct keys "pluginA/negate" / "pluginB/negate"
  // (the transform id must match the script's class id, kNegate's "negate").
  ASSERT_TRUE(fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"a/out"}, kNegate, "{}").has_value());
  const auto b = fx.service.upsertTransform("pluginB", "negate", {"speed"}, {"b/out"}, kNegate, "{}");
  ASSERT_TRUE(b.has_value()) << b.error();

  fx.service.clearTransformsForPlugin("pluginA");

  EXPECT_TRUE(fx.service.transformIdsForPlugin("pluginA").empty());
  ASSERT_EQ(fx.service.transformIdsForPlugin("pluginB").size(), 1u);
  EXPECT_EQ(fx.service.transformIdsForPlugin("pluginB").front(), "negate");
  EXPECT_TRUE(fx.listed(b->output_topic_ids.front()));  // B untouched
}

TEST(DataProcessorTransformTest, MissingBackendDiagnosesNotDrops) {
  Fixture fx;
  fx.seed("speed", {1.0});

  // A persisted recipe whose backend is unavailable in this build (python).
  DataProcessorService::TransformRecipe py;
  py.key = "pluginA/py";
  py.owner_plugin = "pluginA";
  py.user_id = "py";
  py.inputs = {"speed"};
  py.outputs = {"speed/out"};
  py.script = "# pj-script: python\nreturn None";
  py.params_json = "{}";

  const auto restored = fx.service.restoreTransform(py);
  EXPECT_FALSE(restored.has_value());                  // diagnosed, not silently materialized
  EXPECT_TRUE(fx.service.transformRecipes().empty());  // service did not store it (caller keeps the XML)
}

TEST(DataProcessorTransformTest, RestoreRoundTripRebindsAndRuns) {
  Fixture fx;
  fx.seed("speed", {-1.0, 2.0});
  ASSERT_TRUE(fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"speed/out"}, kNegate, "{}").has_value());

  // Snapshot the recipe the way the layout serializer would, then reconcile.
  const auto recipes = fx.service.transformRecipes();
  ASSERT_EQ(recipes.size(), 1u);
  const DataProcessorService::TransformRecipe snapshot = recipes.front();

  fx.service.clearAllTransforms();
  ASSERT_TRUE(fx.service.transformRecipes().empty());

  const auto restored = fx.service.restoreTransform(snapshot);
  ASSERT_TRUE(restored.has_value()) << restored.error();
  EXPECT_EQ(readValues(fx.engine, restored->output_topic_ids.front()), (std::vector<double>{1.0, -2.0}));
}

}  // namespace
}  // namespace PJ
