#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BUILD_DIR="${SCRIPT_DIR}/build"
QT_DIR="${SCRIPT_DIR}/.qt/6.8.3/gcc_64"

if [[ ! -d "$QT_DIR" ]]; then
  echo "Qt 6.8.3 not found at ${QT_DIR}."
  echo "Install it with: aqt install-qt linux desktop 6.8.3 linux_gcc_64 --modules qtcharts qtwebsockets --outputdir ${SCRIPT_DIR}/.qt"
  exit 1
fi

CMAKE_CCACHE_ARGS=()
if command -v ccache &>/dev/null; then
  CMAKE_CCACHE_ARGS+=("-DCMAKE_C_COMPILER_LAUNCHER=ccache" "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache")
fi

conan install "$SCRIPT_DIR" --output-folder="$BUILD_DIR" --build=missing \
  -s build_type=RelWithDebInfo -s compiler.cppstd=20

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="${QT_DIR}" \
  "${CMAKE_CCACHE_ARGS[@]+"${CMAKE_CCACHE_ARGS[@]}"}"

cmake --build "$BUILD_DIR" -j "$(nproc)"
