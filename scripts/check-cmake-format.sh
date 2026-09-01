#!/usr/bin/env bash
# Verifies that CMake files are cmake-format clean.
# Usage: scripts/check-cmake-format.sh [--fix]
set -euo pipefail

cd "$(dirname "$0")/.."

if ! command -v cmake-format >/dev/null 2>&1; then
  echo "error: cmake-format not found (pipx install cmakelang && pipx inject cmakelang pyyaml)" >&2
  exit 2
fi

list_files() {
  git ls-files -z --cached --others --exclude-standard \
    'CMakeLists.txt' '**/CMakeLists.txt' 'cmake/*.cmake'
}

if [ "${1:-}" = "--fix" ]; then
  list_files | xargs -0 cmake-format -i
  exit 0
fi

if list_files | xargs -0 cmake-format --check; then
  echo "cmake-format: OK"
else
  echo >&2
  echo "error: CMake formatting issues found; run scripts/format.sh" >&2
  exit 1
fi
