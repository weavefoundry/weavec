// RUN: %weavec --analysis-stats=%t.stats.json %s --
// RUN: FileCheck %s --check-prefix=STATS < %t.stats.json
// RUN: not %weavec --analysis-stats=%t.stats.json/unwritable %s -- 2>&1 | FileCheck %s --check-prefix=OUTPUT
// RUN: not %weavec --analysis-stats= %s -- 2>&1 | FileCheck %s --check-prefix=EMPTY
// OUTPUT: weavec: error: cannot write analysis statistics '{{.*}}.stats.json/unwritable'
// EMPTY: weavec: error: analysis statistics require a path
// STATS: "version":1
// STATS-SAME: "unit_parses":1
// STATS-SAME: "final":true
// RFC 0020: work counts describe actual execution; explicit output errors fail.
// The object engine (RFC 0031 §12) does not yet record its own counters
// (block transfers, joins, materialisations), so only the driver's count of
// parsed units is pinned; the old engine's CFG and per-function counters are
// gone with it (RFC 0031 §10).
int main(void) { return 0; }
