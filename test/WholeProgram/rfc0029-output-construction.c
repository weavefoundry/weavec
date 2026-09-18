// RUN: %weavec --checked-function=build --checked-function=main %S/../evaluation/rfc0029/output-construction/build.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/output-transport/client.c %S/../evaluation/rfc0029/output-transport/library.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=build %S/../evaluation/rfc0029/output-construction/uncleared-failure.c -- 2>&1 | FileCheck %s --check-prefix=OUTPUT
// CLEAN-NOT: error:
// OUTPUT: cannot establish checked safety: recursive construction does not establish its complete output contract [weavec::checking-incomplete]
// RFC 0029: an output-slot candidate needs actual failure nullness and a
// complete fresh forest on every successful return.
