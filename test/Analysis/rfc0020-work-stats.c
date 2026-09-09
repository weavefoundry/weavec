// RUN: %weavec --checked --analysis-stats=%t.stats.json %s --
// RUN: FileCheck %s --check-prefix=STATS < %t.stats.json
// RUN: not %weavec --analysis-stats=%t.stats.json/unwritable %s -- 2>&1 | FileCheck %s --check-prefix=OUTPUT
// RUN: not %weavec --analysis-stats= %s -- 2>&1 | FileCheck %s --check-prefix=EMPTY
// RUN: not %weavec --checked-report-format=unknown %s -- 2>&1 | FileCheck %s --check-prefix=FORMAT
// RUN: %weavec --dump-analysis %s -- > %t.fresh
// RUN: %weavec --dump-analysis --analysis-cache=%t.cache %s -- > %t.cached
// RUN: diff %t.fresh %t.cached
// OUTPUT: weavec: error: cannot write analysis statistics '{{.*}}.stats.json/unwritable'
// EMPTY: weavec: error: analysis statistics and cache options require a path
// FORMAT: weavec: error: checked report format must be expanded or compact
// STATS: "version":1
// STATS-SAME: "cfg_builds":1,
// STATS-SAME: "cfg_order_builds":1,
// STATS-SAME: "cfg_order_reuses":1,
// STATS-SAME: "cfg_reuses":1,
// STATS-SAME: "function:{{[^"]+}}#main":2,
// STATS-SAME: "function_analyses":2,
// STATS-SAME: "final":true
// RFC 0020: work counts describe actual execution; explicit output errors fail.
int main(void) { return 0; }
