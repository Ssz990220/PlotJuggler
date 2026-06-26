#!/usr/bin/env bash
#
# Build a relocatable PlotJuggler 4 AppImage.
#
# FIRST DRAFT. Bundles the pj_app shell, its Qt 6 + Conan runtime, and a fixed
# allow-list of plugins (see RELEASE_PLUGINS below) into a single self-contained
# AppImage. Unlike PJ3 (system Qt 5), PJ4 links Qt from ./.qt and most external
# deps from Conan, so linuxdeploy must be pointed at both — that is the bulk of
# the env setup here.
#
# Prerequisites (run these first, they are NOT done here):
#   ./install_qt6.sh                         # Qt into ./.qt/<ver>/gcc_64
#   ./build.sh                               # builds build/pj_app/pj_app + Conan env
#   the plugin .so files must already be built under plotjuggler_sdk/pj_ported_plugins/
#   (see that module's build.sh / rebuild_all_and_gather.sh)
#
# Usage:
#   appimage/build_appimage.sh            # -> PlotJuggler-4.0.0-dev-x86_64.AppImage
#
# KNOWN LIMITATION (first draft): the bundled plugin directory lives inside the
# read-only AppImage and is passed to pj_app via --plugin-dir, which is ALSO the
# directory the marketplace would install into. Marketplace install is therefore
# inert from within the AppImage. Making installs work means seeding the bundled
# plugins into the writable AppDataLocation on first run instead; deferred.
set -euo pipefail

ARCH="x86_64"
QT_VERSION="6.11.1"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD="${ROOT}/build"
QT_DIR="${ROOT}/.qt/${QT_VERSION}/gcc_64"
PLUGINS_BUILD="${ROOT}/plotjuggler_sdk/pj_ported_plugins"
APPDIR="${BUILD}/AppDir"
VERSION="4.0.0-dev"   # keep in sync with pj_app/src/main.cpp setApplicationVersion()

# --- Plugins that MUST ship in the release. Values are the built .so basenames.
#     ROS source/parser plugins are distro-agnostic proxies (no rclcpp link); the
#     per-distro inner .so files are bundled separately below.
declare -A RELEASE_PLUGINS=(
  [data_load_MCAP]=libmcap_source_plugin.so
  [data_load_CSV]=libcsv_source_plugin.so
  [data_load_parquet]=libparquet_source_plugin.so
  [data_stream_dummy]=libdummy_stream_plugin.so
  [data_stream_ros2]=libros2_stream_plugin.so
  [data_stream_foxglove_bridge]=libfoxglove_source_plugin.so
  [data_stream_pj_bridge]=libpj_bridge_source_plugin.so
  [parser_ros]=libparser_ros_plugin.so
  [parser_protobuf]=libparser_protobuf_plugin.so
  [parser_json]=libparser_json_plugin.so
  [toolbox_mosaico]=libtoolbox_mosaico_plugin.so
  [toolbox_quaternion]=libtoolbox_quaternion_plugin.so
  [toolbox_transform_editor]=libtoolbox_transform_editor_plugin.so
)
# Explicitly excluded by request (documented so a future "bundle everything"
# sweep does not silently pull them back in):
#   toolbox_colormap, toolbox_reactive_scripts_editor

# ---------------------------------------------------------------------------
# 0. Sanity checks
# ---------------------------------------------------------------------------
[[ -d "${QT_DIR}" ]]            || { echo "Qt not found at ${QT_DIR}. Run ./install_qt6.sh"; exit 1; }
[[ -x "${BUILD}/pj_app/pj_app" ]] || { echo "pj_app not built. Run ./build.sh"; exit 1; }
[[ -f "${BUILD}/conanrun.sh" ]] || { echo "build/conanrun.sh missing. Run ./build.sh"; exit 1; }
command -v wget >/dev/null     || { echo "wget required"; exit 1; }

# ---------------------------------------------------------------------------
# 1. linuxdeploy + its Qt plugin (themselves AppImages, fetched once)
# ---------------------------------------------------------------------------
cd "${SCRIPT_DIR}"
LD="linuxdeploy-${ARCH}.AppImage"
LDQT="linuxdeploy-plugin-qt-${ARCH}.AppImage"
[[ -f "${LD}" ]]   || wget -q "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/${LD}"
[[ -f "${LDQT}" ]] || wget -q "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/${LDQT}"
chmod +x "${LD}" "${LDQT}"

# ---------------------------------------------------------------------------
# 2. Assemble the AppDir skeleton
# ---------------------------------------------------------------------------
# App plugins live OUTSIDE usr/plugins on purpose: linuxdeploy-plugin-qt deploys
# Qt's own platform/imageformat plugins into usr/plugins, and pj_app's recursive
# .so scanner would otherwise try to dlopen those as PlotJuggler plugins.
PJ_PLUGINS_REL="usr/share/pj_app/plugins"
rm -rf "${APPDIR}"
mkdir -p "${APPDIR}/usr/bin" "${APPDIR}/${PJ_PLUGINS_REL}"
# pj_app itself is installed by linuxdeploy via --executable below (it copies the
# binary into usr/bin and deploys its Qt + Conan dependency closure).

