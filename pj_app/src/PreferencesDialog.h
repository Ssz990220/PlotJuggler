#pragma once

#include <QDialog>

namespace Ui {
class PreferencesDialog;
}

namespace PJ {

class Theme;

// Application preferences. Currently just the theme selector — anchors
// the home for future preferences (precision, OpenGL, plugin folders…).
// On accept, calls Theme::setTheme() with the selected name; on cancel,
// the dialog closes without touching anything.
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
