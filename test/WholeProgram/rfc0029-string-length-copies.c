// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/string-length-copies/client.c %S/../evaluation/rfc0029/string-length-copies/callback.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/string-length-copies/client.c %S/../evaluation/rfc0029/string-length-copies/overread.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
