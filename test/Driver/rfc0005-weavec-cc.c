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
// RUN: not %weavec_cc -fweavec-link=analyze %t/node.o %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=LINK %s
// RUN: not ls %t/prog
//
// A one-step build reports the same and produces nothing.
// RUN: not %weavec_cc -fweavec-link=analyze %S/../WholeProgram/Inputs/node.c %s -o %t/prog1 -I%S/../WholeProgram/Inputs 2>&1 | FileCheck --check-prefix=LINK %s
// RUN: not ls %t/prog1
//
// -fweavec-link=none skips the whole-program step; the objects link as usual.
// RUN: %weavec_cc -fweavec-link=none %t/node.o %t/main.o -o %t/prog2 2>&1 | count 0
// RUN: ls %t/prog2
//
// A callee no unit defines is unknown code at the link step too: RFC 0030
// §5.1 records the calls as ledger rows and reports nothing, and the linker
// fails on the undefined symbols.
// RUN: %weavec_cc -c %s -o %t/bnd.o -I%S/../WholeProgram/Inputs -DBOUNDARY 2>&1 | count 0
// RUN: %weavec --dump-record=%t/bnd.o.weavec | FileCheck --check-prefix=DEFERRED %s
// RUN: not %weavec_cc -fweavec-link=analyze %t/node.o %t/bnd.o -o %t/prog3 2>&1 | FileCheck --check-prefix=BOUNDARY %s
//
// A record written for another object is stale (RFC 0030 §13.1): here the
// object is rebuilt without WeaveC, which leaves the old record behind. The
// input is named in the link's `unanalyzed-input` warning (§13.2), and the
// object is unknown code, so nothing is checked and the link goes ahead.
// RUN: %weavec_cc -fno-weavec -c %s -o %t/main.o -I%S/../WholeProgram/Inputs
// RUN: %weavec_cc -fweavec-link=analyze %t/node.o %t/main.o -o %t/prog4 2>&1 | FileCheck --check-prefix=STALE %s
// RUN: ls %t/prog4
#include "../Inputs/prelude.h"
#include "node.h"

// The record is format 30: each function's summary is its format-30 text in
// the field `effects` (RFC 0031 *Implementation amendments*, "The unit
// record" and "Summary format 30").
// RECORD: "format": 31,
// RECORD: "source": "{{.*}}node.c",
// RECORD-NEXT: "cwd": "{{.+}}",
// RECORD-NEXT: "command": [
// RECORD-NEXT: "-triple",
// RECORD: "-emit-obj",
// RECORD: "object": {
// RECORD-NEXT: "path": "{{.*}}node.o",
// RECORD-NEXT: "digest": "sha256:{{[0-9a-f]+}}"
// RECORD: "functions": [
// `node_free` releases `n` and `n->name` where `n` is not null.
// RECORD: "name": "node_free",
// RECORD-NEXT: "linkage": "external",
// RECORD-NEXT: "addressTaken": false,
// RECORD-NEXT: "typeKey": "void (struct node *)",
// RECORD-NEXT: "effects": "returns always\neffect release p0* when=-:0!=0 family=free\neffect release p0*.name* when=-:0!=0 family=free\nreads p0*\n",
// `node_new` returns a fresh `free` allocation or null.
// RECORD: "name": "node_new",
// RECORD-NEXT: "linkage": "external",
// RECORD-NEXT: "addressTaken": false,
// RECORD-NEXT: "typeKey": "struct node *(void)",
// RECORD-NEXT: "effects": "returns always\n{{.*}}result classes=null,nonnull :: fresh family=free extent=16{{.*}}\n",
// RECORD: "name": "node_set_name",
// RECORD: "typeKey": "void (struct node *, char *)",
// RECORD-NEXT: "effects": "returns always\neffect release p0*.name* when=-:- family=free\nstore p0*.name when=-:- :: path path=p1 offset=0{{.*}}\n",
// `node_vp` returns `n` itself, at the offset of `v` (0; RFC 0011).
// RECORD: "name": "node_vp",
// RECORD: "typeKey": "int *(struct node *)",
// RECORD-NEXT: "effects": "returns always\nresult classes=nonnull :: path path=p0 offset=0\nnonnull-on nonnull p0\n",
// RECORD: "imports": [
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
// MAIN: "reported": [],

// The callees no unit defines are recorded as imports with their calls;
// the link step finds no definition for them. (The record's `unknown` list
// is no longer filled by the object engine.)
// DEFERRED: "imports": [
// DEFERRED-NEXT: {
// DEFERRED-NEXT: "name": "blob_close",
// DEFERRED: "calls": [
// DEFERRED-NEXT: {
// DEFERRED-NEXT: "function": "main",
// DEFERRED: "name": "blob_open",
// DEFERRED: "calls": [
// DEFERRED-NEXT: {
// DEFERRED-NEXT: "function": "main",

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
  // `node_free` reads `n->name` before it frees `n`, so the first violation
  // of the second call is that read, a use after free (RFC 0031 §6.3).
  // LINK: rfc0005-weavec-cc.c:[[@LINE+1]]:13: error: use of 'n' after it was freed [weavec::use-after-free]
  node_free(n);
  // `node_vp` returns a copy of `n` at the field `v` (RFC 0011).
  // LINK: rfc0005-weavec-cc.c:[[@LINE+1]]:11: error: use of 'p' after it was freed [weavec::use-after-free]
  return *p;
}
#endif

// STALE: weavec-cc: warning: link input '{{.*}}main.o' has a stale WeaveC record ('{{.*}}main.o.weavec': it describes another object (digest mismatch)); calls into it are trusted [weavec::unanalyzed-input]
// STALE-NOT: error:
