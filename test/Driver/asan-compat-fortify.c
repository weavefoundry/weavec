// RFC 0036 §1: on Darwin, an optimising build keeps the SDK's fortified
// string functions under ASan compatibility (Clang's ASan turns them off).
// REQUIRES: host-darwin
// RUN: %weavec_cc -O2 -E -dM %s | FileCheck %s
// CHECK: #define _FORTIFY_SOURCE 2
#include <string.h>
