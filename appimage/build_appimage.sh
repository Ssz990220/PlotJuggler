#!/usr/bin/env bash
#
# Build a relocatable PlotJuggler 4 AppImage.
#
# Bundles the plotjuggler4 shell and its Qt 6 + Conan runtime into a single
# self-contained AppImage. Unlike PJ3 (system Qt 5), PJ4 links Qt from ./.qt and
# most external deps from Conan, so linuxdeploy must be pointed at both — that is
# the bulk of the env setup here.
#
# Prerequisites (run these first, they are NOT done here):
#   ./install_qt6.sh      # Qt into ./.qt/<ver>/gcc_64
#   ./build.sh            # builds build/pj_app/plotjuggler4 + Conan env
#
# Plugins are NOT part of this repo — they are built and published separately by
# pj-official-plugins (per-extension marketplace zips) and indexed by the
# pj-plugin-registry. There are two ways to bundle them (and a no-plugin default):
#
#   appimage/build_appimage.sh                       # app-only AppImage
#   appimage/build_appimage.sh --plugins-dir <path>  # LOCAL: copy a folder you
#                                                    #   curated, verbatim, into
#                                                    #   usr/lib/plotjuggler/plugins
#   appimage/build_appimage.sh --plugins-registry [url]
#                                                    # REGISTRY: download the
#                                                    #   curated set (BUNDLE_IDS)
#                                                    #   from the plugin registry,
#                                                    #   verify checksums, unpack
#
# Bundled plugins land at usr/lib/plotjuggler/plugins. The app never scans that
# dir directly — at startup it seeds its contents into the writable per-user
# extensions dir (copying new ids, refreshing ones whose bundled version is
# newer) and loads everything from there, so marketplace installs/uninstalls
# work normally and the read-only bundle stays a seed source.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
source "${ROOT}/versions.env"

ARCH="${PJ_APPIMAGE_ARCH}"
QT_VERSION="${PJ_QT_VERSION}"
PLATFORM="linux-${ARCH}"   # registry artifact key (registry.json platforms.<key>)

BUILD="${ROOT}/build"
QT_DIR="${ROOT}/.qt/${QT_VERSION}/gcc_64"
APPDIR="${BUILD}/AppDir"
VERSION="${PJ_VERSION:-${PJ_APP_VERSION}}"

# Bundled plugins go where the installed app expects its seed source: <prefix>/
# lib/plotjuggler/plugins, which plotjuggler4 resolves relative to itself
# (usr/bin -> ../lib/plotjuggler/plugins). Kept OUT of usr/plugins on purpose —
# linuxdeploy-plugin-qt deploys Qt's own platform/imageformat plugins there, and
# the seed's plugin scan must never try to dlopen those as PlotJuggler plugins.
PJ_PLUGINS_REL="usr/lib/plotjuggler/plugins"

# The plugin registry to resolve --plugins-registry against. Tracks main (latest
# published plugin versions); override with the optional --plugins-registry <url>
# argument to pin a specific ref.
REGISTRY_URL="https://raw.githubusercontent.com/PlotJuggler/pj-plugin-registry/main/registry.json"

# Curated set of registry extension ids bundled by --plugins-registry. The
# registry lists every official extension; this is the subset that ships in the
# AppImage. Excluded by request: toolbox-colormap, toolbox-reactive-scripts-editor.
# Not yet bundle-able (absent from the registry): toolbox-mosaico,
# toolbox-transform-editor.
BUNDLE_IDS=(
  csv-loader
  mcap-loader
  parquet-loader
  dummy-streamer
  foxglove-bridge
  plotjuggler-bridge
  ros-parser
  protobuf-parser
  json-parser
  toolbox-quaternion
  # ros2-stream  # <-- multi-distro ROS 2 subscriber. Uncomment once it is
  #                 published to a release + indexed in pj-plugin-registry (see
  #                 "Follow-up: multi-distro ROS 2" in README.md). The single
  #                 linux-x86_64 zip carries the distro-agnostic proxy + per-distro
  #                 inners under dist/<distro>/; registry-mode unpacks it verbatim,
  #                 so no special handling is needed here once the entry exists.
)

PLUGINS_MODE="none"        # none | local | registry
PLUGINS_LOCAL_DIR=""

usage() {
  sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --plugins-dir)
      PLUGINS_MODE="local"; PLUGINS_LOCAL_DIR="${2:?--plugins-dir needs a path}"; shift 2 ;;
    --plugins-registry)
      PLUGINS_MODE="registry"
      # Optional URL argument (anything not starting with '-').
      if [[ -n "${2:-}" && "${2}" != -* ]]; then REGISTRY_URL="$2"; shift; fi
      shift ;;
    -h | --help)
      usage; exit 0 ;;
    *)
      echo "build_appimage: unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

