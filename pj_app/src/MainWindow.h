#pragma once

#include <QMainWindow>

#include <memory>

#include "pj_app_core/AppSession.h"

namespace ads {
class CDockManager;
}

namespace PJ {

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

 private:
  std::unique_ptr<AppSession> session_;
  ads::CDockManager* dock_manager_ = nullptr;
};

}  // namespace PJ
