// Definitions in included headers are not analysed unless --analyze-headers.
// RUN: %weavec %s -- 2>&1 | count 0
// RUN: not %weavec --analyze-headers %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include "Inputs/buggy-header.h"

void fine(void) {}

// CHECK: buggy-header.h:{{[0-9]+}}:{{[0-9]+}}: error: 'p' is freed twice [weavec::double-free]
