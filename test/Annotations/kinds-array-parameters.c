// RFC 0030 §7.2, rows 9 and 10: `T p[static N]` is `counted(N) nonnull`;
// a VLA parameter `T p[n]`, with `n` an earlier parameter, is `counted(n)
// nullable`. A constant `T p[N]` without `static` is no requirement (C gives
// it none and code commonly passes fewer elements): it only yields a fix-it
// suggestion that inserts `static`.
// RUN: %weavec --dump-kinds %s -- | FileCheck %s

// CHECK: param digest 0 'out': counted(32) nonnull [declared, declared, shape ecosystem, nullability ecosystem]
void digest(unsigned char out[static 32]);
// CHECK: param scale 1 'v': counted(param 0 scale 1 plus 0) nullable [declared, declared, shape ecosystem]
// CHECK: param scale 2 'w': counted(param 0 scale 1 plus 1) nullable [declared, declared, shape ecosystem]
void scale(int n, double v[n], double w[n + 1]);
// CHECK: param both 1 'm': counted(param 0 scale 1 plus 0) nonnull [declared, declared, shape ecosystem, nullability ecosystem]
void both(int n, double m[static n]);
// `[*]` names no count.
// CHECK-NOT: param star
void star(int n, double s[*]);

// CHECK-NOT: param loose 0
// CHECK: suggestion [[@LINE+1]]:18: 'k' is declared with 4 elements, which C does not require of callers; declare it 'k[static 4]' to require them (insert 'static ')
void loose(int k[4]);
// The suggestion goes on the definition, not on this prototype.
// CHECK-NOT: suggestion [[@LINE+1]]:
void later(int k[4]);
// CHECK: suggestion [[@LINE+1]]:18: 'k' is declared with 4 elements, which C does not require of callers; declare it 'k[static 4]' to require them (insert 'static ')
void later(int k[4]) { (void)k; }

void use(unsigned char *d, double *x, int *k) {
  digest(d);
  scale(2, x, x);
  both(2, x);
  loose(k);
}