# ---------------------------------------------------------------------------
# 0. Sanity checks
# ---------------------------------------------------------------------------
[[ -d "${QT_DIR}" ]]                    || { echo "Qt not found at ${QT_DIR}. Run ./install_qt6.sh"; exit 1; }
[[ -x "${BUILD}/pj_app/plotjuggler4" ]] || { echo "plotjuggler4 not built. Run ./build.sh"; exit 1; }
[[ -f "${BUILD}/conanrun.sh" ]]         || { echo "build/conanrun.sh missing. Run ./build.sh"; exit 1; }
command -v wget >/dev/null              || { echo "wget required"; exit 1; }

# ---------------------------------------------------------------------------
# 1. linuxdeploy + its Qt plugin (themselves AppImages, fetched once)
# ---------------------------------------------------------------------------
cd "${SCRIPT_DIR}"
LD="linuxdeploy-${ARCH}.AppImage"
LDQT="linuxdeploy-plugin-qt-${ARCH}.AppImage"
# appimagetool packages the finished AppDir into the AppImage. We invoke it
# directly (instead of linuxdeploy's --output appimage) so plugins can be copied
# in AFTER linuxdeploy has deployed the app's dependency closure — see step 5.
AT="appimagetool-${ARCH}.AppImage"
[[ -f "${LD}" ]]   || wget -q "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/${LD}"
[[ -f "${LDQT}" ]] || wget -q "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/${LDQT}"
[[ -f "${AT}" ]]   || wget -q "https://github.com/AppImage/appimagetool/releases/download/continuous/${AT}"
chmod +x "${LD}" "${LDQT}" "${AT}"

# ---------------------------------------------------------------------------
# 2. Assemble the AppDir skeleton
# ---------------------------------------------------------------------------
rm -rf "${APPDIR}"
mkdir -p "${APPDIR}/usr/bin" "${APPDIR}/${PJ_PLUGINS_REL}"
# plotjuggler4 itself is installed by linuxdeploy via --executable below (it copies
# the binary into usr/bin and deploys its Qt + Conan dependency closure).

# Icon: committed 256x256 raster (resources/svg/plotjuggler4.png), rendered once
# from resources/svg/plotjuggler.svg with the pinned Qt6Svg. No host rasterizer
# (ImageMagick/rsvg/inkscape) is required at build time.
ICON_PNG="${ROOT}/resources/svg/plotjuggler4.png"
[[ -f "${ICON_PNG}" ]] || { echo "ERROR: committed icon ${ICON_PNG} is missing"; exit 1; }

# ---------------------------------------------------------------------------
# 3. Plugin-bundling helpers (per the selected mode), copying into per-id subdirs
#    of ${PJ_PLUGINS_REL}. The app scans this tree recursively. Published plugins
#    are self-contained (heavy deps static-linked), so no dependency deployment
#    is needed — a plain copy/unpack is the install. These run AFTER linuxdeploy
#    (step 5), never before: linuxdeploy would otherwise try to resolve every
#    bundled .so's dependency closure and abort on a plugin whose deps are
#    intentionally external — e.g. the ROS 2 subscriber's per-distro inner under
#    dist/<distro>/, which binds to the user's *sourced* system ROS and must NOT
#    be self-contained.
# ---------------------------------------------------------------------------
collect_plugins_local() {
  [[ -d "${PLUGINS_LOCAL_DIR}" ]] || { echo "ERROR: --plugins-dir '${PLUGINS_LOCAL_DIR}' is not a directory"; exit 1; }
  echo "Plugins: copying local folder '${PLUGINS_LOCAL_DIR}' -> ${PJ_PLUGINS_REL}"
  cp -a "${PLUGINS_LOCAL_DIR}/." "${APPDIR}/${PJ_PLUGINS_REL}/"
}

