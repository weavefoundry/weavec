// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/numeric-container-frames/local-end.c %S/../evaluation/rfc0029/numeric-container-frames/local-end-inspect.c %S/../evaluation/rfc0029/numeric-container-frames/reader.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/numeric-container-frames/owned-end.c %S/../evaluation/rfc0029/numeric-container-frames/owned-end-inspect.c %S/../evaluation/rfc0029/numeric-container-frames/reader.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
