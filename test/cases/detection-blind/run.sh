#!/bin/sh
# Builds every NN_name.c in this directory twice, as the bug and as its fixed
# twin (-DFIX), runs both with a 10-second limit, and prints one line each:
#
#   NN_name bug:<status> fix:<status>
#
# where <status> is the exit status, SIG<NAME> if a signal ended the program
# (SIGALRM means it hit the time limit), or build-failed.
#
#   CC      compiler command (default: clang)
#   CFLAGS  extra flags for both builds (default: none)
#   OUT     directory for binaries and logs (default: a fresh temp directory)
#
# An optional argument selects the programs whose name contains it, e.g.
#   CC=clang CFLAGS='-O1 -fsanitize=address' ./run.sh 0
# Programs that include <pthread.h> are built with -pthread.

CC=${CC:-clang}
CFLAGS=${CFLAGS:-}
dir=$(cd "$(dirname "$0")" && pwd)
out=${OUT:-$(mktemp -d "${TMPDIR:-/tmp}/detection-blind.XXXXXX")}
mkdir -p "$out" || exit 1
filter=${1:-}

# run BINARY: runs it with a 10-second alarm and prints its status.
run() {
    perl -e 'alarm 10; exec @ARGV or exit 127' "$1" >"$1.log" 2>&1
    rc=$?
    if [ "$rc" -gt 128 ]; then
        name=$(kill -l $((rc - 128)) 2>/dev/null) || name=$((rc - 128))
        echo "SIG$name"
    else
        echo "$rc"
    fi
}

# build SRC BIN [FLAGS...]: compiles SRC into BIN, then runs it.
build_and_run() {
    src=$1
    bin=$2
    shift 2
    # $CC and $CFLAGS are split into words on purpose.
    if $CC -std=c11 $CFLAGS "$@" "$src" -o "$bin" >"$bin.build.log" 2>&1; then
        run "$bin"
    else
        echo build-failed
    fi
}

for src in "$dir"/[0-9][0-9]_*.c; do
    name=$(basename "$src" .c)
    case $name in
    *"$filter"*) ;;
    *) continue ;;
    esac
    threads=
    if grep -q '<pthread.h>' "$src"; then
        threads=-pthread
    fi
    bug=$(build_and_run "$src" "$out/$name.bug" $threads)
    fix=$(build_and_run "$src" "$out/$name.fix" -DFIX $threads)
    echo "$name bug:$bug fix:$fix"
done
echo "binaries and logs: $out" >&2
