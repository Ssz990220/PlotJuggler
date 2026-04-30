#pragma once

#include <QDialog>

namespace Ui {
class PreferencesDialog;
}

namespace PJ {

class Theme;

class PreferencesDialog : public QDialog {
  Q_OBJECT
 public:
  explicit PreferencesDialog(Theme& theme, QWidget* parent = nullptr);
  ~PreferencesDialog() override;

 private slots:
  void onAccepted();

 private:
  Ui::PreferencesDialog* ui_;
  Theme& theme_;
};

}  // namespace PJ
