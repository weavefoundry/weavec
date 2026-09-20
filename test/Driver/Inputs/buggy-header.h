/* Deliberately buggy inline definitions for headers-analysed.c. */
static inline void buggy(int *p) {
  free(p);
  free(p);
}
/* Referenced by nothing: never emitted, so never analysed. */
static inline void unused_buggy(int *p) {
  free(p);
  free(p);
}
