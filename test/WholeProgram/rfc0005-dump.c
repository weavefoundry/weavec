// RFC 0005, *Debug output*: in whole-program mode --dump-analysis prints
// each unit's dump in analysis order, prefixed with the
// unit, then the program database. The format is a debugging aid; this pins
// only its shape.
//
// RUN: %weavec --whole-program --dump-analysis %s %S/Inputs/node.c -- -I%S/Inputs | FileCheck %s
#include "../Inputs/prelude.h"
#include "node.h"

// Dependencies come first: node.c defines what this unit calls (RFC 0031
// §7; the RFC 0016 context-request components are gone, §6.1). Each unit
// prints its functions' format-30 summaries (RFC 0031 *Summary format 30*).
// CHECK: unit '{{.*}}node.c':
// CHECK-NEXT: function 'node_new':
// CHECK-NEXT: summary:
// CHECK: function 'node_free':
// CHECK: unit '{{.*}}rfc0005-dump.c':
// CHECK-NEXT: function 'release':
// CHECK-NEXT: summary:
// CHECK: program:
// CHECK-NEXT: function 'node_free':
// CHECK-NEXT: always-returns
// CHECK-NEXT: release *param0 free when param 0 !=0
// CHECK-NEXT: release *param0->name free when param 0 !=0
// CHECK-NEXT: function 'node_new':
// CHECK-NEXT: always-returns
// CHECK-NEXT: result fresh#0 free extent 16 {{.*}}maybe-null when null nonnull
// CHECK-NEXT: store result->name := null
// CHECK-NEXT: function 'node_set_name':
// CHECK-NEXT: always-returns
// CHECK-NEXT: release *param0->name free when always
// CHECK-NEXT: store param0->name := path param1
// CHECK-NEXT: function 'node_vp':
// CHECK-NEXT: always-returns
// CHECK-NEXT: result path param0 when nonnull
// CHECK-NEXT: nonnull-on nonnull param0
// `node_free` releases only a non-null argument, so `release` releases its
// own argument possibly (the key `param 0 !=0` is not carried over).
// CHECK-NEXT: function 'release':
// CHECK-NEXT: always-returns
// CHECK-NEXT: release *param0 free {{(may )?}}when
// CHECK-NEXT: release *param0->name free {{(may )?}}when

void release(struct node *n) { node_free(n); }
