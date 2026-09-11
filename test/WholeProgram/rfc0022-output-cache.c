// RUN: split-file %s %t
// RUN: %weavec --whole-program --checked-function=main --analysis-cache=%t/cache --checked-report=%t/cold.json %t/helper.c %t/main.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --whole-program --checked-function=main --analysis-cache=%t/cache --analysis-stats=%t/stats.json --checked-report=%t/warm.json %t/helper.c %t/main.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: FileCheck %s --check-prefix=WARM --implicit-check-not='"function_analyses":' < %t/stats.json
// RUN: diff %t/cold.json %t/warm.json
// RFC 0022: final-output null conditions survive persistent reuse verbatim.
// CLEAN-NOT: error:
// WARM: "cache_hits":2

//--- helper.c
#include <stdlib.h>
void create(char **out) { *out = malloc(1); if (*out) **out = 7; }
//--- main.c
#include <stdlib.h>
void create(char **);
int main(void) {
  char *p = 0;
  create(&p);
  if (!p) return 0;
  int result = *p;
  free(p);
  return result;
}
