#!/usr/bin/env bash
# Runs clang-tidy over the project using an existing compile_commands.json.
# Usage: scripts/run-clang-tidy.sh [build-dir] [extra run-clang-tidy args...]
set -euo pipefail

cd "$(dirname "$0")/.."

build_dir="${1:-build/dev}"
shift || true

if [ ! -f "$build_dir/compile_commands.json" ]; then
  echo "error: $build_dir/compile_commands.json not found; configure first" >&2
  echo "       (e.g. cmake --preset dev)" >&2
  exit 2
fi

RUN_CLANG_TIDY="${RUN_CLANG_TIDY:-run-clang-tidy}"
if ! command -v "$RUN_CLANG_TIDY" >/dev/null 2>&1; then
  echo "error: $RUN_CLANG_TIDY not found (set RUN_CLANG_TIDY=...)" >&2
  exit 2
fi

# Only lint our own sources, not fetched third-party code.
exec "$RUN_CLANG_TIDY" -p "$build_dir" -quiet \
  "$(pwd)/(include|lib|tools|unittests)/.*" "$@"
