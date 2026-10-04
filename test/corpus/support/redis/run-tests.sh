#!/bin/sh
# Redis's Tcl test suite for scripts/corpus-gate.py (test/corpus/manifest.json,
# config redis): the units named as arguments, run by ./runtest with 4 test
# clients against src/redis-server.
#
# Every test server listens on a Unix socket under tests/tmp, and Darwin
# limits a socket path to 104 bytes: under the gate's build directory the
# path is longer, and the servers do not start. tests/tmp is therefore a
# symbolic link to a fresh directory under /tmp (tests/support/server.tcl
# normalizes the socket's path, which resolves the link), removed afterwards.
set -u
tmp=$(mktemp -d /tmp/weavec-redis.XXXXXX) || exit 1
trap 'rm -rf "$tmp"' EXIT
rm -rf tests/tmp && ln -s "$tmp" tests/tmp || exit 1
args=""
for unit in "$@"; do
  args="$args --single $unit"
done
# shellcheck disable=SC2086 # the units are words
./runtest --clients 4 $args
