// RFC 0041 §5: an escaping parameter of a safe function must not point into
// the stack (`stack-escape`); §6: a safe function's local whose address
// escapes is placed on the heap, which a remark says.
// RUN: %weavec_cc -O2 -g %s -o %t -Rpass=weavec-safe 2>&1 | FileCheck %s --check-prefix=REMARK
// RUN: %t
// RUN: not --crash %t stack 2>&1 | FileCheck %s
// RUN: not --crash %t dangling 2>&1 | FileCheck %s --check-prefix=DANGLING
#include <weavec.h>
#include <string.h>

struct node { struct node *next; int v; };
static struct node *head;

// CHECK: weavec: stack-escape at {{.*}}safe-escape.c:[[@LINE+1]]
WEAVEC_SAFE void push(struct node *WEAVEC_ESCAPES n) {
  n->next = head;
  head = n;
}

WEAVEC_SAFE int boxed(int x) {
  // REMARK: safe-escape.c:[[@LINE+1]]:{{[0-9]+}}: remark: local 'local' is placed on the heap: its address may outlive its scope
  struct node local = {0, x};
  push(&local);
  return head->v;
}

// A local that stays borrowed stays in the frame.
WEAVEC_SAFE int peek(const struct node *n) { return n->v; }
WEAVEC_SAFE int framed(int x) {
  // REMARK-NOT: local 'kept'
  struct node kept = {0, x};
  return peek(&kept);
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "stack")) {
    struct node mine;
    push(&mine);
  }
  const int ok = boxed(7) == 7 && framed(3) == 3;
  // The heap-placed local was released when boxed() returned: guarded code
  // that reads it through the pointer push() kept reports it.
  // DANGLING: weavec: heap-use-after-free at {{.*}}safe-escape.c:[[@LINE+2]]
  if (argc > 1 && !strcmp(argv[1], "dangling"))
    return head->v;
  return ok ? 0 : 1;
}
