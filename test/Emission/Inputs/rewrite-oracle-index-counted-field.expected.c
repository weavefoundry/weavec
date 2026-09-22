/* rewrite-oracle-index-counted-field.c as the check emitter rewrites it. */
#include <weavec.h>
/* The field annotation records the source's name and line. */
#line 10 ORACLE_FILE
struct buf { char *WEAVEC_COUNTED_BY(cap) data; unsigned long cap; };
char at(const struct buf *b, unsigned long i) {
  return ((char *)__weavec_chk_nonnull(((const struct buf *)__weavec_chk_nonnull(b))->data))[__weavec_chk_index(i, (*(const struct buf *)__weavec_chk_nonnull(b)).cap)];
}
