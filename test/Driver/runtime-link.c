// RFC 0035 §9: a guarded link carries the runtime: the allocator's archive
// first, forced in by `-u malloc`, then the runtime. Without guards no
// runtime is on the line. A sanitizer that replaces the allocator, and a
// unit that defines one, turn the allocator off with a note.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %s -o %t/main.o
// RUN: %weavec_cc -### %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=LINK %s
// RUN: %weavec_cc -### -fweavec-checks=none %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=NONE %s
// RUN: %weavec_cc -### -fsanitize=address %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=ASAN %s
// RUN: %weavec_cc -### -nostdlib %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=NOSTDLIB %s
//
// RUN: %weavec_cc -c %S/Inputs/rfc0030-allocator.c -o %t/alloc.o
// RUN: %weavec_cc %t/alloc.o %t/main.o -o %t/own 2>&1 | FileCheck --check-prefix=OWN %s
// RUN: %t/own
//
// The allocator is what libweavec_alloc.a defines strongly (malloc, calloc, realloc, free):
// a program's own shim over those replaces a weak definition and keeps the arena.
// RUN: %weavec_cc %S/Inputs/runtime-shim.c -o %t/shim 2>&1 | FileCheck --allow-empty --check-prefix=SHIM %s
// RUN: env WEAVEC_RT_STATS=1 %t/shim 2>&1 | FileCheck --check-prefix=SHIMMED %s
//
// The program runs with the runtime's allocator, and says what it did when asked.
// RUN: %weavec_cc %t/main.o -o %t/prog
// RUN: env WEAVEC_RT_STATS=1 %t/prog 2>&1 | FileCheck --check-prefix=STATS %s
#include <stdlib.h>

int main(void) {
  char *p = malloc(4);
  if (!p)
    return 1;
  p[0] = 0;
  free(p);
  return 0;
}

// LINK: "-u" "{{_?}}malloc" "{{[^"]*}}libweavec_alloc.a" "{{[^"]*}}libweavec_rt.a"

// NONE-NOT: libweavec_

// ASAN: weavec-cc: note: building without the WeaveC runtime (-fsanitize=address replaces the allocator): memory accesses are not guarded
// ASAN-NOT: libweavec_alloc.a

// NOSTDLIB: weavec-cc: note: building without the WeaveC runtime (-nostdlib links no C library): memory accesses are not guarded
// NOSTDLIB-NOT: libweavec_alloc.a

// OWN: weavec-cc: note: '{{.*}}alloc.o' defines the allocator, so the WeaveC runtime's is not linked: the heap is untracked, guards pass on it and releases are not validated

// SHIM-NOT: defines the allocator

// SHIMMED: shim
// SHIMMED: weavec: runtime: 1 allocations

// STATS: weavec: runtime: {{[0-9]+}} allocations
// STATS-NEXT: weavec: runtime: {{[0-9]+}} releases
