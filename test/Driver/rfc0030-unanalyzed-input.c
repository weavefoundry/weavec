// RFC 0030 §13.2, *Diagnostics*: the link step no longer skips inputs
// silently. Every input that is not the platform's and has no valid WeaveC
// record (an object, an archive, a library `-l` finds, a stale record) is
// named in one `unanalyzed-input` warning per link; the analysis of the
// inputs with records is unchanged, and the link goes ahead.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %s -o %t/main.o
// RUN: %weavec_cc -c %s -DUNIT_B -o %t/bw.o
// RUN: %weavec_cc -c %s -DUNIT_C -o %t/cw.o
// RUN: %clang -c %s -DUNIT_B -o %t/b.o
// RUN: %clang -c %s -DUNIT_C -o %t/c.o
// RUN: llvm-ar rcs %t/libextra.a %t/c.o
//
// One input without a record: the message names it.
// RUN: %weavec_cc %t/main.o %t/b.o %t/cw.o -o %t/one 2>&1 | FileCheck --check-prefix=ONE %s
// RUN: %t/one
//
// Several: one warning, with a note per input.
// RUN: %weavec_cc %t/main.o %t/b.o %t/libextra.a -o %t/many 2>&1 | FileCheck --check-prefix=MANY %s
// RUN: %t/many
//
// `-l` is resolved as the linker resolves it; a library of the platform
// (`-lm`) is not named, one found through `-L` is.
// RUN: %weavec_cc %t/main.o %t/bw.o -L%t -lextra -lm -o %t/lib 2>&1 | FileCheck --check-prefix=LIB %s
// RUN: %weavec_cc %t/main.o %t/bw.o %t/cw.o -lm -o %t/system 2>&1 | count 0
//
// A record written for another object is stale (§13.1): rebuilding the
// object without WeaveC (and with another value) leaves the old record
// behind. rfc0030-stale-record.c has the other reasons.
// RUN: %weavec_cc -fno-weavec -c %s -DUNIT_C -DOTHER=8 -o %t/cw.o
// RUN: %weavec_cc %t/main.o %t/bw.o %t/cw.o -o %t/stale 2>&1 | FileCheck --check-prefix=STALE %s
//
// The warning follows the -W flags: it can be disabled, or made an error
// that stops the link.
// RUN: %weavec_cc -Wno-weavec-unanalyzed-input %t/main.o %t/b.o %t/libextra.a -o %t/quiet 2>&1 | count 0
// RUN: not %weavec_cc -Werror=weavec-unanalyzed-input %t/main.o %t/b.o %t/libextra.a -o %t/raised 2>&1 | FileCheck --check-prefix=RAISED %s
// RUN: not ls %t/raised
// RUN: %weavec_cc -fno-weavec-link %t/main.o %t/b.o %t/libextra.a -o %t/nolink 2>&1 | count 0

// ONE: weavec-cc: warning: link input '{{.*}}b.o' has no WeaveC record; calls into it are trusted [weavec::unanalyzed-input]
// ONE-NOT: warning:

// MANY: weavec-cc: warning: 2 link inputs have no WeaveC record; calls into them are trusted [weavec::unanalyzed-input]
// MANY-NEXT: weavec-cc: note: link input '{{.*}}b.o' has no WeaveC record
// MANY-NEXT: weavec-cc: note: link input '{{.*}}libextra.a' has no WeaveC record
// MANY-NOT: warning:

// LIB: weavec-cc: warning: link input '{{.*}}libextra.a' has no WeaveC record; calls into it are trusted [weavec::unanalyzed-input]
// LIB-NOT: libm

// STALE: weavec-cc: warning: link input '{{.*}}cw.o' has a stale WeaveC record ('{{.*}}cw.o.weavec': it describes another object (digest mismatch)); calls into it are trusted [weavec::unanalyzed-input]

// RAISED: weavec-cc: error: 2 link inputs have no WeaveC record; calls into them are trusted [weavec::unanalyzed-input]

// No pointer crosses the units, so no boundary is reported beside it.
#if defined(UNIT_B)
int helper(int v) { return v; }
#elif defined(UNIT_C)
#ifndef OTHER
#define OTHER 7
#endif
int other(void) { return OTHER; }
#else
int helper(int v);
int other(void);

int main(void) { return helper(1) + other() - 8; }
#endif
