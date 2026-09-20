/* A header for the precompiled-header fallback test (RFC 0030, 10.9). */
struct item {
  int value;
  struct item *next;
};

static inline int value_of(const struct item *item) { return item->value; }
