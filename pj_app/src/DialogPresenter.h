#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QtCore/QtGlobal>
#include <optional>
#include <string>
#include <string_view>

#include "pj_plugins/dialog_protocol.h"
#include "pj_runtime/ExtensionCatalogService.h"

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace PJ {

class DataSourceHandle;

namespace dialog_presenter {

// Outcome of presenting a plugin's configuration dialog.
//
// kNoDialog and kPluginContractViolation are NOT interchangeable:
//   * kNoDialog: the plugin doesn't advertise kCapabilityHasDialog.
//     Caller should proceed with whatever config it had on entry.
//   * kPluginContractViolation: the plugin advertised kCapabilityHasDialog
//     but didn't ship a usable vtable, or getDialog() returned a null ctx.
//     This is a plugin bug — silently proceeding produces wrong data with
//     no user-visible error. Caller should fail the operation and surface
//     `error` to the user so the broken plugin can be reinstalled / fixed.
enum class Outcome { kNoDialog, kAccepted, kRejected, kPluginContractViolation };

// Configs returned by an accepted dialog. parser_config is empty when
// the dialog has no pj_parser_slot widget.
struct AcceptedPayload {
  std::string saved_config;
  std::string parser_config;
};

// Result of a data-source dialog. payload is engaged iff outcome == kAccepted;
// error is non-empty iff outcome == kPluginContractViolation.
// NSDMIs keep partial designated-init construction warning-clean under
// -Wmissing-field-initializers (pj_app builds with PJ_WARNING_FLAGS).
struct DataSourceResult {
  Outcome outcome = Outcome::kNoDialog;
  std::optional<AcceptedPayload> payload = std::nullopt;
  std::string error = {};
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
