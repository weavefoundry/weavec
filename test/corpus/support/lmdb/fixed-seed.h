/* Forced into LMDB's mtest programs (-include) by scripts/corpus-gate.py
 * (test/corpus/manifest.json, config lmdb): mtest, mtest2, mtest3 and mtest5
 * seed rand() with time(NULL), which makes their key counts and values
 * differ from run to run. The gate's tests must be deterministic, so the
 * seed is fixed. The headers are included first so that the macro does not
 * rename their declaration of srand. */
#include <stdlib.h>
#include <time.h>
#define srand(seed) srand(1)
