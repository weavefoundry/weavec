#!/bin/sh
# The corpus gate's test of hiredis (test/corpus/manifest.json, config hiredis): the project's
# test.sh, which starts a redis-server (it must be on PATH) on a free port and runs
# hiredis-test against it. On Darwin two connection-error tests fail with any compiler at the
# pinned commit: after a refused non-blocking connect, the kernel answers
# setsockopt(TCP_NODELAY) with EINVAL, and hiredis reports that instead of "Connection
# refused" ("Returns error when the port is not open", "We don't clobber connection exception
# with setsockopt error"). Exactly those two are tolerated there; any other failure, and any
# death by a signal (a trap), fails the test.
set -u
log=hiredis-test.gate.log
REDIS_PORT=${REDIS_PORT:-$((50000 + $$ % 1000))} sh ./test.sh > "$log" 2>&1
status=$?
cat "$log"
if [ "$status" -ne 1 ] || [ "$(uname -s)" != Darwin ]; then
  exit "$status"
fi
others=$(grep 'FAILED' "$log" | grep -v 'TESTS FAILED' \
  | grep -v -e 'Returns error when the port is not open' \
            -e "We don't clobber connection exception with setsockopt error")
count=$(sed -n 's/.*\*\*\* \([0-9][0-9]*\) TESTS* FAILED.*/\1/p' "$log")
if [ -z "$others" ] && [ "$count" = 2 ]; then
  echo "corpus gate: only the two known Darwin connection-error failures"
  exit 0
fi
exit 1
