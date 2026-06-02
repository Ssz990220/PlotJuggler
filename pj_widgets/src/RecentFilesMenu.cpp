// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/RecentFilesMenu.h"

#include <QAction>
#include <QMenu>
#include <QSettings>
#include <utility>

namespace PJ {

RecentFilesMenu::RecentFilesMenu(QString settings_key, int max_entries, QWidget* parent)
    : QObject(parent),
      settings_key_(std::move(settings_key)),
      max_entries_(max_entries),
      files_(QSettings().value(settings_key_).toStringList()),
      menu_(new QMenu(parent)),
      clear_action_(new QAction(tr("Clear Recent"), this)) {
  connect(clear_action_, &QAction::triggered, this, &RecentFilesMenu::clear);
  rebuildMenu();
}

void RecentFilesMenu::record(const QString& path) {
  if (!files_.isEmpty() && files_.first() == path) {
    return;
  }
  const bool was_empty = files_.isEmpty();
  files_.removeAll(path);
  files_.prepend(path);
  while (files_.size() > max_entries_) {
    files_.removeLast();
  }
  rebuildMenu();
  persist();
  if (was_empty) {
    emit enabledChanged(true);
  }
}

void RecentFilesMenu::clear() {
  if (files_.isEmpty()) {
    return;
  }
  files_.clear();
  rebuildMenu();
  persist();
  emit enabledChanged(false);
}

void RecentFilesMenu::popupAt(QPoint global_pos) {
  if (!menu_->isEmpty()) {
    menu_->exec(global_pos);
  }
}

bool RecentFilesMenu::isEmpty() const {
  return files_.isEmpty();
}

void RecentFilesMenu::rebuildMenu() {
  menu_->clear();
  for (const QString& path : files_) {
    QAction* action = menu_->addAction(path);
    connect(action, &QAction::triggered, this, [this, path]() { emit activated(path); });
  }
  if (!files_.isEmpty()) {
    menu_->addSeparator();
    menu_->addAction(clear_action_);
  }
}

void RecentFilesMenu::persist() const {
  QSettings().setValue(settings_key_, files_);
}

}  // namespace PJ
