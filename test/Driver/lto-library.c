// RFC 0038 amendment 22: on Darwin a -flto link uses the libLTO of the LLVM
// weavec-cc is built with, which the toolchain carries as lib/libLTO.dylib
// (the system linker's own cannot read its bitcode), and the guards hold.
// REQUIRES: host-darwin
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -O2 -flto=thin -c %S/Inputs/runtime-overflow.c -o %t/overflow.o
// RUN: %weavec_cc -### -flto=thin %t/overflow.o -o %t/prog 2>&1 | FileCheck --check-prefix=LINK %s
// RUN: %weavec_cc -flto=thin %t/overflow.o -o %t/prog
// RUN: %t/prog 4
// RUN: not --crash %t/prog 5 2>&1 | FileCheck --check-prefix=TRAP %s
//
// LINK: "-lto_library" "{{[^"]*}}lib{{/|\\\\}}libLTO.dylib"
// TRAP: weavec: heap-buffer-overflow
