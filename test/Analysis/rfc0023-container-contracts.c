// RUN: split-file %s %t
// RUN: %weavec --checked-function=main %t/good.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=main %t/bad.c -- -Wno-everything 2>&1 | FileCheck %s --check-prefix=BAD
// RFC 0023: a closed caller must establish the complete finite-chain premise.
// CLEAN-NOT: error:
// BAD: error: cannot establish checked safety: callee container chain precondition must hold [weavec::checking-incomplete]
// BAD: error: checked safety requirements were not established

//--- good.c
struct node { unsigned value; struct node *next; };
static unsigned count(const struct node *p) {
  unsigned n = 0;
  while (p) { ++n; p = p->next; }
  return n;
}
int main(void) {
  struct node last = {2, 0}, first = {1, &last};
  return count(&first) != 2;
}

//--- bad.c
struct node { unsigned value; struct node *next; };
static unsigned count(const struct node *p) {
  unsigned n = 0;
  while (p) { ++n; p = p->next; }
  return n;
}
int main(void) {
  struct node last = {2, 0}, first = {1, &last};
  last.next = &first;
  return count(&first) != 2;
}
