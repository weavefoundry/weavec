// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/recursive-cases/zero.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/recursive-cases/live.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=walk %S/../evaluation/rfc0029/recursive-cases/generic.c -- 2>&1 | FileCheck %s --check-prefix=LIMIT
// CLEAN-NOT: error:
// LIMIT: summary iteration limit
// RFC 0029: a proved unreachable recursive branch does not exhaust the case;
// the independently selected generic recursive definition retains its limit.