collect_plugins_registry() {
  command -v python3   >/dev/null || { echo "python3 required for --plugins-registry"; exit 1; }
  command -v unzip     >/dev/null || { echo "unzip required for --plugins-registry"; exit 1; }
  command -v sha256sum >/dev/null || { echo "sha256sum required for --plugins-registry"; exit 1; }

  local registry_json; registry_json="$(mktemp)"
  echo "Plugins: fetching registry ${REGISTRY_URL}"
  wget -qO "${registry_json}" "${REGISTRY_URL}" || { echo "ERROR: failed to download registry"; exit 1; }

  local id url checksum want got zip dest
  for id in "${BUNDLE_IDS[@]}"; do
    # Resolve this extension's url + checksum for our platform from the registry.
    read -r url checksum < <(python3 - "${registry_json}" "${id}" "${PLATFORM}" <<'PY'
import json, sys
reg, want_id, plat = sys.argv[1], sys.argv[2], sys.argv[3]
data = json.load(open(reg))
for ext in data.get("extensions", []):
    if ext.get("id") == want_id:
        p = ext.get("platforms", {}).get(plat)
        if p:
            print(p.get("url", ""), p.get("checksum", ""))
        break
PY
)
    [[ -n "${url}" ]] || { echo "ERROR: registry has no '${id}' for ${PLATFORM}"; exit 1; }

    zip="$(mktemp --suffix=.zip)"
    echo "  ${id}: ${url}"
    wget -qO "${zip}" "${url}" || { echo "ERROR: download failed for '${id}'"; exit 1; }

    want="${checksum#sha256:}"   # registry value is 'sha256:<hex>'
    if [[ -n "${want}" ]]; then
      got="$(sha256sum "${zip}" | awk '{print $1}')"
      [[ "${got}" == "${want}" ]] || { echo "ERROR: checksum mismatch for '${id}' (want ${want}, got ${got})"; exit 1; }
    fi

    dest="${APPDIR}/${PJ_PLUGINS_REL}/${id}"
    mkdir -p "${dest}"
    unzip -oq "${zip}" -d "${dest}"
    rm -f "${zip}"
  done
  rm -f "${registry_json}"
  echo "Plugins: bundled ${#BUNDLE_IDS[@]} extension(s) from the registry for ${PLATFORM}"
}

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
# 5. Deploy, then bundle plugins, then package — in that order.
#
#    linuxdeploy populates the AppDir with the app binary plus its Qt + Conan
#    dependency closure (and installs the custom AppRun / desktop / icon), but is
#    NOT given --output appimage. Plugins are copied in only AFTER that deploy, so
#    linuxdeploy never walks their .so closures: self-contained plugins need no
#    deployment, and a plugin with intentionally-external deps (the ROS 2 inner)
#    would make linuxdeploy abort trying to resolve ROS libs that must come from
#    the user's sourced system ROS at runtime. appimagetool then packages the
#    finished AppDir verbatim without re-scanning dependencies.
# ---------------------------------------------------------------------------
cd "${SCRIPT_DIR}"
"./${LD}" \
  --appdir "${APPDIR}" \
  --executable "${BUILD}/pj_app/plotjuggler4" \
  --desktop-file "${SCRIPT_DIR}/plotjuggler4.desktop" \
  --icon-file "${ICON_PNG}" \
  --custom-apprun "${SCRIPT_DIR}/AppRun.sh" \
  --plugin qt

case "${PLUGINS_MODE}" in
  local)    collect_plugins_local ;;
  registry) collect_plugins_registry ;;
  none)     echo "Plugins: none (app-only AppImage; pass --plugins-dir or --plugins-registry to bundle)" ;;
esac

# ---------------------------------------------------------------------------
# 5b. Bundle the embedded CPython stdlib for the Python Data Processor backend.
#     The app bakes PYTHONHOME to the BUILD host's Conan cpython path
#     (PJ_PYTHON_HOME), which does not exist on any other machine, so CPython
#     fails to find its stdlib (encodings, …) and Python filters die with
#     "Failed to init CPython". AppRun overrides PYTHONHOME to <AppDir>/usr, so
#     ship the stdlib at usr/lib/pythonX.Y. pj_scripting/CMakeLists.txt records
#     the prefix in build/pj_python_home.txt.
# ---------------------------------------------------------------------------
PY_HOME_FILE="${BUILD}/pj_python_home.txt"
if [[ -f "${PY_HOME_FILE}" ]]; then
  py_prefix="$(head -n1 "${PY_HOME_FILE}")"
  py_stdlib="$(ls -d "${py_prefix}"/lib/python3.* 2>/dev/null | head -n1)"
  if [[ -n "${py_stdlib}" && -d "${py_stdlib}" ]]; then
    py_ver="$(basename "${py_stdlib}")"   # e.g. python3.12
    echo "Python: bundling stdlib ${py_stdlib} -> usr/lib/${py_ver}"
    mkdir -p "${APPDIR}/usr/lib"
    cp -a "${py_stdlib}" "${APPDIR}/usr/lib/${py_ver}"
  else
    echo "WARNING: Python stdlib not found under '${py_prefix}/lib/python3.*' — Python Data Processors will fail at runtime" >&2
  fi
else
  echo "WARNING: ${PY_HOME_FILE} missing — cannot bundle Python stdlib (Python Data Processors will fail)" >&2
fi

OUTPUT="${SCRIPT_DIR}/PlotJuggler-${VERSION}-${ARCH}.AppImage"
ARCH="${ARCH}" "./${AT}" "${APPDIR}" "${OUTPUT}"

echo ""
echo "Done: ${OUTPUT}"
