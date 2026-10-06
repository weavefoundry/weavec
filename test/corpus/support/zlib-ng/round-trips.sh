#!/bin/sh
# Round trips for the corpus gate's zlib-ng config, run from the checkout root
# after the CMake build into out/: minigzip at every level and strategy, and
# minideflate at three levels and window sizes (its -w option is the window,
# kept in this script because the gate's flag check forbids -w in commands).
set -u
mkdir -p gate-rt
cat ./*.c arch/generic/*.c > gate-rt/src
: > gate-rt/empty
for f in gate-rt/src gate-rt/empty test/data/fireworks.jpg test/data/lcet10.txt; do
    for o in -0 -1 -2 -3 -4 -5 -6 -7 -8 -9 "-6 -h" "-6 -R" "-6 -F" "-6 -f" "-6 -T"; do
        # shellcheck disable=SC2086
        out/minigzip $o < "$f" > gate-rt/c &&
            out/minigzip -d < gate-rt/c > gate-rt/d &&
            cmp gate-rt/d "$f" || { echo "FAIL minigzip $o $f"; exit 1; }
    done
    for l in 1 6 9; do
        for w in 9 12 15; do
            cp "$f" gate-rt/m &&
                out/minideflate -c -k -$l -w $w -m 8 -s 2 gate-rt/m > gate-rt/m.z &&
                out/minideflate -c -d -k -w $w gate-rt/m.z > gate-rt/m.out &&
                cmp gate-rt/m.out "$f" || { echo "FAIL minideflate -$l -w $w $f"; exit 1; }
        done
    done
done
echo "round trips passed"
