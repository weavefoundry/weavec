#!/bin/sh
# tmux's regression suite for scripts/corpus-gate.py (test/corpus/manifest.json, config tmux):
# every regress/*.sh against the built tmux (TEST_TMUX), each in turn, with a timeout, but those
# named below, which fail with the reference compiler too on the gate's machines. A script
# passes with status 0; any other status, a timeout, or a death by a signal (a trap) fails the
# test.
#
# Left out on Darwin, where they fail with the reference compiler too (darwin-arm64, RFC 0038's
# reference run): check-names and the scripts that compare a redrawn screen against a recorded
# one or drive an inner client (screen-redraw-*, copy-mode-redraw, tty-draw-line,
# format-animation).
set -u
tmux=$PWD/tmux
cd regress || exit 1
excluded=${TMUX_GATE_EXCLUDED:-}
if [ "$(uname -s)" = Darwin ]; then
  excluded="$excluded check-names copy-mode-redraw format-animation screen-redraw-fill-character
    screen-redraw-indicators screen-redraw-menus screen-redraw-popups screen-redraw-status
    screen-redraw-tiled screen-redraw-window-style tty-draw-line"
fi
# Left out on Linux: control-client-exit-stalled, whose server misses its 20-second exit
# window after the rest of the suite with the reference compiler too (linux-aarch64, RFC
# 0038's reference run; it passes alone).
if [ "$(uname -s)" = Linux ]; then
  excluded="$excluded control-client-exit-stalled"
fi
# One space between names, so that each matches below.
excluded=$(echo $excluded)
failed=""
run=0
for t in *.sh; do
  case " $excluded " in
    *" ${t%.sh} "*) echo "# left out: $t"; continue ;;
  esac
  run=$((run + 1))
  TEST_TMUX=$tmux perl -e 'alarm 120; exec @ARGV' sh "$t" > "/tmp/tmux-gate-$$.log" 2>&1
  status=$?
  if [ "$status" -ne 0 ]; then
    failed="$failed ${t%.sh}"
    echo "FAIL: ${t%.sh} (status $status)"
    tail -5 "/tmp/tmux-gate-$$.log"
  fi
done
rm -f "/tmp/tmux-gate-$$.log"
"$tmux" kill-server 2>/dev/null
echo "$run scripts run"
[ -z "$failed" ] || { echo "failed:$failed"; exit 1; }
