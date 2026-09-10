// RUN: split-file %s %t
// RUN: %weavec --whole-program --checked %t/suffix.c %t/good.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --whole-program --checked --analysis-cache=%t/cache --analysis-stats=%t/cold.json %t/suffix.c %t/good.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --whole-program --checked --analysis-cache=%t/cache --analysis-stats=%t/warm.json %t/suffix.c %t/good.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: FileCheck %s --check-prefix=WARM --implicit-check-not='"function_analyses":' --implicit-check-not='"cache_write_failures":' < %t/warm.json
// RUN: not %weavec --whole-program --checked %t/suffix.c %t/bad.c -- 2>&1 | FileCheck %s --check-prefix=BAD
// RFC 0021: builtin string requirements at an advanced pointer retain the
// minimum witness index and the reserved zero end field through export/cache.
// An earlier zero does not satisfy a suffix's later terminator requirement.
// CLEAN-NOT: checking-incomplete
// CLEAN-NOT: checking-failed
// WARM: "cache_hits":2
// BAD: callee terminated safety precondition must hold [weavec::checking-incomplete]
// BAD: error: checked safety requirements were not established

//--- suffix.c
unsigned long strlen(const char *);
unsigned long suffix(const char *p) { return strlen(p + 2); }

//--- good.c
unsigned long suffix(const char *);
int main(void) {
  char input[] = {'a', 0, 'b', 0};
  input[3] = 0;
  return suffix(input) != 1;
}

//--- bad.c
unsigned long suffix(const char *);
int main(void) {
  char input[] = {'a', 0, 'b'};
  return suffix(input) != 1;
}
