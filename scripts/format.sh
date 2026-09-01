#!/usr/bin/env bash
# Formats all C/C++ sources and CMake files in place.
set -euo pipefail

cd "$(dirname "$0")/.."

scripts/check-format.sh --fix

if command -v cmake-format >/dev/null 2>&1; then
  scripts/check-cmake-format.sh --fix
else
  echo "note: cmake-format not installed; skipping CMake files (pipx install cmakelang && pipx inject cmakelang pyyaml)"
fi
