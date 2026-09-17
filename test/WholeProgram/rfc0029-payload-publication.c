// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/payload-publication/client.c %S/../evaluation/rfc0029/payload-publication/good.c %S/../evaluation/rfc0029/payload-publication/drop.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/payload-publication/client.c %S/../evaluation/rfc0029/payload-publication/helper.c %S/../evaluation/rfc0029/payload-publication/drop.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/payload-publication/client.c %S/../evaluation/rfc0029/payload-publication/duplicate.c %S/../evaluation/rfc0029/payload-publication/drop.c -- 2>&1 | FileCheck %s
// CHECK: error: cannot establish checked safety: container operation loses part of the owned allocation footprint [weavec::checking-incomplete]
// RFC 0029: publication transfers a proved live allocation exactly once.
