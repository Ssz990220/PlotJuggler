#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Disable the IBus platform input context: it's loaded from the system Qt
# install (often an older major version) and segfaults under Qt 6.8.
export QT_IM_MODULE=""

exec "${SCRIPT_DIR}/build/pj_app/pj_app" "$@"
