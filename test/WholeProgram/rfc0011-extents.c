// RFC 0011, *Extents in summaries*: an allocation's extent, the extent of
// what a callee writes through a parameter, and the offset a callee releases
// at all cross translation units, in-process and through the unit record.
//
// RFC 0031 §6.1: format-30 summaries carry no extent requirements; a store
// the callee makes on every return, outside the caller's object, is an
// error at the call (RFC 0031 *Implementation amendments*, "Stores past the
// caller's object").
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: not %weavec --no-runtime --whole-program %s %S/Inputs/buffers.c -- -I%S/Inputs 2>&1 | FileCheck %s
// RUN: not %weavec --no-runtime --whole-program --ledger=%t/program.json %s %S/Inputs/buffers.c -- -I%S/Inputs 2>/dev/null
// RUN: FileCheck --check-prefix=LEDGER %s < %t/program.json
// RUN: not %weavec --no-runtime --whole-program --dump-analysis %s %S/Inputs/buffers.c -- -I%S/Inputs 2>/dev/null | FileCheck --check-prefix=DUMP %s
//
// Alone, the calls are unchecked boundaries: nothing is reported.
// RUN: %weavec --no-runtime %s -- -I%S/Inputs 2>&1 | FileCheck --check-prefix=ALONE %s
//
// The same through weavec-cc: the unit record carries all three.
// RUN: %weavec_cc -c %S/Inputs/buffers.c -o %t/buffers.o -I%S/Inputs 2>&1 | count 0
// RUN: %weavec_cc -c %s -o %t/main.o -I%S/Inputs 2>&1 | count 0
// RUN: %weavec --dump-record=%t/buffers.o.weavec | FileCheck --check-prefix=RECORD %s
// RUN: not %weavec_cc -fweavec-link=analyze %t/buffers.o %t/main.o -o %t/prog 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include "buffers.h"

// DUMP: program:
// DUMP: function 'buffer_fill':
// DUMP-NEXT: always-returns
// DUMP-NEXT: store *param0[*] elements [0, param1) := int [0, 0]
// DUMP-NEXT: function 'buffer_new':
// DUMP-NEXT: always-returns
// DUMP-NEXT: result fresh#0 free extent param0 {{.*}}when null nonnull
// DUMP-NEXT: function 'buffer_put8':
// DUMP-NEXT: always-returns
// DUMP-NEXT: store *param0[*] elements [0, 8) := int [0, 7]
// DUMP: function 'wrapped_release':
// DUMP-NEXT: always-returns
// DUMP-NEXT: release *param0 free offset -4 when always

// RECORD: "format": 31,
// RECORD: "name": "buffer_fill",
// RECORD: "effects": "{{.*}}store p0*[] when=-:- elements=0,p1@1@0 :: int lo=0 hi=0\nreads p0*\nwrites p0*\n",
// RECORD: "kind": "counted(param 1 scale 1 plus 0) nonnull",
// RECORD: "name": "buffer_new",
// RECORD: "effects": "{{.*}}result classes=null,nonnull :: fresh family=free extent=p0@1@0 {{.*}}\n",
// RECORD: "name": "buffer_put8",
// RECORD: "effects": "{{.*}}store p0*[] when=-:- elements=0,8 :: int lo=0 hi=7\nreads p0*\nwrites p0*\n",
// RECORD: "kind": "counted(8) nonnull",
// RECORD: "name": "wrapped_release",
// RECORD: "effects": "{{.*}}effect release p0* when=-:- family=free offset=-4\n",

// ALONE-NOT: error:
// ALONE-NOT: out-of-bounds

// Clean: the allocation is as large as the callee needs.
void ok(void) {
  char *b = buffer_new(8);
  if (!b)
    return;
  buffer_put8(b);
  buffer_fill(b, 8);
  free(b);
}

// The extent comes back from `buffer_new`; the requirement from `buffer_put8`.
void short_alloc(void) {
  char *b = buffer_new(4);
  if (!b)
    return;
  // CHECK: rfc0011-extents.c:[[@LINE+2]]:15: error: 'buffer_put8' requires 8 bytes behind 'b', which has 4 bytes [weavec::out-of-bounds]
  // CHECK: rfc0011-extents.c:[[@LINE-4]]:13: note: 'b' is allocated here
  buffer_put8(b);
  free(b);
}

// A symbolic requirement against a constant extent.
void short_fill(void) {
  char buf[16];
  buffer_fill(buf, 16);
  // CHECK: rfc0011-extents.c:[[@LINE+6]]:15: error: 'buffer_fill' requires 17 bytes behind 'buf', which has 16 bytes [weavec::out-of-bounds]
  // LEDGER: "text": "buffer_fill(buf,17)",
  // LEDGER: "spatial": {
  // LEDGER-NEXT: "outcome": "violation",
  // LEDGER-NEXT: "reason": null,
  // LEDGER-NEXT: "detail": "'buffer_fill' requires 17 bytes behind 'buf', which has 16 bytes",
  buffer_fill(buf, 17);
}

// A local index the caller proves against the extent it got back.
void indexed(size_t n) {
  char *b = buffer_new(n);
  if (!b)
    return;
  // CHECK: rfc0011-extents.c:[[@LINE+1]]:3: error: 'b[n]' is out of bounds: 'n' is the number of elements of 'b' [weavec::out-of-bounds]
  b[n] = 0;
  free(b);
}

// The release at an offset composes: `payload` is a derived pointer into
// the `struct wrapped`, and the callee frees the whole from it.
void release_wrapped(void) {
  struct wrapped *w = malloc(sizeof *w);
  if (!w)
    return;
  w->tag = 1;
  wrapped_release(w->payload);
}

// ... so the same pointer given twice is a double free of the container.
void release_wrapped_twice(void) {
  struct wrapped *w = malloc(sizeof *w);
  if (!w)
    return;
  wrapped_release(w->payload);
  // The report names the argument the callee released through.
  // CHECK: rfc0011-extents.c:[[@LINE+1]]:3: error: 'w->payload' is freed twice [weavec::double-free]
  wrapped_release(w->payload);
}
