// RFC 0038 §1: a guarded link carries the runtime's library before the first
// library on its line, with an rpath to the toolchain's runtime directory
// (or the one -fweavec-rpath names, or none). Shared objects get it too; a
// relocatable link and an unguarded one get nothing. The image records
// the runtime's versioned name (amendment 19). A sanitizer that
// replaces the allocator turns the runtime off with a note; a unit that
// defines the allocator keeps it, with a note.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %s -o %t/main.o
// RUN: %weavec_cc -### %t/main.o -lm -o %t/prog 2>&1 | FileCheck --check-prefix=LINK %s
// RUN: %weavec_cc -### %shared %t/main.o -lm -o %t/lib%so 2>&1 | FileCheck --check-prefix=LINK %s
// RUN: %weavec_cc -### -fweavec-rpath=/opt/app/lib %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=RPATH %s
// RUN: %weavec_cc -### -fno-weavec-rpath %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=NORPATH %s
// RUN: %weavec_cc -### -r %t/main.o -o %t/all.o 2>&1 | FileCheck --check-prefix=NONE %s
// RUN: %weavec_cc -### -fweavec-checks=none %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=NONE %s
// RUN: %weavec_cc -### -fsanitize=address %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=ASAN %s
// RUN: %weavec_cc -### -nostdlib %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=NOSTDLIB %s
// RUN: not %weavec_cc -fweavec-rpath %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=MISSING %s
//
// Amendment 23: a toolchain's default-rpath file names the rpath a link
// records by default (Homebrew's stable path).
// RUN: mkdir -p %t/rt && touch %t/rt/libweavec_rt.so %t/rt/libweavec_rt.dylib %t/rt/libweavec_rt.a
// RUN: echo /opt/stable/lib/weavec > %t/rt/default-rpath
// RUN: env WEAVEC_RESOURCE_DIR=%t/rt %weavec_cc -### %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=STABLE %s
//
// Amendment 20: the archive is never loaded whole, which would put a second
// runtime in the image.
// RUN: %if host-linux %{ %weavec_cc -### %t/main.o -Wl,--whole-archive -lm -Wl,--no-whole-archive -o %t/prog 2>&1 | FileCheck --check-prefix=WHOLE %s %}
// RUN: %if host-darwin %{ %weavec_cc -### %t/main.o -Wl,-all_load -o %t/prog 2>&1 | FileCheck --check-prefix=ALLLOAD %s %}
//
// The archive after the library is what a link by another driver finds in a
// weavec-cc link's implicit libraries (CMake's mixed C and C++ targets): that
// program carries the runtime itself, and a weavec-cc link takes nothing
// from it.
// RUN: %weavec_cc -O1 -c %S/Inputs/runtime-overflow.c -o %t/overflow.o
// RUN: %clang %t/overflow.o %{rt_dir}/libweavec_rt.a %if host-linux %{ -lpthread -ldl %} -o %t/foreign
// RUN: %t/foreign 4
// RUN: not --crash %t/foreign 5 2>&1 | FileCheck --check-prefix=FOREIGN %s
// RUN: %weavec_cc %t/overflow.o -o %t/own-link
// RUN: llvm-nm --defined-only %t/own-link | FileCheck --check-prefix=NOCOPY %s
//
// RUN: %weavec_cc -c %S/Inputs/rfc0030-allocator.c -o %t/alloc.o
// RUN: %weavec_cc %t/alloc.o %t/main.o -o %t/own 2>&1 | FileCheck --check-prefix=OWN %s
// RUN: %t/own
//
// A program's own shim over the standard functions keeps the arena.
// RUN: %weavec_cc %S/Inputs/runtime-shim.c -o %t/shim 2>&1 | FileCheck --allow-empty --check-prefix=SHIM %s
// RUN: env WEAVEC_RT_STATS=1 %t/shim 2>&1 | FileCheck --check-prefix=SHIMMED %s
//
// The program runs with the runtime's allocator, and says what it did when asked.
// RUN: %weavec_cc %t/main.o -o %t/prog
// RUN: llvm-objdump -p %t/prog | FileCheck --check-prefix=NEEDED %s
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

// LINK: "{{[^"]*}}lib/weavec{{/|\\\\}}libweavec_rt.{{dylib|so}}" "-rpath" "{{[^"]*}}lib/weavec" "{{[^"]*}}libweavec_rt.a"{{.*}} "-lm"

// RPATH: libweavec_rt.{{dylib|so}}" "-rpath" "/opt/app/lib"

// NORPATH: libweavec_rt.{{dylib|so}}"
// NORPATH-NOT: "-rpath"

// NONE-NOT: libweavec_rt

// ASAN: weavec-cc: note: building without the WeaveC runtime (-fsanitize=address replaces the allocator): memory accesses are not guarded
// ASAN-NOT: libweavec_rt

// NOSTDLIB: weavec-cc: note: building without the WeaveC runtime (-nostdlib links no C library): memory accesses are not guarded
// NOSTDLIB-NOT: libweavec_rt

// MISSING: weavec-cc: error: missing value for '-fweavec-rpath'

// STABLE: "-rpath" "/opt/stable/lib/weavec"

// WHOLE: "--whole-archive" "{{[^"]*}}libweavec_rt.so" "-rpath" "{{[^"]*}}" "--no-whole-archive" "{{[^"]*}}libweavec_rt.a" "--whole-archive" "-lpthread" "-lm" "--no-whole-archive"

// ALLLOAD-NOT: libweavec_rt.a
// ALLLOAD: "-all_load"
// ALLLOAD-NOT: libweavec_rt.a

// Amendment 19: the image names the runtime of its ABI.
// NEEDED: {{libweavec_rt\.so\.[0-9]+|@rpath/libweavec_rt\.[0-9]+\.dylib}}
// FOREIGN: weavec: heap-buffer-overflow
// NOCOPY-NOT: __weavec_rt_shadow

// OWN: weavec-cc: note: '{{.*}}alloc.o' defines the allocator, which serves the program instead of the WeaveC runtime's: the heap is untracked, guards pass on it and releases are not validated

// SHIM-NOT: defines the allocator

// SHIMMED: shim
// SHIMMED: weavec: runtime: 1 allocations

// STATS: weavec: runtime: {{[0-9]+}} allocations
// STATS-NEXT: weavec: runtime: {{[0-9]+}} releases
