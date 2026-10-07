// RFC 0035 §9: on Darwin one runtime serves every image of a process. A
// shared library built by weavec-cc releases the executable's blocks, the
// executable releases the library's, a guard in the library sees the
// executable's heap object (and so traps past its end), and the C
// library's own allocations (strdup) are in the arena, with their shadow.
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
// PAST: weavec: heap-buffer-overflow at {{.*}}runtime-one-per-process-lib.c:{{[0-9]+}}:{{[0-9]+}}: read of 1 bytes
// One line of statistics: the owner's.
// STATS-COUNT-1: weavec: runtime: {{[0-9]+}} allocations

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *lib_copy(const char *s);
void lib_release(char *p);
int lib_read(const char *p, long i);
/* Whether the n bytes at an address are addressable: not past the end of
 * a tracked block. */
extern int __weavec_rt_range_ok(unsigned long address, unsigned long long n);

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
         __weavec_rt_range_ok((unsigned long)theirs, 100) ? "untracked"
                                                          : "tracked",
         __weavec_rt_range_ok((unsigned long)system, 100) ? "untracked"
                                                          : "tracked");
  lib_release(mine);
  free(theirs);
  free(system);
  return lib_read("x", 0) == 'x' ? 0 : 1;
}
