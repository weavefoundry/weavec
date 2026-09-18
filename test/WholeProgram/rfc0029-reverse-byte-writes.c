// RUN: %weavec --checked-function=encode %S/../evaluation/rfc0029/reverse-byte-writes/library.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/reverse-byte-writes/library.c %S/../evaluation/rfc0029/reverse-byte-writes/good.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/reverse-byte-writes/library.c %S/../evaluation/rfc0029/reverse-byte-writes/short.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
// RFC 0029: sufficient scalar envelopes retain actual caller bounds checks.
