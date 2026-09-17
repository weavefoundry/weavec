// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/mutual-cleanup.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/transport/client.c %S/../evaluation/rfc0029/transport/library.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --whole-program --checked-function=main --checked-function=encode_client %S/../evaluation/rfc0029/serializer/client.c %S/../evaluation/rfc0029/serializer/hex.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/transport/bad.c %S/../evaluation/rfc0029/transport/library.c -- 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/offsets/forward.c -- 2>&1 | FileCheck %s --check-prefix=OFFSET
// CLEAN-NOT: error:
// BAD: cannot establish checked safety: access interval must fit its object [weavec::checking-incomplete]
// OFFSET: checked safety failed: 'p' is released but points 1 element past the start of its allocation [weavec::checking-failed]
// RFC 0029: recursive group proof, imported state roles and actual callbacks.
