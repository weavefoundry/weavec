/* rewrite-oracle-index-declared.c as the check emitter rewrites it. */
#include <weavec.h>
/* The parameter annotation records the source's name and line. */
#line 8 ORACLE_FILE
char at(const char *WEAVEC_SIZED_BY(n) p, int n, int i) {
  return ((const char *)__weavec_chk_nonnull(p))[__weavec_chk_index(i, __weavec_have_s(n))];
}
