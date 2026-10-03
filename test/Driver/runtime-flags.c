// RFC 0032, sections 1, 7 and 10: with the runtime (the default) a facet the analysis leaves
// unresolved and a guard covers is `guarded`; without it the facet stays unresolved. The
// ledger records which, a link of both kinds of unit records "mixed", and the require levels
// treat a guarded facet as their names say.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %s -o %t/on.o -fweavec-ledger=%t/on.json 2>&1 | FileCheck --check-prefix=ON %s
// RUN: FileCheck --check-prefix=ON-LEDGER %s < %t/on.json
// RUN: %weavec_cc -c -fno-weavec-runtime %s -o %t/off.o -fweavec-ledger=%t/off.json 2>&1 | FileCheck --check-prefix=OFF %s
// RUN: FileCheck --check-prefix=OFF-LEDGER %s < %t/off.json
//
// RUN: %weavec_cc -c -fweavec-require=guarded %s -o %t/guarded.o
// RUN: not %weavec_cc -c -fweavec-require=checked %s -o %t/checked.o 2>&1 | FileCheck --check-prefix=CHECKED %s
// RUN: not %weavec_cc -c -fno-weavec-runtime -fweavec-require=guarded %s -o %t/unguarded.o 2>&1 | FileCheck --check-prefix=UNGUARDED %s
//
// RUN: %weavec_cc -c -fno-weavec-runtime %S/Inputs/runtime-other.c -o %t/other.o
// RUN: %weavec_cc %t/on.o %t/other.o -o %t/prog -fweavec-ledger=%t/program.json
// RUN: FileCheck --check-prefix=MIXED %s < %t/program.json
//
// A link without the runtime leaves the guards of its units without an allocator to ask:
// the link says so, and the program's guards read "guardable (not enforced)".
// RUN: %weavec_cc -fno-weavec-runtime %t/on.o -o %t/unlinked -fweavec-ledger=%t/unlinked.json 2>&1 | FileCheck --check-prefix=UNLINKED %s
// RUN: FileCheck --check-prefix=UNLINKED-LEDGER %s < %t/unlinked.json
//
// The tool enforces nothing, so its guarded facets read "guardable"; --no-runtime leaves
// them unresolved.
// RUN: %weavec %s -- 2>&1 | FileCheck --check-prefix=TOOL %s
// RUN: %weavec --no-runtime %s -- 2>&1 | FileCheck --check-prefix=TOOL-OFF %s

struct vec {
  int *data;
  unsigned long n;
};

int at(struct vec *v, unsigned long i) { return v->data[i]; }

int main(void) { return 0; }

// ON: weavec: {{.*}}runtime-flags.c: 4 sites: 2 proven, 1 checked, 1 guarded, 0 unresolved, 0 trusted; 0 errors, 0 warnings
// ON-LEDGER: "version": 2,
// ON-LEDGER: "config": {
// ON-LEDGER-NEXT: "checks": "trap",
// ON-LEDGER-NEXT: "runtime": true,
// ON-LEDGER: "guardedReasons": {
// ON-LEDGER-NEXT: "unknown-extent": 1,
// ON-LEDGER: "guardedShare": {
// ON-LEDGER-NEXT: "spatial": 0.5,
// ON-LEDGER-NEXT: "null": 0,
// ON-LEDGER-NEXT: "temporal": 0
// ON-LEDGER: "outcome": "guarded",
// ON-LEDGER-NEXT: "reason": "unknown-extent",
// ON-LEDGER: "template": "object"

// OFF: weavec: {{.*}}runtime-flags.c: 4 sites: 2 proven, 1 checked, 0 guardable (not enforced), 1 unresolved, 0 trusted; 0 errors, 0 warnings
// OFF-LEDGER: "checks": "trap",
// OFF-LEDGER-NEXT: "runtime": false,
// OFF-LEDGER-NOT: "outcome": "guarded"

// CHECKED: runtime-flags.c:[[#@LINE-24]]:49: error: access 'v->data[i]' is guarded at run time only: the extent of 'v->data' is unknown [unknown-extent] [weavec::unresolved-operation]
// UNGUARDED: runtime-flags.c:[[#@LINE-25]]:49: error: access 'v->data[i]' is neither proven nor checkable: the extent of 'v->data' is unknown [unknown-extent] [weavec::unresolved-operation]

// MIXED: "checks": "trap",
// MIXED-NEXT: "runtime": "mixed",

// UNLINKED: weavec-cc: note: linking without the WeaveC runtime, but '{{.*}}on.o' was compiled with it: the heap is untracked, its guards pass on it and releases are not validated (RFC 0032)
// UNLINKED: weavec: program unlinked: 4 sites in 1 unit: 2 proven, 1 checked, 1 guardable (not enforced), 0 unresolved, 0 trusted;
// UNLINKED-LEDGER: "checks": "trap",
// UNLINKED-LEDGER-NEXT: "runtime": false,

// TOOL: 1 guardable (not enforced), 0 unresolved
// TOOL-OFF: 0 guardable (not enforced), 1 unresolved
