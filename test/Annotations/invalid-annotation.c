// Unknown weavec.* annotations are reported; foreign annotations are ignored.
// RUN: %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"

// CHECK: invalid-annotation.c:[[@LINE+1]]:47: warning: unrecognised weavec annotation on 'typo' [weavec::invalid-annotation]
__attribute__((annotate("weavec.ownd"))) void typo(void) {}

__attribute__((annotate("gsl.owner"))) void foreign(void) {}

// CHECK: 1 warning generated.
