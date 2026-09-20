// RFC 0030 §13.2 step 5, §11: when one unit defines the allocator and
// another lowered its allocation calls to the zero-initialising wrappers,
// the link warns once, and the program ledger names the unit under A5. A
// build without zero-initialisation lowers nothing and is not warned about.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %S/Inputs/rfc0030-allocator.c -o %t/alloc.o
// RUN: %weavec_cc -c %s -o %t/main.o
// RUN: %weavec_cc -fweavec-ledger=%t/program.json %t/alloc.o %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=WARN %s
// RUN: FileCheck --check-prefix=LEDGER %s < %t/program.json
// RUN: %weavec --dump-record=%t/alloc.o.weavec | FileCheck --check-prefix=RECORD %s
//
// RUN: %weavec_cc -fno-weavec-zero-init -c %s -o %t/plain.o
// RUN: %weavec_cc %t/alloc.o %t/plain.o -o %t/prog2 2>&1 | FileCheck --allow-empty --check-prefix=QUIET %s
#include <stdlib.h>

int main(void) {
  char *p = malloc(4);
  if (!p)
    return 1;
  p[0] = 0;
  free(p);
  return 0;
}

// WARN: weavec-cc: warning: heap zero-initialisation assumes the system allocator, but '{{.*}}rfc0030-allocator.c' defines 'malloc'; rebuild with -fno-weavec-zero-init
// WARN-NOT: warning: heap zero-initialisation

// LEDGER: "A5": {
// LEDGER-NEXT: "nonLoweredAllocations": {{[0-9]+}},
// LEDGER-NEXT: "bypassedDeclarations": 0,
// LEDGER-NEXT: "allocatorDefinedBy": "{{.*}}rfc0030-allocator.c"

// RECORD: "definesAllocator": true,
// RECORD-NEXT: "a5": {
// RECORD-NEXT: "loweredAllocations": 0,
// RECORD: "allocator": "malloc"

// QUIET-NOT: heap zero-initialisation
