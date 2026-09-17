// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/floating-call-premises/forwarded.c %S/../evaluation/rfc0029/floating-call-premises/clamp.c %S/../evaluation/rfc0029/floating-call-premises/forward.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/floating-call-premises/nan.c %S/../evaluation/rfc0029/floating-call-premises/clamp.c %S/../evaluation/rfc0029/floating-call-premises/forward.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety: unsupported checked C construct or storage type
