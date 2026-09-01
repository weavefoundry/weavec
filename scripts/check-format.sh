#!/usr/bin/env bash
# Verifies that all C/C++ sources are clang-format clean.
# Usage: scripts/check-format.sh [--fix]
set -euo pipefail

cd "$(dirname "$0")/.."

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
  echo "error: $CLANG_FORMAT not found (set CLANG_FORMAT=...)" >&2
  exit 2
fi

list_files() {
  git ls-files -z --cached --others --exclude-standard \
    'include/**/*.h' \
    'lib/**/*.cpp' 'lib/**/*.h' \
    'tools/**/*.cpp' 'tools/**/*.h' \
    'unittests/**/*.cpp' 'unittests/**/*.h' \
    'resources/**/*.h'
}

if [ "${1:-}" = "--fix" ]; then
  list_files | xargs -0 "$CLANG_FORMAT" -i
  exit 0
fi

if list_files | xargs -0 "$CLANG_FORMAT" --dry-run -Werror; then
  echo "clang-format: OK"
else
  echo >&2
  echo "error: formatting issues found; run scripts/format.sh" >&2
  exit 1
fi
