// RFC 0038 amendment 24: a response file (`@file`) is expanded before
// weavec-cc reads the command line, so a source in it is compiled as WeaveC
// (its macro, its header) and an assembly source in it goes to the system
// assembler.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: echo '-c %s -o %t/unit.o' > %t/c.rsp
// RUN: %weavec_cc @%t/c.rsp
// RUN: echo '-c %S/Inputs/empty.S -o %t/empty.o' > %t/asm.rsp
// RUN: %weavec_cc @%t/asm.rsp
// RUN: test -f %t/unit.o && test -f %t/empty.o
// RUN: not %weavec_cc @%t/missing.rsp 2>&1 | FileCheck %s
//
// CHECK: weavec-cc: error:
#include <weavec.h>

#ifndef __WEAVEC__
#error "compiled without WeaveC"
#endif

int unit(void) { return 0; }
