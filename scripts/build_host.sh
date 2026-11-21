#!/usr/bin/env bash
# Build the host unit tests (Linux x86_64) with the system toolchain.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

build_dir="${BUILD_DIR:-build/host}"
cmake -S . -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build "$build_dir"
ctest --test-dir "$build_dir" --output-on-failure
