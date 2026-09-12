// RUN: split-file %s %t
// RUN: %weavec --whole-program --checked-function=main %t/good.c %t/output.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --whole-program --checked-function=main %t/bad.c %t/output.c -- 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: %weavec --whole-program --analysis-cache=%t/cache --checked-function=main %t/good.c %t/output.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --whole-program --analysis-cache=%t/cache --checked-function=main %t/bad.c %t/output.c -- 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: %weavec_cc -c %t/output.c -o %t/output.o
// RUN: %weavec_cc -c %t/bad.c -o %t/bad.o
// RUN: not %weavec_cc -fweavec-checked-function=main %t/bad.o %t/output.o -o %t/client 2>&1 | FileCheck %s --check-prefix=BAD
// RFC 0024: an implicit stream remains a precondition across source and objects.
// CLEAN-NOT: error:
// BAD: error: cannot establish checked safety: implicit output requires a live standard stream [weavec::checking-incomplete]

//--- output.c
// A valid declaration need not include the platform's stream representation.
int puts(const char *);
void output(void) { puts("ok"); }

//--- good.c
void output(void);
int main(void) { output(); return 0; }

//--- bad.c
#include <stdio.h>
void output(void);
int main(void) { fclose(stdout); output(); return 0; }
