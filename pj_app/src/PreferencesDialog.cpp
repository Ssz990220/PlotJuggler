#include "PreferencesDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>

#include "pj_app_core/Theme.h"
#include "ui_PreferencesDialog.h"

namespace PJ {

PreferencesDialog::PreferencesDialog(Theme& theme, QWidget* parent)
    : QDialog(parent), ui_(new Ui::PreferencesDialog), theme_(theme) {
  ui_->setupUi(this);

  const int idx = Theme::availableThemes().indexOf(theme_.currentTheme());
  ui_->comboBoxTheme->setCurrentIndex(idx >= 0 ? idx : 0);

  connect(this, &QDialog::accepted, this, &PreferencesDialog::onAccepted);
}

PreferencesDialog::~PreferencesDialog() {
  delete ui_;
}

void PreferencesDialog::onAccepted() {
  const int idx = ui_->comboBoxTheme->currentIndex();
  const QStringList themes = Theme::availableThemes();
  if (idx >= 0 && idx < themes.size()) {
    theme_.setTheme(themes.at(idx));
  }
}

}  // namespace PJ
