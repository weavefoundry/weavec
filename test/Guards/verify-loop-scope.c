// RFC 0035 §7: a guard the rules removed stays a monitor in verify mode,
// inside its local's scope: no pass hoists it above the lifetime start of a
// local declared in a loop, so a correct program reports nothing.
// RUN: %weavec_cc -O2 -fweavec-checks=verify %s -o %t
// RUN: %t 2>&1 | FileCheck %s --allow-empty
// CHECK-NOT: weavec
typedef union { void *p; long long i; double n; } Value;
typedef struct { Value value; unsigned char tag; } TValue;

static inline void reverse(TValue *from, TValue *to) {
  for (; from < to; from++, to--) {
    TValue temp;
    TValue *a = &temp;
    a->value = from->value;
    a->tag = from->tag;
    from->value = to->value;
    from->tag = to->tag;
    to->value = a->value;
    to->tag = a->tag;
  }
}

__attribute__((noinline)) void rotate(TValue *p, TValue *t, int n) {
  reverse(p, t - n);
}

int main(int argc, char **argv) {
  TValue s[3];
  (void)argv;
  for (int i = 0; i < 3; i++) {
    s[i].value.i = i + 1;
    s[i].tag = 3;
  }
  rotate(s, s + 2, argc);
  return s[0].value.i == 2 ? 0 : 1;
}
