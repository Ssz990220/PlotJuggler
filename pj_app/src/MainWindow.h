#pragma once

#include <QMainWindow>

#include <memory>

namespace Ui {
class MainWindow;
}

namespace PJ {

class AppSession;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

 private slots:
  void onOpenMarketplace();

 private:
  void refreshStreamingCombo();

  Ui::MainWindow* ui_;
  std::unique_ptr<AppSession> session_;
};

}  // namespace PJ
