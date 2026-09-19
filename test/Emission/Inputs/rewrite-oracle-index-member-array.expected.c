/* rewrite-oracle-index-member-array.c as the check emitter rewrites it. */
struct buf { char data[16]; int len; };
char at(struct buf *b, int i) {
  return ((struct buf *)__weavec_chk_nonnull(b))->data[__weavec_chk_index(i, 16)];
}
