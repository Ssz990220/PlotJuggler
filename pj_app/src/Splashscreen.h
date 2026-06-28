// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QPixmap>
#include <QString>

namespace PJ {

// Preferences::splash_mode storage key and its two values, shared between the
// Preferences dialog (which writes them) and the startup path (which reads
// them) so the strings can't drift out of sync.
inline const QString kSplashModeKey = QStringLiteral("Preferences::splash_mode");
inline const QString kSplashModeMemes = QStringLiteral("memes");
inline const QString kSplashModeSerious = QStringLiteral("serious");

// Builds the startup splash pixmap honouring Preferences::splash_mode:
// "serious" → a composed branded banner (logo + wordmark + random subtitle),
// anything else → a random meme from the bundled pool. Returns a null QPixmap
// when no image is available (e.g. an empty meme pool).
QPixmap makeStartupSplash();

}  // namespace PJ
