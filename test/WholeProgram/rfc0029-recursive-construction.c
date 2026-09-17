// RUN: %weavec --checked-function=build --checked-function=main %S/../evaluation/rfc0029/construction/build.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/recursive-transport/client.c %S/../evaluation/rfc0029/recursive-transport/library.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=build %S/../evaluation/rfc0029/construction/cycle.c -- 2>&1 | FileCheck %s --check-prefix=CYCLE
// RUN: not %weavec --checked-function=build %S/../evaluation/rfc0029/construction/leak.c -- 2>&1 | FileCheck %s --check-prefix=LEAK
// CLEAN-NOT: error:
// CYCLE: cannot establish checked safety: recursive proof cycle has no strict progress [weavec::checking-incomplete]
// LEAK: cannot establish checked safety: container operation loses part of the owned allocation footprint [weavec::checking-incomplete]
// RFC 0029: fresh recursive outputs require decreasing initialized inputs and
// complete allocation accounting on success and failure paths.
