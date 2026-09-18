// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/character-pointer-slots/end.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/character-pointer-slots/unrelated.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety: unsupported checked C construct or storage type [weavec::checking-incomplete]
// RFC 0029: compatible character-pointer views retain the original slot rules.
