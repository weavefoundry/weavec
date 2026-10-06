/* LMDB benchmark for scripts/corpus-gate.py (test/corpus/manifest.json,
 * config lmdb; RFC 0034, gate F7): three rounds of 300000 puts in one write
 * transaction, a cursor scan and 300000 gets, in the database directory
 * named by the argument (created if missing; the gate's command removes it
 * first). Deterministic; prints a checksum only. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <stdlib.h>
#include "lmdb.h"
#define N 300000
#define E(x) do { int rc_ = (x); if (rc_) { fprintf(stderr, "%s: %s\n", #x, mdb_strerror(rc_)); exit(1); } } while (0)
int main(int argc, char **argv) {
  MDB_env *env; MDB_txn *txn; MDB_dbi dbi; MDB_val k, v; MDB_cursor *cur;
  char kb[32], vb[64]; long sum = 0; const char *dir = argc > 1 ? argv[1] : "benchdb";
  mkdir(dir, 0755);
  E(mdb_env_create(&env)); E(mdb_env_set_mapsize(env, 1UL << 30));
  E(mdb_env_open(env, dir, MDB_NOSYNC, 0664));
  for (int round = 0; round < 3; round++) {
    E(mdb_txn_begin(env, NULL, 0, &txn)); E(mdb_dbi_open(txn, NULL, 0, &dbi));
    for (int i = 0; i < N; i++) {
      unsigned h = (unsigned)i * 2654435761u;
      k.mv_size = snprintf(kb, sizeof kb, "%08x%d", h, round); k.mv_data = kb;
      v.mv_size = snprintf(vb, sizeof vb, "value-%d-%d-padpadpadpad", i, round); v.mv_data = vb;
      E(mdb_put(txn, dbi, &k, &v, 0));
    }
    E(mdb_txn_commit(txn));
    E(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn)); E(mdb_cursor_open(txn, dbi, &cur));
    while (mdb_cursor_get(cur, &k, &v, MDB_NEXT) == 0) sum += ((char *)v.mv_data)[6] + (long)v.mv_size;
    for (int i = 0; i < N; i++) {
      unsigned h = (unsigned)i * 2654435761u;
      k.mv_size = snprintf(kb, sizeof kb, "%08x%d", h, round); k.mv_data = kb;
      E(mdb_get(txn, dbi, &k, &v)); sum += v.mv_size;
    }
    mdb_cursor_close(cur); mdb_txn_abort(txn);
  }
  mdb_env_close(env);
  printf("%ld\n", sum);
  return 0;
}
