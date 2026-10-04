#!/bin/sh
# Mbed TLS 3.6's `framework` git submodule (Mbed-TLS/mbedtls-framework), which
# its CMake build needs (the test helpers under framework/tests and the
# scripts that generate the test suites) and the gate's checkout leaves empty.
# Fetched once, at the commit the pinned mbedtls commit records for it, into
# $CACHE (the corpus gate's <workdir>/.cache/mbedtls), and copied into the
# build copy's framework/ without its .git.
set -eu
CACHE=${CACHE:-${SRC:-.}/.gate-cache}
URL=https://github.com/Mbed-TLS/mbedtls-framework.git
SHA=2a3e2c5ea053c14b745dbdf41f609b1edc6a72fa
dest="$CACHE/framework-$SHA"
if [ "$(git -C "$dest" rev-parse HEAD 2>/dev/null || true)" != "$SHA" ]; then
  rm -rf "$dest" "$dest.tmp"
  mkdir -p "$dest.tmp"
  git -C "$dest.tmp" init --quiet
  git -C "$dest.tmp" remote add origin "$URL"
  git -C "$dest.tmp" fetch --quiet --depth 1 origin "$SHA"
  git -C "$dest.tmp" checkout --quiet --detach FETCH_HEAD
  [ "$(git -C "$dest.tmp" rev-parse HEAD)" = "$SHA" ]
  mv "$dest.tmp" "$dest"
fi
recorded=$(git ls-tree HEAD framework | awk '{print $3}')
[ "$recorded" = "$SHA" ] || { echo "framework: the checkout records $recorded, this script fetches $SHA"; exit 1; }
rm -rf framework
mkdir framework
(cd "$dest" && tar cf - --exclude .git .) | (cd framework && tar xf -)
