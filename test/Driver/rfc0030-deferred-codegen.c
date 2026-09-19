// RFC 0030, section 10.5: weavec-cc holds every code generator callback
// until the WeaveC analysis has run, then replays them in order. With no
// rewrites the object is byte-identical to the reference Clang's at -O2,
// -O0 -g and -O2 -flto=thin: gate G7 (scripts/codegen-identity.py) in
// miniature. Both compilers get the same empty sysroot, because -g records
// it and this file includes nothing. The constructs below are the ones whose
// code generation depends on what was parsed before them.
//
// RUN: rm -rf %t && mkdir -p %t/sdk
// RUN: %weavec_cc -isysroot %t/sdk -O2 -c %s -o %t/unit.o
// RUN: mv %t/unit.o %t/weavec.o
// RUN: %clang -isysroot %t/sdk -O2 -c %s -o %t/unit.o -isystem %resource_dir -D__WEAVEC__=1
// RUN: cmp %t/unit.o %t/weavec.o
// RUN: %weavec_cc -isysroot %t/sdk -O0 -g -c %s -o %t/unit.o
// RUN: mv %t/unit.o %t/weavec.o
// RUN: %clang -isysroot %t/sdk -O0 -g -c %s -o %t/unit.o -isystem %resource_dir -D__WEAVEC__=1
// RUN: cmp %t/unit.o %t/weavec.o
// RUN: %weavec_cc -isysroot %t/sdk -O2 -flto=thin -c %s -o %t/unit.o
// RUN: mv %t/unit.o %t/weavec.o
// RUN: %clang -isysroot %t/sdk -O2 -flto=thin -c %s -o %t/unit.o -isystem %resource_dir -D__WEAVEC__=1
// RUN: cmp %t/unit.o %t/weavec.o
//
// The analysis still runs before any code is emitted, and its error still
// drops the object.
// RUN: not %weavec_cc -isysroot %t/sdk -DBUG -O2 -c %s -o %t/bug.o 2>&1 | FileCheck --check-prefix=BUG %s
// RUN: not ls %t/bug.o
//
// Actions that emit no code keep the analysis beside Clang's own consumer.
// RUN: not %weavec_cc -isysroot %t/sdk -DBUG -fsyntax-only %s 2>&1 | FileCheck --check-prefix=BUG %s

// A struct used through a pointer before its definition (debug info).
struct node;
int is_set(const struct node *n);

// A static function used before it is defined, and one never used.
static int twice(int x);
int call_twice(int x) { return twice(x) + 1; }
static int twice(int x) { return 2 * x; }
static int unused_helper(int x) { return x; }

// Tentative definitions, one completed by a later definition.
int counter;
int table[];
int limit;
int limit = 64;

// A K&R definition after a declaration without a prototype.
int add();
int call_add(void) { return add(1, 2); }
int add(a, b) int a, b; { return a + b; }

// #pragma weak on a declared function.
void hook(void);
#pragma weak hook
void run_hook(void) {
  if (hook)
    hook();
}

struct node {
  int value;
  struct node *next;
};
int is_set(const struct node *n) { return n && n->value != 0; }

// A static inline function and a jump table.
static inline int clamp(int x) { return x < 0 ? 0 : x > limit ? limit : x; }
int classify(int x) {
  switch (clamp(x)) {
  case 0:
    return 10;
  case 1:
    return 20;
  case 2:
    return 35;
  case 3:
    return 47;
  default:
    return counter + table[0];
  }
}

#ifdef BUG
void *malloc(unsigned long);
void free(void *);
int bug(void) {
  int *p = malloc(sizeof *p);
  if (!p)
    return 0;
  *p = 1;
  free(p);
  // BUG: rfc0030-deferred-codegen.c:[[@LINE+1]]:11: error: use of 'p' after it was freed [weavec::use-after-free]
  return *p;
}
#endif
