#!/bin/sh
# LMDB's test programs for scripts/corpus-gate.py (test/corpus/manifest.json,
# config lmdb), run from the checkout's root after the build: mtest, mtest2,
# mtest3, mtest4 and mtest5, each against a fresh libraries/liblmdb/testdb
# (what `make test` does for mtest). After mtest, the tools over its
# database: mdb_stat, an mdb_dump / mdb_load round trip and mdb_copy (plain and
# compacting), each dumped and compared with the original dump's records.
# Everything is written under libraries/liblmdb. Exits non-zero on the first
# failure.
set -u
cd libraries/liblmdb || exit 1
fail() { echo "FAILED: $*"; exit 1; }
# A dump's records, without its header (which records the map address).
records() { sed '1,/^HEADER=END$/d' "$1"; }
for t in mtest mtest2 mtest3 mtest4 mtest5; do
  rm -rf testdb && mkdir testdb || exit 1
  echo "== $t"
  ./$t > "$t.out" 2>&1
  rc=$?
  if [ $rc -ne 0 ]; then
    tail -20 "$t.out"
    fail "$t (exit $rc)"
  fi
  if [ $t = mtest ]; then
    echo "== mdb_stat, mdb_dump, mdb_load, mdb_copy over mtest's database"
    ./mdb_stat testdb || fail "mdb_stat"
    ./mdb_stat -a -e -f testdb > /dev/null || fail "mdb_stat -a -e -f"
    ./mdb_dump testdb > testdb.dump || fail "mdb_dump"
    rm -rf testdb-load && mkdir testdb-load || exit 1
    ./mdb_load testdb-load < testdb.dump || fail "mdb_load"
    ./mdb_dump testdb-load > testdb-load.dump || fail "mdb_dump of the loaded database"
    records testdb.dump > testdb.rec && records testdb-load.dump | cmp testdb.rec - || fail "dump / load round trip differs"
    for c in "" -c; do
      rm -rf testdb-copy && mkdir testdb-copy || exit 1
      ./mdb_copy $c testdb testdb-copy || fail "mdb_copy $c"
      ./mdb_dump testdb-copy > testdb-copy.dump || fail "mdb_dump of the copy ($c)"
      records testdb-copy.dump | cmp testdb.rec - || fail "mdb_copy $c differs"
    done
    rm -rf testdb-load testdb-copy testdb.dump testdb-load.dump testdb-copy.dump testdb.rec
  fi
done
rm -rf testdb
echo "lmdb tests passed"
