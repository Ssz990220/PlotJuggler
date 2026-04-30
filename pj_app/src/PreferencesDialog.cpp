#include "PreferencesDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>

#include "pj_app_core/Theme.h"
#include "ui_PreferencesDialog.h"

namespace PJ {

namespace {

// Maps an internal theme name (the persisted key) to a user-visible
// label. Centralised here so the dialog's combo population is
// decoupled from any ordering inside Theme::availableThemes().
QString displayNameFor(const QString& theme_name) {
  if (theme_name == QLatin1String("light")) {
    return PreferencesDialog::tr("Light Theme");
  }
  if (theme_name == QLatin1String("dark")) {
    return PreferencesDialog::tr("Dark Theme");
  }
  return theme_name;
}

}  // namespace

PreferencesDialog::PreferencesDialog(Theme& theme, QWidget* parent)
    : QDialog(parent), ui_(new Ui::PreferencesDialog), theme_(theme) {
  ui_->setupUi(this);

  for (const QString& name : Theme::availableThemes()) {
    ui_->comboBoxTheme->addItem(displayNameFor(name), name);
  }
  const int idx = ui_->comboBoxTheme->findData(theme_.currentTheme());
  ui_->comboBoxTheme->setCurrentIndex(idx >= 0 ? idx : 0);

  connect(this, &QDialog::accepted, this, &PreferencesDialog::onAccepted);
}

PreferencesDialog::~PreferencesDialog() {
  delete ui_;
}

void PreferencesDialog::onAccepted() {
  const QString name = ui_->comboBoxTheme->currentData().toString();
  if (!name.isEmpty()) {
    theme_.setTheme(name);
  }
}

}  // namespace PJ
