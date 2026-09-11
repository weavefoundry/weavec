// RUN: split-file %s %t
// RUN: cp %t/good.h %t/config.h
// RUN: %weavec --whole-program --checked-function=main --analysis-cache=%t/cache --analysis-stats=%t/cold.json --checked-report=%t/cold-report.json %t/hooks.c %t/main.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --whole-program --checked-function=main --analysis-cache=%t/cache --analysis-stats=%t/warm.json --checked-report=%t/warm-report.json %t/hooks.c %t/main.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: FileCheck %s --check-prefix=WARM --implicit-check-not='"function_analyses":' --implicit-check-not='"cache_write_failures":' < %t/warm.json
// RUN: diff %t/cold-report.json %t/warm-report.json
// RUN: cp %t/short.h %t/config.h
// RUN: not %weavec --whole-program --checked-function=main --analysis-cache=%t/cache %t/hooks.c %t/main.c -- 2>&1 | FileCheck %s --check-prefix=SHORT
// RUN: cp %t/view.h %t/config.h
// RUN: not %weavec --whole-program --checked-function=main --analysis-cache=%t/cache %t/hooks.c %t/main.c -- 2>&1 | FileCheck %s --check-prefix=VIEW
// RFC 0022: private hook bindings survive checkpoints. Header changes to a
// callback body and recovered object view invalidate dependent checked callers.
// CLEAN-NOT: error:
// WARM: "cache_hits":2
// SHORT: access interval must fit its object
// SHORT: error: checked safety requirements were not established
// VIEW: pointer recovery has an incompatible object type or alignment
// VIEW: error: checked safety requirements were not established

//--- good.h
#define CAP(n) (n)
#define VIEW int
//--- short.h
#define CAP(n) 1
#define VIEW int
//--- view.h
#define CAP(n) (n)
#define VIEW float
//--- hooks.c
#include <stdlib.h>
#include "config.h"
static void *custom(size_t n) { return malloc(CAP(n)); }
static void *(*allocate)(size_t) = malloc;
void configure(void) { allocate = custom; }
void *acquire(size_t n) { return allocate(n); }
int read_value(void *p) { return *(VIEW *)p; }
//--- main.c
#include <stdlib.h>
void configure(void);
void *acquire(size_t);
int read_value(void *);
int main(void) {
  configure();
  char *p = acquire(8);
  if (!p) return 0;
  p[7] = 7;
  free(p);
  int value = 7;
  return read_value(&value);
}
