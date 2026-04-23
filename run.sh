#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Disable the IBus platform input context: it's loaded from the system Qt
# install (often an older major version) and segfaults under Qt 6.8.
export QT_IM_MODULE=""

# Point Qt plugin discovery at the bundled Qt 6.8.3 only. If the user's shell
# has QT_PLUGIN_PATH set to a stale Qt (e.g. /home/.../qt/6.4.2/plugins), Qt
# scans it first, picks up the cert-only TLS backend there, then fails to load
# its OpenSSL sibling (symbol mismatch against the newer libstdc++) and all
# HTTPS traffic breaks — including the marketplace registry fetch.
export QT_PLUGIN_PATH="${SCRIPT_DIR}/.qt/6.8.3/gcc_64/plugins"

exec "${SCRIPT_DIR}/build/pj_app/pj_app" "$@"
