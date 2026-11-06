// RFC 0038 §1: one runtime serves every image of a process, whatever the
// images' symbol tables and link order. A shared library built by
// weavec-cc releases the executable's blocks, the executable releases the
// library's, a guard in the library sees the executable's heap object (and
// so stops past its end), and the C library's own allocations (strdup) are
// in the arena, with their shadow. That holds for a library that exports
// only its own symbols (a version script, an export list), one linked with
// -Bsymbolic, and an executable that names the library before the runtime;
// a guarded plug-in in a guarded program stops its own overflow too.
//
// RUN: rm -rf %t && mkdir -p %t/plain %t/hidden %t/symbolic
// RUN: %weavec_cc %shared -O1 %S/Inputs/shared-images-lib.c -o %t/plain/libone%so
// RUN: %weavec_cc %shared -O1 %S/Inputs/shared-images-lib.c -o %t/hidden/libone%so %if host-darwin %{ -Wl,-exported_symbols_list,%S/Inputs/shared-images.darwin-exports %} %else %{ -Wl,--version-script=%S/Inputs/shared-images.map %}
// RUN: %weavec_cc -O1 %s -L%t/plain -lone -Wl,-rpath,%t/plain -o %t/plain/prog
// RUN: %weavec_cc -O1 %s -L%t/hidden -lone -Wl,-rpath,%t/hidden -o %t/hidden/prog
// RUN: %weavec_cc -O1 %t/hidden/libone%so %s -Wl,-rpath,%t/hidden -o %t/hidden/first
// RUN: %t/plain/prog ok | FileCheck --check-prefix=OK %s
// RUN: %t/hidden/prog ok | FileCheck --check-prefix=OK %s
// RUN: %t/hidden/first ok | FileCheck --check-prefix=OK %s
// RUN: not --crash %t/plain/prog past 2>&1 | FileCheck --check-prefix=PAST %s
// RUN: not --crash %t/hidden/prog past 2>&1 | FileCheck --check-prefix=PAST %s
// RUN: not --crash %t/hidden/first past 2>&1 | FileCheck --check-prefix=PAST %s
// RUN: %if host-linux %{ %weavec_cc -shared -fPIC -O1 -Wl,-Bsymbolic %S/Inputs/shared-images-lib.c -o %t/symbolic/libone.so %}
// RUN: %if host-linux %{ %weavec_cc -O1 %s -L%t/symbolic -lone -Wl,-rpath,%t/symbolic -o %t/symbolic/prog %}
// RUN: %if host-linux %{ %t/symbolic/prog ok | FileCheck --check-prefix=OK %s %}
// RUN: %if host-linux %{ not --crash %t/symbolic/prog past 2>&1 | FileCheck --check-prefix=PAST %s %}
//
// One line of statistics: the process's one runtime.
// RUN: env WEAVEC_RT_STATS=1 %t/plain/prog ok 2>&1 | FileCheck --check-prefix=STATS %s
//
// A guarded plug-in, in a guarded host and in one weavec-cc did not link.
// On Linux the unguarded host's allocator is the C library's, which the
// runtime says once, and preloading the runtime checks the heap; on Darwin
// the runtime's zone serves the process from the moment it loads.
// RUN: %weavec_cc -O1 %S/Inputs/shared-images-host.c -o %t/host
// RUN: %clang -O1 %S/Inputs/shared-images-host.c -o %t/unguarded %if host-linux %{ -ldl %}
// RUN: %t/host %t/hidden/libone%so 7 | FileCheck --check-prefix=READ %s
// RUN: not --crash %t/host %t/hidden/libone%so 8 2>&1 | FileCheck --check-prefix=PLUGIN %s
// RUN: %if host-darwin %{ not --crash %t/unguarded %t/hidden/libone%so 8 2>&1 | FileCheck --check-prefix=PLUGIN %s %}
// RUN: %if host-linux %{ %t/unguarded %t/hidden/libone%so 8 2>&1 | FileCheck --check-prefix=NOTE %s %}
// RUN: %if host-linux %{ env WEAVEC_RT_NOTES=0 %t/unguarded %t/hidden/libone%so 8 2>&1 | FileCheck --check-prefix=QUIET %s %}
// RUN: %if host-linux %{ env LD_PRELOAD=%weavec_rt not --crash %t/unguarded %t/hidden/libone%so 8 2>&1 | FileCheck --check-prefix=PLUGIN %s %}
//
// A second runtime (another copy, as another WeaveC version's would be)
// stops the program.
// RUN: mkdir -p %t/copy && cp %weavec_rt %t/copy/
// RUN: not %t/host runtime %t/copy/libweavec_rt%so 2>&1 | FileCheck --check-prefix=TWO %s
//
// OK: shared: tracked tracked
// PAST: weavec: heap-buffer-overflow at {{.*}}shared-images-lib.c:{{[0-9]+}}:{{[0-9]+}}: read of 1 bytes
// STATS-COUNT-1: weavec: runtime: {{[0-9]+}} allocations
// READ: read: 120
// PLUGIN: weavec: heap-buffer-overflow at {{.*}}shared-images-lib.c:{{[0-9]+}}:{{[0-9]+}}: read of 1 bytes
// NOTE: weavec: note: the program's allocator is not WeaveC's ({{.*}}libc.so.6 serves it: the program was not linked by weavec-cc): heap accesses are not checked; preload {{.*}}libweavec_rt.so{{.*}} to check them
// NOTE: read:
// QUIET-NOT: weavec: note
// TWO: weavec: error: two WeaveC runtimes in one process ({{.*}}libweavec_rt{{.*}} and {{.*}}copy{{/|\\\\}}libweavec_rt{{.*}}): build every image with one weavec-cc

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
