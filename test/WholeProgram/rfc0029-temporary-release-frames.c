// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/temporary-release-frames/client.c %S/../evaluation/rfc0029/temporary-release-frames/guarded.c %S/../evaluation/rfc0029/temporary-release-frames/reader.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/temporary-release-frames/client.c %S/../evaluation/rfc0029/temporary-release-frames/attached.c %S/../evaluation/rfc0029/temporary-release-frames/reader.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
