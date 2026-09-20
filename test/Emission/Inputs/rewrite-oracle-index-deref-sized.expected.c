/* rewrite-oracle-index-deref-sized.c as the check emitter rewrites it. */
#include <weavec.h>
/* The parameter annotation records the source's name and line. */
#line 8 ORACLE_FILE
char head(const char *WEAVEC_SIZED_BY(n) p, int n) {
  return *(__weavec_chk_index(0, __weavec_have_s(n)), (const char *)__weavec_chk_nonnull(p));
}
