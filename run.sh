#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Disable the IBus platform input context: it's loaded from the system Qt
# install (often an older major version) and segfaults under Qt 6.8.
export QT_IM_MODULE=""

# Native Wayland for ADS drag is patched in 3rdparty/Qt-Advanced-Docking/.
# Uncomment the next line to fall back to XWayland if a regression appears.
# export QT_QPA_PLATFORM=xcb

# Point Qt plugin discovery at the bundled Qt 6.8.3 only. If the user's shell
# has QT_PLUGIN_PATH set to a stale Qt (e.g. /home/.../qt/6.4.2/plugins), Qt
# scans it first, picks up the cert-only TLS backend there, then fails to load
# its OpenSSL sibling (symbol mismatch against the newer libstdc++) and all
# HTTPS traffic breaks — including the marketplace registry fetch.
export QT_PLUGIN_PATH="${SCRIPT_DIR}/.qt/6.8.3/gcc_64/plugins"

BIN="${SCRIPT_DIR}/build/pj_app/pj_app"

# --apitrace: launch under apitrace to capture a GL call trace (a smoke-test for
# redundant per-frame GL work — shader recompiles, full-cloud re-uploads, etc.).
# The flag is consumed here; all other arguments are forwarded to pj_app. If
# apitrace isn't installed we log and launch normally rather than failing.
#
#   ./run.sh --apitrace
#     → load a topic, interact briefly, quit. Keep it SHORT: apitrace records
#       buffer payloads, so traces grow fast.
#   Analyse (frames vs one-time GL setup that's wrongly per-frame):
#     t=./pj_app.trace
#     echo "frames:   $(apitrace dump "$t" | grep -c glXSwapBuffers)"
#     echo "compiles: $(apitrace dump "$t" | grep -c glCompileShader)"
#     echo "uploads:  $(apitrace dump "$t" | grep -c glBufferData)"
#   Healthy → compiles a small constant (≈ #programs) regardless of frames.
#   Env overrides: PJ_TRACE_API (gl|egl; try egl if the trace is empty),
#                  PJ_TRACE_OUT (output path; default ./pj_app.trace).
use_apitrace=0
user_set_plugin_dir=0
app_args=()
for arg in "$@"; do
  case "$arg" in
    --apitrace) use_apitrace=1 ;;
    --plugin-dir|--plugin-dir=*) user_set_plugin_dir=1; app_args+=("$arg") ;;
    *) app_args+=("$arg") ;;
  esac
done

# Default plugin discovery to the locally-built official plugins so a plain
# `./run.sh` can open MCAP/CSV/etc. without a Marketplace install. The app's
# built-in default (QStandardPaths AppDataLocation/extensions) is empty on a
# dev box, so without this you get "No DataSource plugin handles .mcap files".
# Build them with: (cd pj-official-plugins && ./build.sh). Overridable: pass
# your own --plugin-dir, or set PJ_PLUGIN_DIR, to take precedence.
DEFAULT_PLUGIN_DIR="${PJ_PLUGIN_DIR:-${SCRIPT_DIR}/pj-official-plugins/build/all/Release/bin}"
if [ "$user_set_plugin_dir" -eq 0 ]; then
  if [ -d "$DEFAULT_PLUGIN_DIR" ]; then
    app_args+=("--plugin-dir" "$DEFAULT_PLUGIN_DIR")
  else
    echo "run.sh: no plugin dir at ${DEFAULT_PLUGIN_DIR} — data-source plugins (MCAP, CSV, …) won't load." >&2
    echo "run.sh: build them with: (cd pj-official-plugins && ./build.sh)" >&2
  fi
fi

if [ "$use_apitrace" -eq 1 ]; then
  if command -v apitrace >/dev/null 2>&1; then
    out="${PJ_TRACE_OUT:-${SCRIPT_DIR}/pj_app.trace}"
    echo "run.sh: tracing GL with apitrace -> ${out}" >&2
    exec apitrace trace --api "${PJ_TRACE_API:-gl}" --output "${out}" \
         "${BIN}" ${app_args[@]+"${app_args[@]}"}
  fi
  echo "run.sh: --apitrace requested but 'apitrace' is not installed; launching normally." >&2
fi

exec "${BIN}" ${app_args[@]+"${app_args[@]}"}
