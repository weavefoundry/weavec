// RFC 0005, *weavec-cc*: the compile step analyses the unit alone, writes
// the object and its unit record (`.weavec`, RFC 0030 §13.1) next to it,
// and defers boundary warnings; the link step reads the records, analyses
// the program, reports what needs two files, and refuses to link on an
// error.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %S/../WholeProgram/Inputs/node.c -o %t/node.o -I%S/../WholeProgram/Inputs > %t/compile.log 2>&1
// RUN: count 0 < %t/compile.log
// RUN: %weavec_cc -c %s -o %t/main.o -I%S/../WholeProgram/Inputs > %t/compile.log 2>&1
// RUN: count 0 < %t/compile.log
// RUN: %weavec --dump-record=%t/node.o.weavec | FileCheck --check-prefix=RECORD %s
// RUN: %weavec --dump-record=%t/main.o.weavec | FileCheck --check-prefix=MAIN %s
// RUN: not %weavec_cc %t/node.o %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=LINK %s
// RUN: not ls %t/prog
//
// A one-step build reports the same and produces nothing.
// RUN: not %weavec_cc %S/../WholeProgram/Inputs/node.c %s -o %t/prog1 -I%S/../WholeProgram/Inputs 2>&1 | FileCheck --check-prefix=LINK %s
// RUN: not ls %t/prog1
//
// -fno-weavec-link skips the whole-program step; the objects link as usual.
// RUN: %weavec_cc -fno-weavec-link %t/node.o %t/main.o -o %t/prog2 2>&1 | count 0
// RUN: ls %t/prog2
//
// A callee no unit defines is unknown code at the link step too: RFC 0030
// §5.1 records the calls as ledger rows and reports nothing, and the linker
// fails on the undefined symbols.
// RUN: %weavec_cc -c %s -o %t/bnd.o -I%S/../WholeProgram/Inputs -DBOUNDARY 2>&1 | count 0
// RUN: %weavec --dump-record=%t/bnd.o.weavec | FileCheck --check-prefix=DEFERRED %s
// RUN: not %weavec_cc %t/node.o %t/bnd.o -o %t/prog3 2>&1 | FileCheck --check-prefix=BOUNDARY %s
//
// A record written for another object is stale (RFC 0030 §13.1): here the
// object is rebuilt without WeaveC, which leaves the old record behind. The
// input is named in the link's `unanalyzed-input` warning (§13.2), and the
// object is unknown code, so nothing is checked and the link goes ahead.
// RUN: %weavec_cc -fno-weavec -c %s -o %t/main.o -I%S/../WholeProgram/Inputs
// RUN: %weavec_cc %t/node.o %t/main.o -o %t/prog4 2>&1 | FileCheck --check-prefix=STALE %s
// RUN: ls %t/prog4
#include "../Inputs/prelude.h"
#include "node.h"

// RECORD: "format": 28,
// RECORD: "source": "{{.*}}node.c",
// RECORD-NEXT: "cwd": "{{.+}}",
// RECORD-NEXT: "command": [
// RECORD-NEXT: "-triple",
// RECORD: "-emit-obj",
// RECORD: "object": {
// RECORD-NEXT: "path": "{{.*}}node.o",
// RECORD-NEXT: "digest": "sha256:{{[0-9a-f]+}}"
// RECORD: "functions": [
// RECORD: "name": "node_free",
// RECORD-NEXT: "linkage": "external",
// RECORD-NEXT: "addressTaken": false,
// RECORD-NEXT: "typeKey": "void (struct node *)",
// RECORD-NEXT: "summary": "summary\n  object-view param 0 * {{.*}}\n  effect param 0 freed(free)\n  effect param 0 *.name freed(free){{.*}}\nend\n",
// RECORD: "acceptsMemory": true,
// RECORD: "name": "node_new",
// RECORD-NEXT: "linkage": "external",
// RECORD-NEXT: "addressTaken": false,
// RECORD-NEXT: "typeKey": "struct node *(void)",
// RECORD-NEXT: "summary": "summary\n{{.*}}  return fresh(free){{.*}}\n  return null\nend\n",
// RECORD: "name": "node_set_name",
// RECORD: "typeKey": "void (struct node *, char *)",
// RECORD-NEXT: "summary": "summary\n  object-view param 0 * {{.*}}\n  effect param 0 *.name written,freed(free),replaced\n  store param 0 *.name copy param 1\n{{.*}}  requires 0\nend\n",
// RECORD: "name": "node_vp",
// RECORD: "typeKey": "int *(struct node *)",
// RECORD-NEXT: "summary": "summary\n  object-view param 0 * {{.*}}\n  return copy param 0 @+struct~node.v\n  requires 0\nend\n",
// RECORD: "imports": [
// RECORD: "name": "free",
// RECORD: "name": "malloc",
// RECORD: "sites": [
// RECORD: "function": "node_new",

// MAIN: "functions": [],
// MAIN: "imports": [
// MAIN: "name": "node_free",
// MAIN: "calls": [
// MAIN-NEXT: {
// MAIN-NEXT: "function": "main",
// MAIN: "name": "node_new",
// MAIN: "name": "node_vp",
// MAIN: "unknown": [
// MAIN: "node_free",
// MAIN: "reported": [],

// DEFERRED: "unknown": [
// DEFERRED-NEXT: "blob_close",
// DEFERRED-NEXT: "blob_open",

#ifdef BOUNDARY
struct blob;
struct blob *blob_open(const char *path);
void blob_close(struct blob *b);

int main(void) {
  // BOUNDARY-NOT: warning:
  // BOUNDARY: blob_close
  // BOUNDARY-NOT: warning:
  struct blob *b = blob_open("x");
  blob_close(b);
  node_free(node_new());
  return 0;
}
#else
int main(void) {
  struct node *n = node_new();
  if (!n)
    return 1;
  int *p = node_vp(n);
  node_free(n);
  // LINK: rfc0005-weavec-cc.c:[[@LINE+1]]:3: error: 'n' is freed twice [weavec::double-free]
  node_free(n);
  // `node_vp` returns a copy of `n` at the field `v` (RFC 0011).
  // LINK: rfc0005-weavec-cc.c:[[@LINE+1]]:11: error: use of 'p' after it was freed [weavec::use-after-free]
  return *p;
}
#endif

// STALE: weavec-cc: warning: link input '{{.*}}main.o' has a stale WeaveC record ('{{.*}}main.o.weavec': it describes another object (digest mismatch)); calls into it are trusted [weavec::unanalyzed-input]
// STALE-NOT: error:
