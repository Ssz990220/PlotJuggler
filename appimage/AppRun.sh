#!/usr/bin/env bash
#
# Custom AppRun for the PlotJuggler 4 AppImage.
#
# Replaces linuxdeploy's generated launcher purely to reproduce the library/Qt
# environment the stock AppRun would set. It does NOT inject --plugin-dir: the
# app auto-discovers the bundled plugins at <prefix>/lib/plotjuggler/plugins
# (usr/bin/plotjuggler4 -> ../lib/plotjuggler/plugins), so --plugin-dir stays a
# user-facing option — anything the user passes is forwarded verbatim via "$@".
set -e

# $APPDIR is set by the AppImage runtime when mounted; fall back to this
# script's location for `--appimage-extract`ed / manual runs.
HERE="$(dirname "$(readlink -f "${0}")")"
APPDIR="${APPDIR:-${HERE}}"

export LD_LIBRARY_PATH="${APPDIR}/usr/lib:${LD_LIBRARY_PATH:-}"
export QT_PLUGIN_PATH="${APPDIR}/usr/plugins:${QT_PLUGIN_PATH:-}"
export XDG_DATA_DIRS="${APPDIR}/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"

# IBus input-context module loaded from a system/older Qt segfaults under the
# bundled Qt 6.11 runtime (same reason run.sh unsets it for dev runs).
unset QT_IM_MODULE

exec "${APPDIR}/usr/bin/plotjuggler4" "$@"
