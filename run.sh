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
app_args=()
for arg in "$@"; do
  case "$arg" in
    --apitrace) use_apitrace=1 ;;
    *) app_args+=("$arg") ;;
  esac
done

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
