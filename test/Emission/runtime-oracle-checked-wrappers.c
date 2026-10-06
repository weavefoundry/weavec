// RFC 0034, section 5.2: a library call whose need no term can state calls
// its row's checked wrapper instead, which computes the need at run time:
// sprintf's output against the room behind its destination, a string copy's
// length against the room behind the destination and its operands'
// disjointness, a memcpy's disjointness. After the row's arguments the
// wrapper takes the object size of a fortified call (the maximum here) and
// the checks planned (1 room, 2 disjoint); a check the plan states keeps
// its own guard.
// The -O0 IR equals that of Inputs/runtime-oracle-checked-wrappers.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/runtime-oracle-checked-wrappers.expected.c %t -- -fweavec-runtime -fno-weavec-global-objects -Wno-weavec

extern char *strcpy(char *, const char *);
extern void *memcpy(void *, const void *, unsigned long);
extern int sprintf(char *, const char *, ...);
extern int vsprintf(char *, const char *, __builtin_va_list);
extern const char *name(void);

int show(char *out, int port) { return sprintf(out, "%d", port); }
void copy(char *d) { strcpy(d, name()); }
void shift(char *p) { strcpy(p, p + 2); }
void slide(char *a, unsigned long n) { memcpy(a, a + 1, n); }
int vshow(char *out, __builtin_va_list ap) { return vsprintf(out, "%d", ap); }
// A fortified call passes its object size, and drops its flag.
void fortified(char *d, int port) {
  __builtin___strcpy_chk(d, name(), __builtin_object_size(d, 1));
  __builtin___sprintf_chk(d, 0, __builtin_object_size(d, 1), "%d", port);
}
