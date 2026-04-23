#include "pj_app_core/AppSession.h"

#include "pj_app_core/CatalogModel.h"
#include "pj_app_core/PlaybackEngine.h"
#include "pj_app_core/SessionManager.h"

namespace PJ {

AppSession::AppSession(QObject* parent)
    : QObject(parent),
      session_manager_(std::make_unique<SessionManager>()),
      playback_engine_(std::make_unique<PlaybackEngine>()),
      catalog_model_(std::make_unique<CatalogModel>(session_manager_.get())) {}

AppSession::~AppSession() = default;

}  // namespace PJ
