#pragma once

#include <QObject>
#include <QString>

#include <memory>
#include <vector>

namespace PJ {

class SessionManager;

// Qt-side facade over the catalog of topics/curves known to the current
// session. Populated as data sources load; GUI views (CurveListPanel,
// catalog trees in widget families) subscribe to the add/remove signals.
class CatalogModel : public QObject {
  Q_OBJECT
 public:
  using Ptr = std::shared_ptr<CatalogModel>;

  explicit CatalogModel(SessionManager* session = nullptr, QObject* parent = nullptr);
  ~CatalogModel() override;

  CatalogModel(const CatalogModel&) = delete;
  CatalogModel& operator=(const CatalogModel&) = delete;

  std::vector<QString> curveNames() const;

 signals:
  void curveAdded(const QString& name);
  void curveRemoved(const QString& name);
  void cleared();

 private:
  SessionManager* session_ = nullptr;
};

}  // namespace PJ
