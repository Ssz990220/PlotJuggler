#pragma once

#include <QtCore/QtGlobal>
#include <string>
#include <string_view>

#include "pj_plugins/dialog_protocol.h"

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace PJ {

class DataSourceHandle;
class ExtensionCatalogService;
struct RuntimeDataSourcePlugin;
using LoadedDataSource = RuntimeDataSourcePlugin;

namespace dialog_presenter {

// Outcome of presenting a plugin's configuration dialog.
//
// kNoDialog covers two distinct cases that callers don't need to
// distinguish: (a) the plugin doesn't advertise kCapabilityHasDialog,
// or (b) it advertises but didn't ship a usable vtable / context. Either
// way the caller should proceed with whatever config it had on entry.
enum class Outcome { kNoDialog, kAccepted, kRejected };

// Result of a data-source dialog. saved_config and parser_config are
// only meaningful when outcome == kAccepted.
struct DataSourceResult {
  Outcome outcome = Outcome::kNoDialog;
  std::string saved_config;
  std::string parser_config;  // empty when the dialog has no pj_parser_slot
};

// Inputs for showDataSourceDialog. Preconditions (NOT checked by the
// helper):
//   * `handle` has been bind()-ed.
//   * Any runtime-host callbacks the plugin needs (message-box, etc.)
//     are already installed on the runtime host. The dialog flow does
//     not own that wiring — see proto_app's setMessageBoxCallback /
//     PJ4's RuntimeHost vtable for how it's done.
//   * `handle` outlives this call. The borrowed dialog ctx is owned by
//     the source plugin instance held by `handle`; the helper is
//     synchronous, so no further lifetime extension is needed. A future
//     modeless caller would need to revisit this.
struct DataSourceRequest {
  const LoadedDataSource& source;
  DataSourceHandle& handle;
  const ExtensionCatalogService& catalog;
  QWidget* parent = nullptr;
  std::string_view initial_parser_config{};
};

// Pop the data-source plugin's configuration dialog modally. Returns
// Outcome::kNoDialog when the plugin doesn't actually expose a dialog,
// Outcome::kRejected when the user cancels, Outcome::kAccepted with
// the dialog's serialized config otherwise. Caller decides what to
// persist and whether to re-loadConfig before start().
DataSourceResult showDataSourceDialog(const DataSourceRequest& req);

}  // namespace dialog_presenter

}  // namespace PJ
