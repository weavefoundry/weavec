/* Deliberately buggy inline definition for headers-skipped.c. */
static inline void buggy(int *p) {
  free(p);
  free(p);
}
