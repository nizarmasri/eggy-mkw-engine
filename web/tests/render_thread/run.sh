#!/usr/bin/env bash
# Builds the render thread test (CMakeLists.txt here: aurora with -pthread and render_thread_test.cpp)
# and runs it in headless Chromium (run_render_thread_test.mjs). No game data.
# usage: web/tests/render_thread/run.sh [build_dir] [log_file]
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
build="${1:-${TMPDIR:-/tmp}/mkw-render-thread-test}"
logfile="${2:-$build/render-thread-test.log}"
if ! command -v emcmake >/dev/null 2>&1; then
  # shellcheck disable=SC1091
  source "${EMSDK:-/opt/emsdk}/emsdk_env.sh" >/dev/null 2>&1
fi
emcmake cmake -S "$repo/web/tests/render_thread" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS=-pthread -DCMAKE_CXX_FLAGS=-pthread -DSDL_PTHREADS=ON
cmake --build "$build" --target render_thread_test
exec node "$repo/web/tests/render_thread/run_render_thread_test.mjs" "$build" "$logfile"
