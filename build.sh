#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# aqtinstall lays Qt out under a host-specific subdir: linux uses gcc_64, macOS
# uses macos (arch-neutral universal), windows uses msvc2022_64. Pick by uname so
# build.sh works on the unofficial macOS build as well as Linux CI.
case "$(uname -s)" in
  Darwin) QT_HOST_DIR="macos" ;;
  *)      QT_HOST_DIR="gcc_64" ;;
esac
QT_DIR="${SCRIPT_DIR}/.qt/6.11.1/${QT_HOST_DIR}"

# nproc is coreutils-only; macOS provides the count via sysctl.
nproc() { command nproc 2>/dev/null || sysctl -n hw.ncpu; }

# `./build.sh --tsan` builds + runs the Qt-free foundation concurrency tests under
# ThreadSanitizer in a separate build-tsan/ tree (the default build/ is untouched).
# It guards the datastore worker-thread race regressions; the Linux CI `tsan` job
# runs the same target set. TSan is wired via -DPJ_ENABLE_TSAN=ON on a normal
# RelWithDebInfo configure, so the Conan dependency closure is reused as-is (no
# Debug rebuild) and only our own sources are instrumented.
TSAN=0
for arg in "$@"; do
  case "$arg" in
    --tsan) TSAN=1 ;;
    *) echo "unknown argument: $arg (supported: --tsan)" >&2; exit 2 ;;
  esac
done

if [[ ! -d "$QT_DIR" ]]; then
  echo "Qt 6.11.1 not found at ${QT_DIR}."
  echo "Install it with: ./install_qt6.sh"
  exit 1
fi

CMAKE_CCACHE_ARGS=()
if command -v ccache &>/dev/null; then
  CMAKE_CCACHE_ARGS+=("-DCMAKE_C_COMPILER_LAUNCHER=ccache" "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache")
fi

# Foundation concurrency tests exercised under ThreadSanitizer. Keep in sync with
# the `tsan` job in .github/workflows/linux-ci.yml.
TSAN_TESTS=(engine_thread_safety_test engine_concurrency_test)

if [[ "$TSAN" == "1" ]]; then
  BUILD_DIR="${SCRIPT_DIR}/build-tsan"

  conan install "$SCRIPT_DIR" --output-folder="$BUILD_DIR" --build=missing \
    -s build_type=RelWithDebInfo -s compiler.cppstd=20 -r conancenter

  # PJ4_BUILD_APP=OFF + building only the foundation test targets keeps Qt out of
  # the picture entirely (no Qt code is compiled), even though configure still
  # finds Qt for the modules it won't build.
  cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/conan_toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_PREFIX_PATH="${QT_DIR}" \
    -DPJ_ENABLE_TSAN=ON \
    -DPJ4_BUILD_APP=OFF \
    "${CMAKE_CCACHE_ARGS[@]+"${CMAKE_CCACHE_ARGS[@]}"}"

  cmake --build "$BUILD_DIR" --target "${TSAN_TESTS[@]}" -j "$(nproc)"

  # Run under ctest so the per-test CMake TIMEOUT catches a deadlock regression,
  # and so TSan's non-zero exit (it dies on the first report under halt_on_error)
  # fails the test. ^(...)$ restricts the run to the targets we actually built.
  filter="$(IFS='|'; echo "${TSAN_TESTS[*]}")"
  TSAN_OPTIONS="halt_on_error=1 history_size=4 ${TSAN_OPTIONS:-}" \
    ctest --test-dir "$BUILD_DIR" -R "^(${filter})$" --output-on-failure --timeout 120
  exit 0
fi

BUILD_DIR="${SCRIPT_DIR}/build"

# Pin resolution to conancenter. A developer machine may have private org remotes
# (e.g. an Artifactory) listed ahead of conancenter that host forked recipes under
# a user channel — those would shadow the stock recipes and drag a whole `@<org>`
# dependency subtree into the graph. conancenter carries every PJ4 dependency.
conan install "$SCRIPT_DIR" --output-folder="$BUILD_DIR" --build=missing \
  -s build_type=RelWithDebInfo -s compiler.cppstd=20 -r conancenter

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="${QT_DIR}" \
  "${CMAKE_CCACHE_ARGS[@]+"${CMAKE_CCACHE_ARGS[@]}"}"

cmake --build "$BUILD_DIR" -j "$(nproc)"
