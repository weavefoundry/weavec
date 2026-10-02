// RFC 0031 §4.9, *Summaries*: the fields of a member array's records.
// STAGE: S7
// `match` writes both fields of every element of `m->sub` (mujs's
// `Resub`). Positions are kept modulo their stride, so the second field of
// `sub[i]` is counted from the start of `sub[i + 1]` (`[1, 17) * 16 + 0`
// here); its store names `param1->sub[*].ep` over elements `[0, 16)`. Without
// the shift it was dropped (and `use` read `m.sub[0].ep` as uninitialised);
// and the caller's check measured the range's last element as a whole
// stride, a false `out-of-bounds` of 8 bytes past `m`.
// CLEAN
// ASAN
// RUN-INPUT:
struct Resub {
  int nsub;
  struct {
    const char *sp;
    const char *ep;
  } sub[16];
};

static int match(const char *s, struct Resub *m) {
  for (int i = 0; i < 16; ++i) {
    m->sub[i].sp = s;
    m->sub[i].ep = s + 1;
  }
  m->nsub = 1;
  return 0;
}

int main(void) {
  struct Resub m;
  const char *text = "ab";
  match(text, &m);
  return m.nsub == 1 && m.sub[0].ep - m.sub[0].sp == 1 && *m.sub[15].ep == 'b'
             ? 0
             : 1;
}
