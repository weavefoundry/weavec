// RFC 0020: ordinary AST retention is bounded. Checked selections and
// persistent checkpoints keep their original AST/input binding lifetimes.
// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: %weavec --whole-program --analysis-stats=%t/ordinary.json %t/main.c %t/f*.c --
// RUN: FileCheck %s --check-prefix=EVICT < %t/ordinary.json
// RUN: %weavec --whole-program --checked --analysis-stats=%t/checked.json %t/main.c %t/f*.c --
// RUN: FileCheck %s --check-prefix=KEEP --implicit-check-not=unit_evictions < %t/checked.json
// RUN: %weavec --whole-program --analysis-stats=%t/annotated.json %t/main.c %t/f*.c -- -DCHECKED_LEAF
// RUN: FileCheck %s --check-prefix=KEEP --implicit-check-not=unit_evictions < %t/annotated.json
// RUN: %weavec --whole-program --analysis-cache=%t/cache --analysis-stats=%t/cached.json %t/main.c %t/f*.c --
// RUN: FileCheck %s --check-prefix=KEEP --implicit-check-not=unit_evictions < %t/cached.json
// RUN: %weavec_cc -fweavec-analysis-stats=%t/compiler.json %t/main.c %t/f*.c -o %t/program
// RUN: FileCheck %s --check-prefix=EVICT < %t/compiler.json

// EVICT: "unit_evictions":{{[1-9][0-9]*}},
// KEEP: "unit_parses":8{{[,}]}}

//--- main.c
void f1(void);
int main(void) { f1(); return 0; }

//--- f1.c
void f7(void);
void f1(void) { f7(); }

//--- f2.c
void f7(void);
void f2(void) { f7(); }

//--- f3.c
void f7(void);
void f3(void) { f7(); }

//--- f4.c
void f7(void);
void f4(void) { f7(); }

//--- f5.c
void f7(void);
void f5(void) { f7(); }

//--- f6.c
void f7(void);
void f6(void) { f7(); }

//--- f7.c
#ifdef CHECKED_LEAF
__attribute__((annotate("weavec.checked")))
#endif
void f7(void) {}
