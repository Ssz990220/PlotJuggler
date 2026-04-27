#pragma once

#include <QtCore/QtGlobal>
#include <optional>
#include <string>
#include <string_view>

#include "pj_app_core/ExtensionCatalogService.h"
#include "pj_plugins/dialog_protocol.h"

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace PJ {

class DataSourceHandle;

namespace dialog_presenter {

// Outcome of presenting a plugin's configuration dialog.
//
// kNoDialog covers two distinct cases that callers don't need to
// distinguish: (a) the plugin doesn't advertise kCapabilityHasDialog,
// or (b) it advertises but didn't ship a usable vtable / context. Either
// way the caller should proceed with whatever config it had on entry.
enum class Outcome { kNoDialog, kAccepted, kRejected };

// Configs returned by an accepted dialog. parser_config is empty when
// the dialog has no pj_parser_slot widget.
struct AcceptedPayload {
  std::string saved_config;
  std::string parser_config;
};

// Result of a data-source dialog. payload is engaged iff outcome == kAccepted —
// the type expresses what would otherwise live only in a comment.
struct DataSourceResult {
  Outcome outcome = Outcome::kNoDialog;
  std::optional<AcceptedPayload> payload;
};

// Inputs for showDataSourceDialog. Preconditions (NOT checked by the helper):
//   * `handle` has been bind()-ed.
//   * Any runtime-host callbacks the plugin needs (message-box, etc.) are
//     installed on the runtime host before bind() — see RuntimeHost in
//     FileLoader.cpp for the pattern.
// Synchronous; helper does not extend `handle` or `catalog` lifetime past
// return. A future modeless caller would have to revisit both.
struct DataSourceRequest {
  const LoadedDataSource& source;
  DataSourceHandle& handle;
  const ExtensionCatalogService& catalog;
  QWidget* parent = nullptr;
  std::string_view initial_parser_config{};
};

// Caller decides what to persist and whether to re-loadConfig before start().
DataSourceResult showDataSourceDialog(const DataSourceRequest& req);

}  // namespace dialog_presenter

}  // namespace PJ
