#include "pj_app_core/AppSession.h"

#include <utility>

#include "pj_app_core/CatalogModel.h"
#include "pj_app_core/ExtensionCatalogService.h"
#include "pj_app_core/PlaybackEngine.h"
#include "pj_app_core/SessionManager.h"

namespace PJ {

AppSession::AppSession(QObject* parent) : AppSession(QString{}, parent) {}

AppSession::AppSession(QString extensions_dir, QObject* parent)
    : AppSession(std::move(extensions_dir), DiagnosticSink{}, parent) {}

AppSession::AppSession(QString extensions_dir, DiagnosticSink sink, QObject* parent)
    : QObject(parent),
      session_manager_(std::make_unique<SessionManager>()),
      playback_engine_(std::make_unique<PlaybackEngine>()),
      catalog_model_(std::make_unique<CatalogModel>(session_manager_.get())),
      extension_catalog_(std::make_unique<ExtensionCatalogService>(std::move(extensions_dir), std::move(sink))) {}

AppSession::~AppSession() = default;

}  // namespace PJ
