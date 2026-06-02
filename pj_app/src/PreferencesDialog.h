#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <vector>

#include "pj_widgets/ChromeMetrics.h"
#include "pj_widgets/Dialog.h"

namespace Ui {
class PreferencesDialog;
}

namespace PJ {

class Theme;
class PreferencesNavRow;

class PreferencesDialog : public Dialog {
  Q_OBJECT
 public:
  explicit PreferencesDialog(Theme& theme, QWidget* parent = nullptr);
  ~PreferencesDialog() override;

 private:
  Ui::PreferencesDialog* ui_;
  Theme& theme_;
  // Owns nothing — Qt parentage owns the row widgets. This is just the
  // iteration target for selection updates.
  std::vector<PreferencesNavRow*> nav_rows_;
  // Snapshot taken at construction so Cancel can revert any live
  // preview the user triggered via the toggle / scrubbers.
  QString original_theme_;
  ChromeMetrics original_metrics_;
};

}  // namespace PJ
