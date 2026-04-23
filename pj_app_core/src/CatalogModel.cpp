#include "pj_app_core/CatalogModel.h"

namespace PJ {

CatalogModel::CatalogModel(SessionManager* session, QObject* parent)
    : QObject(parent), session_(session) {}

CatalogModel::~CatalogModel() = default;

std::vector<QString> CatalogModel::curveNames() const {
  return {};
}

}  // namespace PJ
