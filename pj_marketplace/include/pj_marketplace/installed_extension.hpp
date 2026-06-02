#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDateTime>
#include <QString>

namespace PJ {

// Installed extension discovered from an embedded plugin manifest on disk.
struct InstalledExtension {
  QString id;  ///< Matches Extension::id from the registry
  QString version;
  QDateTime install_date;
  QString path;  ///< Absolute path to ~/.plotjuggler/extensions/<id>/
  bool enabled = true;
};

}  // namespace PJ
