// RUN: split-file %s %t
// RUN: %weavec --checked-function=main %t/good.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=main %t/bad.c -- 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %weavec --checked-function=main %t/raw-write.c -- 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %weavec --checked-function=main %t/helper-write.c -- 2>&1 | FileCheck %s --check-prefix=BAD
// RFC 0025: matching tags cannot manufacture an initialized member view.
// CLEAN-NOT: error:
// BAD: error: cannot establish checked safety: read requires an initialized compatible union member [weavec::checking-incomplete]

//--- good.c
union value { int number; int *pointer; };
int main(void) {
  union value v = {.number = 7};
  return v.number;
}

//--- bad.c
union value { int number; int *pointer; };
int main(void) {
  int n = 7;
  union value v = {.pointer = &n};
  return v.number;
}

//--- raw-write.c
union value { int number; int other; };
int read(union value *u) {
  ((unsigned char *)u)[0] = 0;
  return u->number;
}
int main(void) {
  union value u = {.number = 7};
  return read(&u);
}

//--- helper-write.c
union value { int number; int other; };
void set(int *p) { *p = 3; }
int read(union value *u) {
  set(&u->other);
  return u->number;
}
int main(void) {
  union value u = {.number = 7};
  return read(&u);
}
