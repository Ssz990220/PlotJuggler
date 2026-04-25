#pragma once

#include <QMainWindow>
#include <memory>

namespace Ui {
class MainWindow;
}

namespace PJ {

class AppSession;
class FileLoader;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

 private slots:
  void onOpenMarketplace();
  void onLoadDataRequested();

 private:
  void refreshStreamingCombo();

  Ui::MainWindow* ui_;
  std::unique_ptr<AppSession> session_;
  std::unique_ptr<FileLoader> file_loader_;
};

}  // namespace PJ
