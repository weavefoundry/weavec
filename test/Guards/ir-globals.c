// RFC 0035 §4: a global the unit defines is replaced by one with a redzone,
// aligned to 32, and registered by a constructor; string literals are not.
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s

// CHECK: @table = {{(dso_local )?}}global { [4 x i32], [{{[0-9]+}} x i8] } {{.*}}align 32
int table[4] = {1, 2, 3, 4};
// CHECK: @llvm.global_ctors = appending global {{.*}}@__weavec.globals.register
// CHECK-NOT: @.str = {{.*}}{ [

int get(int i) { return table[i]; }
const char *name(void) { return "a literal"; }

// CHECK-LABEL: define internal void @__weavec.globals.register(
// CHECK: call void @__weavec_rt_globals_register(ptr @__weavec.globals, i64
