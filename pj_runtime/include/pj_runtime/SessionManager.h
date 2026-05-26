#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"

namespace PJ {

class MessageParserPluginBase;

// Owns the datastore for the current app session. v1 commit calls are expected
// on the GUI thread so plot adapters never observe mutation during paint.
class SessionManager : public QObject {
  Q_OBJECT
 public:
  using Ptr = std::shared_ptr<SessionManager>;

  explicit SessionManager(QObject* parent = nullptr);
  ~SessionManager() override;

  SessionManager(const SessionManager&) = delete;
  SessionManager& operator=(const SessionManager&) = delete;

  [[nodiscard]] DataEngine& dataEngine() noexcept {
    return data_engine_;
  }
  [[nodiscard]] ObjectStore& objectStore() noexcept {
    return object_store_;
  }
  [[nodiscard]] DataReader createReader() const;

  [[nodiscard]] std::vector<TopicId> commitChunks(std::vector<std::pair<TopicId, TopicChunk>> chunks);

  void registerObjectTopicParser(ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser);
  [[nodiscard]] MessageParserPluginBase* parserForObjectTopic(ObjectTopicId id) const;

  struct LoadedSource {
    QString path;
    QString prefix;
    QString plugin_id;           // Empty when the loader didn't record a plugin (e.g. legacy paths).
    QString plugin_config_json;  // Plugin's saveConfig() JSON at load time.
  };

  [[nodiscard]] std::optional<LoadedSource> lastLoadedSource() const noexcept {
    return last_loaded_source_;
  }
  void recordLoadedSource(QString path, QString prefix, QString plugin_id = {}, QString plugin_config_json = {});
  void clearLoadedSource() noexcept {
    last_loaded_source_.reset();
  }

 signals:
  void topicsCommitted(QVector<PJ::TopicId> ids);

 private:
  DataEngine data_engine_;
  ObjectStore object_store_;
  std::unordered_map<uint32_t, std::unique_ptr<MessageParserHandle>> object_topic_parsers_;
  std::optional<LoadedSource> last_loaded_source_;
};

}  // namespace PJ
