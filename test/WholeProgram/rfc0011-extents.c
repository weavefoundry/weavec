// RFC 0011, *Extents in summaries*: an allocation's extent, a callee's
// requirement on a parameter's extent, and the offset a callee releases at
// all cross translation units, in-process and through the sidecar.
//
// RUN: not %weavec --whole-program %s %S/Inputs/buffers.c -- -I%S/Inputs 2>&1 | FileCheck %s
// RUN: not %weavec --whole-program --dump-analysis %s %S/Inputs/buffers.c -- -I%S/Inputs 2>&1 | FileCheck --check-prefix=DUMP %s
//
// Alone, the calls are unchecked boundaries: nothing is reported.
// RUN: %weavec %s -- -I%S/Inputs 2>&1 | FileCheck --check-prefix=ALONE %s
//
// The same through weavec-cc: the sidecar carries all three (format 7).
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %S/Inputs/buffers.c -o %t/buffers.o -I%S/Inputs 2>&1 | count 0
// RUN: %weavec_cc -c %s -o %t/main.o -I%S/Inputs 2>&1 | count 0
// RUN: FileCheck --check-prefix=SIDECAR %s < %t/buffers.o.weavec
// RUN: not %weavec_cc %t/buffers.o %t/main.o -o %t/prog 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include "buffers.h"

// DUMP: program:
// DUMP: function 'buffer_fill': param 0 *: written; stores{} returns{} requires{param 0} requires-extent{param 0: param 1 scale 1 plus 0}
// DUMP-NEXT: function 'buffer_new': stores{} returns{fresh(free) extent param 0 scale 1 plus 0, null}
// DUMP-NEXT: function 'buffer_put8': param 0 *: written; stores{} returns{} requires{param 0} requires-extent{param 0: 8}
// DUMP: function 'wrapped_release': param 0: freed(free),at(-struct~wrapped.payload); stores{} returns{}

// SIDECAR: weavec-summaries 7
// SIDECAR: function buffer_fill
// SIDECAR: requires-extent 0 param 1 scale 1 plus 0
// SIDECAR: function buffer_new
// SIDECAR: return fresh(free) extent param 0 scale 1 plus 0
// SIDECAR: function buffer_put8
// SIDECAR: requires-extent 0 8
// SIDECAR: function wrapped_release
// SIDECAR: effect param 0 freed(free),at(-struct~wrapped.payload)

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
  // CHECK: rfc0011-extents.c:[[@LINE+1]]:15: error: 'buffer_put8' requires 8 bytes behind 'b', which has 4 bytes [weavec::out-of-bounds]
  buffer_put8(b);
  // CHECK: rfc0011-extents.c:[[@LINE-5]]:13: note: 'b' is allocated here
  free(b);
}

// A symbolic requirement against a constant extent.
void short_fill(void) {
  char buf[16];
  buffer_fill(buf, 16);
  // CHECK: rfc0011-extents.c:[[@LINE+1]]:15: error: 'buffer_fill' requires 17 bytes behind 'buf', which has 16 bytes [weavec::out-of-bounds]
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
  // CHECK: rfc0011-extents.c:[[@LINE+1]]:3: error: 'w' is freed twice [weavec::double-free]
  wrapped_release(w->payload);
}
