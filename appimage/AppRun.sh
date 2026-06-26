#!/usr/bin/env bash
#
# Custom AppRun for the PlotJuggler 4 AppImage.
#
# Replaces linuxdeploy's generated launcher so we can inject --plugin-dir,
# pointing pj_app at the plugins baked into the image. The library/Qt
# environment that the stock AppRun would set is reproduced here.
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

# Bundled plugins. The dir is read-only inside the mounted image, so the
# marketplace cannot install here — see KNOWN LIMITATION in build_appimage.sh.
PJ_PLUGINS="${APPDIR}/usr/share/pj_app/plugins"

# Don't double up if the user passes their own --plugin-dir.
for arg in "$@"; do
  case "${arg}" in
    --plugin-dir|--plugin-dir=*) exec "${APPDIR}/usr/bin/pj_app" "$@" ;;
  esac
done

exec "${APPDIR}/usr/bin/pj_app" --plugin-dir "${PJ_PLUGINS}" "$@"
