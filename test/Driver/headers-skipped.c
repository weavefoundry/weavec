// Definitions in included headers are not analysed yet. RFC 0030 §5.6
// removes --analyze-headers and analyses every emitted function, headers
// included; this test is then inverted (headers-analysed.c).
// RUN: %weavec %s -- 2>&1 | FileCheck --allow-empty --check-prefix=QUIET %s
// QUIET-NOT: {{warning|error}}:
#include "../Inputs/prelude.h"
#include "Inputs/buggy-header.h"

void fine(void) {}

