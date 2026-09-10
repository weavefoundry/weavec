// RUN: split-file %s %t
// RUN: %weavec --whole-program --checked-function=main %t/helpers.c %t/compact.c %t/main.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --whole-program --checked-function=main --analysis-cache=%t/cache --analysis-stats=%t/cold.json %t/helpers.c %t/compact.c %t/main.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --whole-program --checked-function=main --analysis-cache=%t/cache --analysis-stats=%t/warm.json %t/helpers.c %t/compact.c %t/main.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: FileCheck %s --check-prefix=WARM --implicit-check-not='"function_analyses":' < %t/warm.json
// RUN: %weavec_cc -c %t/helpers.c -o %t/helpers.o
// RUN: %weavec_cc -c %t/compact.c -o %t/compact.o
// RUN: %weavec_cc -fweavec-checked-function=main -c %t/main.c -o %t/main.o
// RUN: %weavec_cc %t/helpers.o %t/compact.o %t/main.o -o %t/program 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --whole-program --checked-function=main -Wno-error=weavec %t/helpers.c %t/compact.c %t/bad.c -- 2>&1 | FileCheck %s --check-prefix=BAD
// RFC 0021: entry witnesses, paired progress and ordered overlapping cursors
// cross source units, serialized compiler objects and persistent checkpoints.
// CLEAN-NOT: checking-incomplete
// CLEAN-NOT: checking-failed
// WARM: "cache_hits":3
// BAD: callee terminated safety precondition must hold [weavec::checking-incomplete]
// BAD: error: checked safety requirements were not established

//--- helpers.c
void skip(char **p) { while (**p && **p == ' ') ++*p; }
void copy(char **p, char **out) {
  while (**p && **p != ' ') { **out = **p; ++*p; ++*out; }
}

//--- compact.c
void skip(char **);
void copy(char **, char **);
void compact(char *p) {
  char *out = p;
  while (*p) {
    if (*p == ' ') skip(&p);
    else copy(&p, &out);
  }
  *out = 0;
}

//--- main.c
void compact(char *);
int main(void) { char a[] = "ab cd ef"; compact(a); return a[0] != 'a'; }

//--- bad.c
void compact(char *);
int main(void) { char a[3] = {'a','b','c'}; compact(a); return 0; }
