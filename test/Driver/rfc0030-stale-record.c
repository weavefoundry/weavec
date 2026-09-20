// RFC 0030 §13.1: a reader accepts only a format-28 record with this
// schema's fingerprint and a valid digest, written for the object next to
// it. Anything else is a stale record: the link names the input in its one
// `unanalyzed-input` warning with the reason, treats the object as unknown
// code and goes ahead. There is no legacy reader: the RFC 0005 text sidecar
// is not a record. `weavec --dump-record` prints a record, or why it is
// stale.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %s -o %t/main.o
// RUN: %weavec_cc -c %s -DUNIT_B -o %t/b.o
// RUN: cp %t/b.o.weavec %t/good.weavec
// RUN: %weavec --dump-record=%t/b.o.weavec | FileCheck --check-prefix=DUMP %s
//
// A byte of the header changed: the digest no longer matches.
// RUN: printf 'X' | dd of=%t/b.o.weavec bs=1 seek=100 conv=notrunc 2>/dev/null
// RUN: %weavec_cc %t/main.o %t/b.o -o %t/p1 2>&1 | FileCheck --check-prefix=DIGEST %s
// RUN: not %weavec --dump-record=%t/b.o.weavec 2>&1 | FileCheck --check-prefix=DUMP-DIGEST %s
//
// Another schema: byte 16 is the first of the schema fingerprint.
// RUN: cp %t/good.weavec %t/b.o.weavec
// RUN: printf 'Z' | dd of=%t/b.o.weavec bs=1 seek=16 conv=notrunc 2>/dev/null
// RUN: %weavec_cc %t/main.o %t/b.o -o %t/p2 2>&1 | FileCheck --check-prefix=SCHEMA %s
//
// Another format: byte 8 is the low byte of the format.
// RUN: cp %t/good.weavec %t/b.o.weavec
// RUN: printf '\033' | dd of=%t/b.o.weavec bs=1 seek=8 conv=notrunc 2>/dev/null
// RUN: %weavec_cc %t/main.o %t/b.o -o %t/p3 2>&1 | FileCheck --check-prefix=FORMAT %s
//
// A truncated record.
// RUN: head -c 100 %t/good.weavec > %t/b.o.weavec
// RUN: %weavec_cc %t/main.o %t/b.o -o %t/p4 2>&1 | FileCheck --check-prefix=TRUNCATED %s
//
// The text sidecar of RFC 0005 is not a record.
// RUN: echo "weavec-summaries 28" > %t/b.o.weavec
// RUN: %weavec_cc %t/main.o %t/b.o -o %t/p5 2>&1 | FileCheck --check-prefix=MAGIC %s
//
// The record again: nothing is named, and the program runs.
// RUN: cp %t/good.weavec %t/b.o.weavec
// RUN: %weavec_cc %t/main.o %t/b.o -o %t/p6 2>&1 | count 0
// RUN: %t/p6

// DUMP: {
// DUMP-NEXT: "format": 28,
// DUMP-NEXT: "header": {
// DUMP: "object": {
// DUMP-NEXT: "path": "{{.*}}b.o",
// DUMP-NEXT: "digest": "sha256:{{[0-9a-f]+}}"
// DUMP: "payload": {
// DUMP-NEXT: "functions": [
// DUMP-NEXT: {
// DUMP-NEXT: "name": "helper",
// DUMP: "definesAllocator": false,
// DUMP-NEXT: "a5": {

// DIGEST: weavec-cc: warning: link input '{{.*}}b.o' has a stale WeaveC record ('{{.*}}b.o.weavec': digest mismatch); calls into it are trusted [weavec::unanalyzed-input]
// DUMP-DIGEST: weavec: error: '{{.*}}b.o.weavec' is a stale WeaveC record (digest mismatch)
// SCHEMA: weavec-cc: warning: link input '{{.*}}b.o' has a stale WeaveC record ('{{.*}}b.o.weavec': schema fingerprint mismatch (written by another WeaveC)); calls into it are trusted [weavec::unanalyzed-input]
// FORMAT: weavec-cc: warning: link input '{{.*}}b.o' has a stale WeaveC record ('{{.*}}b.o.weavec': format 27, expected 28); calls into it are trusted [weavec::unanalyzed-input]
// TRUNCATED: weavec-cc: warning: link input '{{.*}}b.o' has a stale WeaveC record ('{{.*}}b.o.weavec': length mismatch {{.*}}); calls into it are trusted [weavec::unanalyzed-input]
// MAGIC: weavec-cc: warning: link input '{{.*}}b.o' has a stale WeaveC record ('{{.*}}b.o.weavec': not a WeaveC record (bad magic)); calls into it are trusted [weavec::unanalyzed-input]

#if defined(UNIT_B)
int helper(int v) { return v; }
#else
int helper(int v);
int main(void) { return helper(0); }
#endif
