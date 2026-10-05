// RUN: rm -rf %t.dir
// RUN: mkdir -p %t.dir
// RUN: %weavec_cc -c %S/Inputs/rfc0014-callback-helper.c -o %t.dir/helper.o
// RUN: %weavec --dump-record=%t.dir/helper.o.weavec | FileCheck %s --check-prefix=RECORD
// RUN: %weavec_cc -c %S/Inputs/rfc0014-callback-client.c -o %t.dir/client.o
// RUN: not %weavec_cc -fweavec-link=analyze %t.dir/helper.o %t.dir/client.o -o %t.dir/program 2>&1 | FileCheck %s --check-prefix=LINK
// RUN: not %weavec --whole-program %S/Inputs/rfc0014-callback-client.c %S/Inputs/rfc0014-callback-helper.c -- 2>&1 | FileCheck %s --check-prefix=LINK
// RFC 0014: the same callback bug is checked through the CLI and the unit
// records (RFC 0030 §13.1).
//
// RFC 0031 §6.1: format 30 has no callback inputs. In its own unit,
// `invoke`'s callback is unknown code that may do anything to `userdata`
// and to any global (RFC 0031 *Implementation amendments*, "Globals that
// unknown code may write");
// at link, the call through the callback parameter takes the parameter's
// slot solution (RFC 0030 §9.3), which is `drop`, so the client's read
// after `invoke(drop, p)` is a use after free.
// RECORD: "format": 31,
// RECORD: "name": "invoke",
// RECORD-NEXT: "linkage": "external",
// RECORD-NEXT: "addressTaken": false,
// RECORD-NEXT: "typeKey": "void (void (*)(void *), void *)",
// RECORD-NEXT: "effects": "returns always\nunknown-globals\neffect unknown p1* when=-:- may\n{{.*}}",
// LINK: error: use of 'p' after it was freed [weavec::use-after-free]
// LINK-NOT: annotation-required
// LINK-NOT: analysis-incomplete
