// RFC 0032, section 5: every global object a unit defines gets a descriptor {address, size}
// in the section the runtime's constructor reads, after the unit's own declarations. An
// object the unit only declares is described by the unit that defines it. (A static local
// gets one too; C cannot name it from the expected file, so the case
// semantics/runtime/global-static-local_bug.c covers it.)
// The -O0 IR equals that of Inputs/runtime-oracle-global-objects.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/runtime-oracle-global-objects.expected.c %t -- -fweavec-runtime -fno-weavec-stack-objects -Wno-weavec

extern int Elsewhere[4];
int Table[8];
static char Name[5] = "name";

char *name(void) { return Name; }
int *table(void) { return Table; }
int *elsewhere(void) { return Elsewhere; }
