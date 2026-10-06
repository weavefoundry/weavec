#!/bin/sh
# Runs GIFLIB's regression suite (tests/makefile) for the corpus gate, from
# the checkout root, after the library and the utilities are built.
#
# It is tests/makefile's `test` target (every *-regress target's TAP output,
# a plan line, ./tapview) with one change: giffix-regress uses GNU head's
# `--bytes=-20`, which BSD head on macOS rejects, so the same check (the
# last 20 bytes of pic/treescap.gif cut off, repaired by giffix, dumped by
# gifbuild and compared with tests/giffixed.ico) is run here with a
# portable `head -c`. Exits non-zero when any test fails.
set -u
cd tests || exit 1
targets="render-regress gif2rgb-regress gifbuild-regress gifclrmp-regress gifecho-regress \
giffilter-regress gifinto-regress gifsponge-regress giftext-regress giftool-regress gifwedge-regress"
log=gate-tap.log
{
	# shellcheck disable=SC2086
	make --quiet $targets || echo "not ok - make $targets"
	n=$(wc -c <../pic/treescap.gif)
	head -c $((n - 20)) <../pic/treescap.gif | ../giffix 2>/dev/null | ../gifbuild -d |
		./tapdiffer "giffix: Testing giffix behavior" giffixed.ico
} >"$log"
count=$(grep -c '^\(not \)\{0,1\}ok' "$log")
[ "$count" -gt 0 ] || { echo "no test ran" >&2; exit 1; }
{ cat "$log"; echo "1..$count"; } | ./tapview