# Icon: rasterize the app SVG (linuxdeploy wants a PNG named like the desktop Icon=).
ICON_PNG="${SCRIPT_DIR}/pj_app.png"
if [[ ! -f "${ICON_PNG}" ]]; then
  if command -v convert >/dev/null; then
    convert -background none -resize 256x256 "${ROOT}/resources/svg/plotjuggler.svg" "${ICON_PNG}"
  else
    echo "WARNING: 'convert' not found and ${ICON_PNG} missing — provide an icon manually."; exit 1
  fi
fi

# ---------------------------------------------------------------------------
# 3. Collect the allow-listed plugins (.so + sidecar manifest) into per-id
#    subdirs of usr/plugins. pj_app scans this tree recursively.
# ---------------------------------------------------------------------------
find_plugin_so() {  # echoes the first matching built .so path, or nothing
  find "${PLUGINS_BUILD}" -path '*/Release/bin/*' -name "$1" -print 2>/dev/null | head -1
}

PLUGIN_EXEC_ARGS=()
for name in "${!RELEASE_PLUGINS[@]}"; do
  so_name="${RELEASE_PLUGINS[$name]}"
  so_path="$(find_plugin_so "${so_name}")"
  if [[ -z "${so_path}" ]]; then
    echo "ERROR: required plugin '${name}' (${so_name}) not built — build it first."; exit 1
  fi
  dest="${APPDIR}/${PJ_PLUGINS_REL}/${name}"
  mkdir -p "${dest}"
  cp -v "${so_path}" "${dest}/"
  # Sidecar manifest sits next to the .so in the build tree.
  manifest="$(dirname "${so_path}")/$(basename "${so_name}" .so | sed 's/^lib//').pjmanifest.json"
  [[ -f "${manifest}" ]] && cp -v "${manifest}" "${dest}/"
  # --deploy-deps-only: pull each plugin's own NEEDED libs (mcap, arrow,
  # protobuf, …) into usr/lib but leave the .so where we placed it.
  PLUGIN_EXEC_ARGS+=(--deploy-deps-only "${dest}/${so_name}")
done

# ROS 2 per-distro inner libraries. The proxy resolves its inner via dladdr at
# the fixed path dist/<distro>/libros2_stream_plugin-<distro>.so relative to
# itself, so the layout is mandatory — a flat copy makes the proxy return a null
# vtable and the plugin never registers. Each inner resolves rclcpp against the
# user's sourced ROS install at runtime; non-matching distros simply fail to
# dlopen and are skipped by the per-file-tolerant scanner.
ROS2_DEST="${APPDIR}/${PJ_PLUGINS_REL}/data_stream_ros2"
while IFS= read -r inner; do
  [[ -z "${inner}" ]] && continue
  base="$(basename "${inner}")"                    # libros2_stream_plugin-humble.so
  distro="${base#libros2_stream_plugin-}"          # humble.so
  distro="${distro%.so}"                           # humble
  mkdir -p "${ROS2_DEST}/dist/${distro}"
  cp -v "${inner}" "${ROS2_DEST}/dist/${distro}/"
done < <(find "${PLUGINS_BUILD}" -name 'libros2_stream_plugin-*.so' -path '*/Release/bin/*' 2>/dev/null | sort -u)

# ---------------------------------------------------------------------------
# 4. Runtime env so linuxdeploy resolves Qt (from ./.qt) and Conan deps.
#    conanrun.sh exports LD_LIBRARY_PATH covering every Conan package this build
#    links (FFmpeg, cloudini, …); without it linuxdeploy drops those libs.
# ---------------------------------------------------------------------------
# shellcheck disable=SC1091
source "${BUILD}/conanrun.sh"
export QMAKE="${QT_DIR}/bin/qmake6"
export PATH="${QT_DIR}/bin:${PATH}"
export LD_LIBRARY_PATH="${QT_DIR}/lib:${LD_LIBRARY_PATH:-}"

# ---------------------------------------------------------------------------
# 5. Package
# ---------------------------------------------------------------------------
cd "${SCRIPT_DIR}"
OUTPUT="PlotJuggler-${VERSION}-${ARCH}.AppImage" \
  "./${LD}" \
    --appdir "${APPDIR}" \
    --executable "${BUILD}/pj_app/pj_app" \
    "${PLUGIN_EXEC_ARGS[@]}" \
    --desktop-file "${SCRIPT_DIR}/pj_app.desktop" \
    --icon-file "${ICON_PNG}" \
    --custom-apprun "${SCRIPT_DIR}/AppRun.sh" \
    --plugin qt \
    --output appimage

echo ""
echo "Done: ${SCRIPT_DIR}/PlotJuggler-${VERSION}-${ARCH}.AppImage"
