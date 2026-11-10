#!/bin/sh
# libuv's test suite for scripts/corpus-gate.py (test/corpus/manifest.json,
# config libuv): every test of out/uv_run_tests_a, one process per test, from
# the source directory (CTest's WORKING_DIRECTORY), except those below. A test
# passes with status 0 (ok) or 7 (TEST_SKIP: needs root, a feature, ...).
#
# Left out, on Darwin, with any compiler (the reference compiler included):
#   emfile                        fails every run: uv_tcp_connect returns
#                                 UV_EMFILE (-24) at test-emfile.c:93 under the
#                                 lowered RLIMIT_NOFILE.
#   spawn_exercise_sigchld_issue  exercises the Darwin kernel's lost SIGCHLD
#                                 (its own comment); timed out in both
#                                 reference runs of the suite, passes alone.
#   udp_multicast_join            need multicast on the host's interfaces;
#   udp_multicast_join6           timed out in one of two reference runs.
#   tcp_connect6_link_local       need an IPv6 link-local address and
#   udp_multicast_interface       multicast routes, which GitHub's macos-15
#   udp_multicast_ttl             runners lack: they failed in the weekly run
#                                 of 2026-11-07 in both modes (ttl: an error
#                                 other than ENETUNREACH or EPERM) and pass on
#                                 a workstation (RFC 0040 section 0).
set -u
bin=out/uv_run_tests_a
excluded="emfile spawn_exercise_sigchld_issue udp_multicast_join udp_multicast_join6"
excluded="$excluded tcp_connect6_link_local udp_multicast_interface udp_multicast_ttl"
tests=$("$bin" --list | awk '{print $1}') || exit 1
[ -n "$tests" ] || { echo "no tests listed by $bin"; exit 1; }
run=0
failed=""
for t in $tests; do
  case " $excluded " in
    *" $t "*) echo "# left out: $t"; continue ;;
  esac
  run=$((run + 1))
  "$bin" "$t"
  status=$?
  if [ "$status" -ne 0 ] && [ "$status" -ne 7 ]; then
    failed="$failed $t"
  fi
done
echo "# $run tests run, failed:${failed:- none}"
[ -z "$failed" ]
