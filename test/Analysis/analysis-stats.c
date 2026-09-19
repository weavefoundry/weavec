// RUN: %weavec --analysis-stats=%t.stats.json %s --
// RUN: FileCheck %s --check-prefix=STATS < %t.stats.json
// RUN: not %weavec --analysis-stats=%t.stats.json/unwritable %s -- 2>&1 | FileCheck %s --check-prefix=OUTPUT
// RUN: not %weavec --analysis-stats= %s -- 2>&1 | FileCheck %s --check-prefix=EMPTY
// OUTPUT: weavec: error: cannot write analysis statistics '{{.*}}.stats.json/unwritable'
// EMPTY: weavec: error: analysis statistics require a path
// STATS: "version":1
// STATS-SAME: "cfg_builds":1,
// STATS-SAME: "cfg_reuses":1,
// STATS-SAME: "function:{{[^"]+}}#main":2,
// STATS-SAME: "function_analyses":2,
// STATS-SAME: "final":true
// RFC 0020: work counts describe actual execution; explicit output errors fail.
int main(void) { return 0; }
