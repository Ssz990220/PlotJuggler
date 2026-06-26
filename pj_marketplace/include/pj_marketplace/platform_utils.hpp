#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <string>

namespace PJ {

// Static helpers for platform detection and standard directory resolution.
//
// All path helpers return absolute paths without a trailing separator.
// Directories are NOT created here — callers are responsible for mkpath.
class PlatformUtils {
 public:
  // Returns the platform identifier used as key in registry artifact maps.
  // Format: "<os>-<arch>", e.g. "linux-x86_64", "windows-x86_64", "macos-arm64".
  static QString currentPlatform();

  // Returns true on Windows builds.
  static bool isWindows();

  // Returns the shared library extension for the current platform:
  //   Linux:   ".so"
  //   Windows: ".dll"
  //   macOS:   ".dylib"
  static std::string pluginExtension();

  // Root of all PlotJuggler user data (QStandardPaths::AppDataLocation, i.e. the
  // PlotJuggler/PlotJuggler4 org/app pair):
  //   Linux:   ~/.local/share/PlotJuggler/PlotJuggler4/
  //   Windows: %LOCALAPPDATA%/PlotJuggler/PlotJuggler4/
  //   macOS:   ~/Library/Application Support/PlotJuggler/PlotJuggler4/
  static QString configDir();

  // <config-root>/extensions/ — active, loaded extensions.
  static QString extensionsDir();

  // <config-root>/.extension_staging/ — restart staging for Windows updates.
  static QString pendingDir();

  // <config-root>/.backup/ — pre-update backups (F-12, deferred to April+).
  static QString backupDir();
};

}  // namespace PJ
