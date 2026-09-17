// RUN: %weavec --checked-function=pair %S/../evaluation/rfc0029/initialized-spans/library.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/initialized-spans/library.c %S/../evaluation/rfc0029/initialized-spans/good.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/initialized-spans/library.c %S/../evaluation/rfc0029/initialized-spans/unrelated.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety: callee requires a live initialized same-array byte span [weavec::checking-incomplete]
// RFC 0029: a span is an explicit caller premise, never pointer-name evidence.
