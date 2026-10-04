// The lookup record-call-member_ok.c calls: a record returned by value from
// another unit, so the caller's analysis takes its fields as unknown.
typedef struct { unsigned hashValue; const char *name; } hashed;
hashed lookup(const char *s, int n) {
  hashed h = {(unsigned)n, s};
  return h;
}
