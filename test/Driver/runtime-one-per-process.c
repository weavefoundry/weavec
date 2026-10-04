// RFC 0033 §6.2: on Darwin one runtime serves every image of a process. A
// shared library built by weavec-cc releases the executable's blocks, the
// executable releases the library's, a guard in the library sees the
// executable's heap object (and so traps past its end), and the C
// library's own allocations (strdup) are in the arena.
//
// REQUIRES: host-darwin
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -dynamiclib -O1 %S/Inputs/runtime-one-per-process-lib.c -o %t/libone.dylib
// RUN: %weavec_cc -O1 %s -L%t -lone -Wl,-rpath,%t -o %t/prog
// RUN: %t/prog ok | FileCheck --check-prefix=OK %s
// RUN: not --crash %t/prog past
// RUN: mkdir -p %t/report
// RUN: %weavec_cc -dynamiclib -O1 -fweavec-checks=report %S/Inputs/runtime-one-per-process-lib.c -o %t/report/libone.dylib
// RUN: %weavec_cc -O1 -fweavec-checks=report %s -L%t/report -lone -Wl,-rpath,%t/report -o %t/report/prog
// RUN: %t/report/prog past 2>&1 | FileCheck --check-prefix=PAST %s
// RUN: env WEAVEC_RT_STATS=1 %t/prog ok 2>&1 | FileCheck --check-prefix=STATS %s
//
// OK: shared: tracked tracked
// PAST: weavec: runtime check failed: object
// One line of statistics: the owner's.
// STATS-COUNT-1: weavec: runtime: {{[0-9]+}} allocations

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *lib_copy(const char *s);
void lib_release(char *p);
int lib_read(const char *p, long i);
/* Fails a 100-byte access exactly when the block is tracked (and smaller). */
extern int __weavec_rt_object(const void *, long long, unsigned long long,
                              unsigned long long, unsigned long long);

int main(int argc, char **argv) {
  char *mine = malloc(8);
  char *theirs = lib_copy("from the library");
  char *system = strdup("from libc");
  if (mine == NULL || theirs == NULL || system == NULL || argc < 2)
    return 2;
  memset(mine, 'm', 8);
  if (strcmp(argv[1], "past") == 0)
    return lib_read(mine, 8) == 'm';
  printf("shared: %s %s\n",
         __weavec_rt_object(theirs, 0, 0, 0, 100) ? "tracked" : "untracked",
         __weavec_rt_object(system, 0, 0, 0, 100) ? "tracked" : "untracked");
  lib_release(mine);
  free(theirs);
  free(system);
  return lib_read("x", 0) == 'x' ? 0 : 1;
}
