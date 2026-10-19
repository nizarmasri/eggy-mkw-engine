#!/usr/bin/env bash
# Builds web/tests/baton (aurora, SDL and the rest compiled from source with -pthread, as the
# baton build does) and runs it in headless Chromium with JSPI removed. No game data involved.
# A long compile: CI runs it (.github/workflows/ios26.yml), not the owner's Mac.
# usage: web/tests/baton/run_baton_test.sh [build_dir] [logfile]
# env:   PLAYWRIGHT_MODULE  Playwright's index.mjs (default: import 'playwright')
#        JOBS               compile parallelism (default 4)
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="${1:-${TMPDIR:-/tmp}/mkw-baton-test-build}"
log="${2:-$out/baton-test.log}"
mkdir -p "$out" "$(dirname "$log")"
if ! command -v emcmake >/dev/null 2>&1; then
  # shellcheck disable=SC1091
  source "${EMSDK:-/opt/emsdk}/emsdk_env.sh" >/dev/null 2>&1
fi
emcmake cmake -S "$repo/web/tests/baton" -B "$out" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS=-pthread -DCMAKE_CXX_FLAGS=-pthread -DSDL_PTHREADS=ON
cmake --build "$out" --target baton_test --parallel "${JOBS:-4}"
node_bin="${MKW_RUNNER_NODE:-$(command -v node)}"
exec "$node_bin" "$repo/web/tests/baton/run_baton_test.mjs" "$out" "$log"
