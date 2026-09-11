// RUN: rm -rf %t.dir
// RUN: mkdir -p %t.dir
// RUN: %weavec_cc -c %S/Inputs/rfc0014-callback-helper.c -o %t.dir/helper.o
// RUN: FileCheck %s --check-prefix=SIDECAR < %t.dir/helper.o.weavec
// RUN: %weavec_cc -c %S/Inputs/rfc0014-callback-client.c -o %t.dir/client.o
// RUN: not %weavec_cc %t.dir/helper.o %t.dir/client.o -o %t.dir/program 2>&1 | FileCheck %s --check-prefix=LINK
// RUN: not %weavec --whole-program %S/Inputs/rfc0014-callback-client.c %S/Inputs/rfc0014-callback-helper.c -- 2>&1 | FileCheck %s --check-prefix=LINK
// RFC 0014: the same callback bug is checked through the CLI and sidecars.
// SIDECAR: weavec-summaries 18
// SIDECAR: function invoke external plain
// SIDECAR: accepts-callbacks
// SIDECAR: callback-input param 0
// LINK: error: use of 'p' after it was freed [weavec::use-after-free]
// LINK-NOT: annotation-required
// LINK-NOT: analysis-incomplete
