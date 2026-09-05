// RFC 0005, *Debug output*: in whole-program mode --dump-analysis prints
// each unit's dump in analysis order (dependencies first), prefixed with the
// unit, then the program database. The format is a debugging aid; this pins
// only its shape.
//
// RUN: %weavec --whole-program --dump-analysis %s %S/Inputs/node.c -- -I%S/Inputs | FileCheck %s
#include "../Inputs/prelude.h"
#include "node.h"

// CHECK: unit '{{.*}}node.c':
// CHECK: function 'node_free':
// CHECK: unit '{{.*}}rfc0005-dump.c':
// CHECK: function 'release':
// CHECK-NEXT: places:
// CHECK: program:
// CHECK-NEXT: function 'node_free': param 0: freed(free); param 0 *.name: freed(free); stores{} returns{}
// CHECK-NEXT: function 'node_new': stores{} returns{fresh(free) extent 16, null}
// CHECK-NEXT: function 'node_set_name': param 0 *.name: written,freed(free),replaced; stores{param 0 *.name = copy param 1} returns{} requires{param 0}
// CHECK-NEXT: function 'node_vp': stores{} returns{copy param 0 @+struct~node.v} requires{param 0}
// CHECK-NEXT: function 'release': param 0: freed(free); stores{} returns{}

void release(struct node *n) { node_free(n); }
