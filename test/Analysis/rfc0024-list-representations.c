// RUN: split-file %s %t
// RUN: %weavec --checked-function=main %t/good.c -- --target=arm64-apple-macosx 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --checked-function=main %t/good.c -- --target=x86_64-unknown-linux-gnu 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --checked-function=main %t/good.c -- --target=aarch64-unknown-linux-gnu 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=main %t/bytes.c -- 2>&1 | FileCheck %s --check-prefix=BYTES
// RUN: not %weavec --checked-function=main %t/scope.c -- 2>&1 | FileCheck %s --check-prefix=SCOPE
// RUN: %weavec --checked-function=main %t/c23.c -- -std=c23 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RFC 0024: pointer, array and record ABIs share opaque lifecycle semantics.
// CLEAN-NOT: error:
// BYTES: error: cannot establish checked safety: variadic traversal requires an active unconsumed argument list [weavec::checking-incomplete]
// SCOPE: error: cannot establish checked safety: locally started or copied argument list requires va_end [weavec::checking-incomplete]

//--- good.c
typedef __builtin_va_list va_list;
int vprintf(const char *, va_list);
static void forward(const char *format, va_list list) { vprintf(format, list); }
static void output(const char *format, ...) {
  va_list a, b;
  __builtin_va_start(a, format);
  __builtin_va_copy(b, a);
  forward(format, a);
  forward(format, b);
  __builtin_va_end(a);
  __builtin_va_end(b);
}
int main(void) { output("%s", "ok"); return 0; }

//--- c23.c
typedef __builtin_va_list va_list;
int vprintf(const char *, va_list);
static void output(const char *format, ...) {
  va_list a;
  __builtin_c23_va_start(a);
  vprintf(format, a);
  __builtin_va_end(a);
}
int main(void) { output("%s", "ok"); return 0; }

//--- bytes.c
typedef __builtin_va_list va_list;
int vprintf(const char *, va_list);
void *memset(void *, int, __SIZE_TYPE__);
static void output(const char *format, ...) {
  va_list a;
  __builtin_va_start(a, format);
  memset(&a, 0, sizeof a);
  vprintf(format, a);
  __builtin_va_end(a);
}
int main(void) { output("%s", "ok"); return 0; }

//--- scope.c
typedef __builtin_va_list va_list;
static void output(const char *format, ...) {
  { va_list a; __builtin_va_start(a, format); }
}
int main(void) { output("%s", "ok"); return 0; }
