// RFC 0035 §8: `weavec --whole-program` prints one summary line per unit,
// from its last run, and one for the program, named after the unit that
// defines `main`, whatever order the units are given in.
//
// RUN: %weavec --whole-program %s %S/Inputs/rfc0030-first.c -- 2>&1 | FileCheck %s
// RUN: %weavec --whole-program %S/Inputs/rfc0030-first.c %s -- 2>&1 | FileCheck %s
// RUN: %weavec --whole-program %S/Inputs/rfc0030-first.c -- 2>&1 | FileCheck --check-prefix=LIBRARY %s

int first(int *p);

int main(void) {
  int a[4] = {1, 2, 3, 4};
  return first(a + 1) - 2;
}

// CHECK-DAG: weavec: {{.*}}program-summary.c: {{[0-9]+}} sites: {{[0-9]+}} proven, {{[0-9]+}} not proven, 0 violations, {{[0-9]+}} trusted; 0 errors, 0 warnings
// CHECK-DAG: weavec: {{.*}}rfc0030-first.c: 2 sites: {{[0-9]+}} proven, {{[0-9]+}} not proven, 0 violations, 0 trusted; 0 errors, 0 warnings
// CHECK: weavec: program program-summary: {{[0-9]+}} sites in 2 units: {{[0-9]+}} proven, {{[0-9]+}} not proven, 0 violations, {{[0-9]+}} trusted; 0 errors, 0 warnings

// LIBRARY: weavec: program program: 2 sites in 1 unit: {{.*}}; 0 errors, 0 warnings
