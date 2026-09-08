// RUN: split-file %s %t
// RUN: %weavec_cc -fweavec-checked -c %t/setter.c -o %t/setter.o 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec_cc -fweavec-checked-function=main -c %t/main.c -o %t/main.o 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec_cc %t/setter.o %t/main.o -o %t/program 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RFC 0019: private output postconditions stay inside their defining unit.
// The caller requires the setter's complete checked contract at link time.
// CLEAN-NOT: checking-incomplete
// CLEAN-NOT: checking-failed

//--- setter.c
static struct { int level; _Bool quiet; } settings;
void set_level(int level) { settings.level = level; }
void set_quiet(_Bool quiet) { settings.quiet = quiet; }

//--- main.c
void set_level(int);
void set_quiet(_Bool);
int main(void) { set_level(2); set_quiet(1); return 0; }
