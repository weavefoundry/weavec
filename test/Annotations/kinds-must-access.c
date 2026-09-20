// RFC 0030 §7.5, must-access requirements from the CFG and post-dominator
// tree: R1 `*p` gives `single nonnull`; R2 `p[i + k]` in a canonical counted
// loop gives `counted(e + k)` under `c < e`; R3 a pointer-range loop gives
// `ended-by(q)` under `p < q`; R4 a scan, or a string argument of a
// LibrarySpec row, gives `nul-terminated`; R5 an unguarded `p[e]` gives
// `counted(e + 1)`. A requirement of a static function whose address is not
// taken is enforced at its call sites; any other is the caller's contract.
// RUN: %weavec --dump-kinds %s -- | FileCheck %s

typedef unsigned long size_t;
size_t strlen(const char *);
void abort(void) __attribute__((noreturn));
void unknown(void);

// CHECK: param r1 0 'p': {{.*}}
// CHECK-NEXT: requires single nonnull (R1) at [[@LINE+1]]:31 [caller-contract]
int r1(const int *p) { return *p; }

// CHECK: param r2 0 'p': {{.*}}
// CHECK-NEXT: requires 0 < param 1 scale 1 plus 0 -> counted(param 1 scale 1 plus 1) nonnull (R2) at [[@LINE+3]]:5 [caller-contract]
void r2(int *p, int n) {
  for (int i = 0; i < n; i++)
    p[i + 1] = 0;
}

// CHECK: param r2_inclusive 0 'p': {{.*}}
// CHECK-NEXT: requires 2 <= param 1 scale 1 plus 0 -> counted(param 1 scale 1 plus 1) nonnull (R2) at [[@LINE+3]]:5 [caller-contract]
void r2_inclusive(int *p, int n) {
  for (int i = 2; i <= n; ++i)
    p[i] = 0;
}

// CHECK: param r3 0 'p': {{.*}}
// CHECK-NEXT: requires param 0 scale 1 plus 0 < param 1 scale 1 plus 0 -> ended-by(param 1) nonnull (R3) at [[@LINE+4]]:12 [caller-contract]
int r3(const int *p, const int *q) {
  int sum = 0;
  for (const int *x = p; x < q; ++x)
    sum += *x;
  return sum;
}

// CHECK: param r3_while 0 'p': {{.*}}
// CHECK-NEXT: requires param 0 scale 1 plus 0 < param 1 scale 1 plus 0 -> ended-by(param 1) nonnull (R3) at [[@LINE+3]]:5 [caller-contract]
void r3_while(char *p, char *q) {
  while (p < q) {
    *p = 0;
    p++;
  }
}

// CHECK: param r4_scan 0 's': {{.*}}
// CHECK-NEXT: requires nul-terminated nonnull (R4) at [[@LINE+3]]:10 [caller-contract]
size_t r4_scan(const char *s) {
  size_t n = 0;
  while (s[n])
    n++;
  return n;
}

// CHECK: param r4_library 0 's': {{.*}}
// CHECK-NEXT: requires nul-terminated nonnull (R4) at [[@LINE+1]]:43 [caller-contract]
size_t r4_library(const char *s) { return strlen(s) + 1; }

// CHECK: param r5 0 'p': {{.*}}
// CHECK-NEXT: requires counted(param 1 scale 1 plus 0) nonnull (R5) at [[@LINE+1]]:41 [caller-contract]
int r5(const int *p, size_t n) { return p[n - 1]; }

// Static and not address-taken: enforced at the calls.
// CHECK: param local 0 'p': {{.*}}
// CHECK-NEXT: requires counted(4) nonnull (R5) at [[@LINE+1]]:41 [call-sites]
static int local(const int *p) { return p[3]; }
int call_local(const int *p) { return local(p); }

// No requirement when something before the access may leave the function
// or the caller may pass null on purpose.
// CHECK: param tested 0 'p': {{.*}}
// CHECK-NOT: requires
int tested(const int *p) {
  if (!p)
    return 0;
  return *p;
}
// CHECK: param guarded 0 'p': {{.*}}
// CHECK-NOT: requires
int guarded(const int *p, int c) { return c ? *p : 0; }
// CHECK: param after_unknown 0 'p': {{.*}}
// CHECK-NOT: requires
int after_unknown(const int *p) {
  unknown();
  return *p;
}
// CHECK: param after_abort 0 'p': {{.*}}
// CHECK-NOT: requires
int after_abort(const int *p, int c) {
  if (c)
    abort();
  return *p;
}
// CHECK: param reassigned 0 'p': {{.*}}
// CHECK-NOT: requires
int reassigned(const int *p, const int *q) {
  p = q;
  return *p;
}
// A loop that may stop early is no canonical counted loop.
// CHECK: param early 0 'p': {{.*}}
// CHECK-NOT: requires
int early(const int *p, int n) {
  for (int i = 0; i < n; i++)
    if (p[i] == 0)
      break;
  return 0;
}

// A helper the unit defines that always returns keeps the access a
// must-access; one that may exit does not.
static void note(void) {}
static void fatal(void) { abort(); }
// CHECK: param after_note 0 'p': {{.*}}
// CHECK-NEXT: requires single nonnull (R1) at [[@LINE+3]]:10 [caller-contract]
int after_note(const int *p) {
  note();
  return *p;
}
// CHECK: param after_fatal 0 'p': {{.*}}
// CHECK-NOT: requires
int after_fatal(const int *p, int c) {
  if (c)
    fatal();
  return *p;
}
