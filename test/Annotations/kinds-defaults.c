// RFC 0030 §7.3, parameters and results: a parameter of a `static` function
// whose address is never taken gets the join of its arguments at the direct
// calls (Single-valid ones give `single`, nonnull when every one is); every
// other parameter is `single nullable` by A1, marked `relies-on-single` when
// a use of its value may rest on that default. `argv` of `main` is
// `counted(argc + 1) nonnull` at level 4 while `argc` and `argv` are never
// assigned. A result is the join over the unit's `return`s, or the A3
// default for a function the unit only declares.
// RUN: %weavec --dump-kinds %s -- | FileCheck %s

struct point { int x, y; };

// CHECK: param norm 0 'p': single nonnull [inferred, lower-bound]
static int norm(struct point *p) { return p->x * p->x + p->y * p->y; }
// CHECK: param shift 0 'p': unknown nullable [inferred]
// CHECK: demoted at {{[0-9]+}}:24: pointer arithmetic
static int shift(int *p) { return p[0]; }
// No direct call: the A1 default.
// CHECK: param unused 0 'p': single nullable [default, lower-bound] relies-on-single
static int unused(int *p) { return *p; }
// The address is taken: the A1 default.
// CHECK: param taken 0 'p': single nullable [default, lower-bound] relies-on-single
static int taken(int *p) { return *p; }

int calls(int *array, int (*out)(int *)) {
  struct point here = {1, 2};
  out = taken;
  return norm(&here) + shift(array + 1) + shift(array) + out(array);
}

// Comparisons, arithmetic and subscripts past 0 do not rest on the default.
// CHECK: param compare 0 'p': single nullable [default, lower-bound]{{$}}
int compare(const char *p, const char *q) { return p < q && p != 0 && p[1]; }

// CHECK: result pick: single nonnull [inferred, lower-bound]
static struct point origin;
struct point *pick(void) { return &origin; }
// CHECK: result maybe: single nullable [inferred, lower-bound]
struct point *maybe(int c) { return c ? &origin : 0; }
// CHECK: result cursor: unknown nullable [inferred]
// CHECK-NEXT: demoted at [[@LINE+1]]:23: pointer arithmetic
int *cursor(int *p) { return p + 1; }
// CHECK: result external: single nullable [default, lower-bound]
struct point *external(void);
struct point *use_external(void) { return external(); }

// CHECK: param main 1 'argv': counted(param 0 scale 1 plus 1) nonnull [declared, declared, shape system-header, nullability system-header] argv
int main(int argc, char **argv) { return argv[argc - 1] != 0; }
