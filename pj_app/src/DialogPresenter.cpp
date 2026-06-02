// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "DialogPresenter.h"

#include <QLoggingCategory>
#include <QString>
#include <utility>

#include "pj_base/data_source_protocol.h"
#include "pj_plugins/host/data_source_handle.hpp"
#include "pj_plugins/host/data_source_library.hpp"
#include "pj_plugins/host/dialog_handle.hpp"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_plugins/host/plugin_runtime_catalog.hpp"
#include "pj_plugins/host_qt/dialog_engine.hpp"
#include "pj_runtime/ExtensionCatalogService.h"

namespace PJ::dialog_presenter {

namespace {

Q_LOGGING_CATEGORY(lcDialogPresenter, "pj.app.dialogpresenter")

// Adapter from ExtensionCatalogService::findParserByEncoding to the
// QueryParserDialogFn shape DialogEngine expects. Returns the parser's
// dialog vtable for a given encoding, or nullptr when the parser doesn't
// exist or doesn't expose a dialog. Captures the catalog by reference —
// callers must keep the catalog alive for the duration of the dialog.
QueryParserDialogFn makeParserDialogProvider(const ExtensionCatalogService& catalog) {
  return [&catalog](const std::string& encoding) -> const PJ_dialog_vtable_t* {
    const auto* parser = catalog.findParserByEncoding(QString::fromStdString(encoding));
    if (parser == nullptr) {
      return nullptr;
    }
    auto vtable = parser->library.resolveDialogVtable();
    return vtable ? *vtable : nullptr;
  };
}

DataSourceResult contractViolation(const std::string& plugin_name, const std::string& reason) {
  qCWarning(lcDialogPresenter).noquote() << "Plugin" << QString::fromStdString(plugin_name)
                                         << "advertises kCapabilityHasDialog but" << QString::fromStdString(reason);
  return {.outcome = Outcome::kPluginContractViolation, .error = "plugin '" + plugin_name + "': " + reason};
}

}  // namespace

DataSourceResult showDataSourceDialog(const DataSourceRequest& req) {
  if ((req.source.capabilities & PJ_DATA_SOURCE_CAPABILITY_HAS_DIALOG) == 0) {
    return {};
  }

  auto vtable_result = req.source.library.resolveDialogVtable();
  if (!vtable_result) {
    return contractViolation(req.source.name, "resolveDialogVtable failed: " + vtable_result.error());
  }

  const PJ_borrowed_dialog_t borrowed = req.handle.getDialog();
  if (borrowed.ctx == nullptr) {
    return contractViolation(req.source.name, "getDialog() returned null context");
  }

  DialogEngineConfig engine_config;
  engine_config.parser_dialog_provider = makeParserDialogProvider(req.catalog);
  // string-from-string_view ctor (C++17) handles a default-empty view safely;
  // raw .assign(data(), size()) would be UB when data() is nullptr.
  engine_config.initial_parser_config = std::string(req.initial_parser_config);

  DialogEngine engine(DialogHandle::borrowed(*vtable_result, borrowed.ctx), std::move(engine_config));
  if (engine.showDialog(req.parent) == DialogResult::kRejected) {
    return {.outcome = Outcome::kRejected};
  }

  return {
      .outcome = Outcome::kAccepted,
      .payload = AcceptedPayload{.saved_config = engine.savedConfig(), .parser_config = engine.parserConfig()},
  };
}

}  // namespace PJ::dialog_presenter
