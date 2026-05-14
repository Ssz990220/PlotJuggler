#pragma once

#include <QString>

#include "pj_widgets/Dialog.h"

namespace Ui {
class PreferencesDialog;
}

namespace PJ {

class Theme;

class PreferencesDialog : public Dialog {
  Q_OBJECT
 public:
  explicit PreferencesDialog(Theme& theme, QWidget* parent = nullptr);
  ~PreferencesDialog() override;

 private:
  Ui::PreferencesDialog* ui_;
  Theme& theme_;
  // Snapshot taken at construction so Cancel can revert any live
  // preview the user triggered via the toggle.
  QString original_theme_;
};

}  // namespace PJ
