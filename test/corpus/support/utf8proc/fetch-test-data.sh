#!/bin/sh
# The Unicode 18.0.0 conformance files utf8proc's normtest and graphemetest read
# (data/download.sh fetches the same versions; they are not tracked). Downloaded once into
# $CACHE (the corpus gate's <workdir>/.cache/utf8proc), checked against their SHA-256, and
# copied into the build copy's data/.
set -eu
CACHE=${CACHE:-${SRC:-.}/.gate-cache}
BASE=https://www.unicode.org/Public/18.0.0/ucd
mkdir -p "$CACHE" data
fetch() {
  name=$1 url=$2 sum=$3
  if [ ! -f "$CACHE/$name" ] || ! echo "$sum  $CACHE/$name" | shasum -a 256 -c - >/dev/null 2>&1; then
    curl -fsSL -o "$CACHE/$name.tmp" "$url"
    echo "$sum  $CACHE/$name.tmp" | shasum -a 256 -c - >/dev/null
    mv "$CACHE/$name.tmp" "$CACHE/$name"
  fi
  cp "$CACHE/$name" "data/$name"
}
fetch NormalizationTest.txt "$BASE/NormalizationTest.txt" \
  25a50d816764b04abfb4a646d3eb2b2a803284c3873d9a06757b94fe4513dde3
fetch GraphemeBreakTest.txt "$BASE/auxiliary/GraphemeBreakTest.txt" \
  b0cf047ee94485bbdc846de2b902f5f8a815f6b674f9d04223cddadd91c9df31
