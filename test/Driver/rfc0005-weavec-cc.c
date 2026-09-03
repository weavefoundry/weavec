// RFC 0005, *weavec-cc*: the compile step analyses the unit alone, writes
// the object and a `.weavec` sidecar next to it, and defers boundary
// warnings; the link step reads the sidecars, analyses the program, reports
// what needs two files, and refuses to link on an error.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %S/../WholeProgram/Inputs/node.c -o %t/node.o -I%S/../WholeProgram/Inputs 2>&1 | count 0
// RUN: %weavec_cc -c %s -o %t/main.o -I%S/../WholeProgram/Inputs 2>&1 | count 0
// RUN: FileCheck --check-prefix=SIDECAR %s < %t/node.o.weavec
// RUN: FileCheck --check-prefix=MAIN %s < %t/main.o.weavec
// RUN: not %weavec_cc %t/node.o %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=LINK %s
// RUN: not ls %t/prog
//
// A one-step build reports the same and produces nothing.
// RUN: not %weavec_cc %S/../WholeProgram/Inputs/node.c %s -o %t/prog1 -I%S/../WholeProgram/Inputs 2>&1 | FileCheck --check-prefix=LINK %s
// RUN: not ls %t/prog1
//
// -fno-weavec-link skips the whole-program step; the objects link as usual.
// RUN: %weavec_cc -fno-weavec-link %t/node.o %t/main.o -o %t/prog2 2>&1 | count 0
// RUN: ls %t/prog2
//
// A boundary deferred by the compile step is reported by the link step when
// no unit defines the callee (the linker then fails on the same symbols).
// RUN: %weavec_cc -c %s -o %t/bnd.o -I%S/../WholeProgram/Inputs -DBOUNDARY 2>&1 | count 0
// RUN: FileCheck --check-prefix=DEFERRED %s < %t/bnd.o.weavec
// RUN: not %weavec_cc %t/node.o %t/bnd.o -o %t/prog3 2>&1 | FileCheck --check-prefix=BOUNDARY %s
//
// A sidecar older than its object is stale: ignored with a warning, and the
// object is unknown code, so nothing is checked and the link goes ahead.
// RUN: touch -t 203001010000 %t/main.o
// RUN: %weavec_cc %t/node.o %t/main.o -o %t/prog4 2>&1 | FileCheck --check-prefix=STALE %s
// RUN: ls %t/prog4
#include "../Inputs/prelude.h"
#include "node.h"

// SIDECAR: weavec-summaries 3
// SIDECAR: source {{.*}}node.c
// SIDECAR: cwd {{.+}}
// SIDECAR: arg -triple
// SIDECAR: arg -emit-obj
// SIDECAR: import free
// SIDECAR: import malloc
// SIDECAR: function node_free external plain void (struct node *)
// SIDECAR-NEXT: summary
// SIDECAR-NEXT:   effect param 0 freed(free)
// SIDECAR-NEXT:   effect param 0 *.name freed(free)
// SIDECAR-NEXT: end
// SIDECAR: function node_new external plain struct node *(void)
// SIDECAR-NEXT: summary
// SIDECAR-NEXT:   return fresh(free)
// SIDECAR-NEXT: end
// SIDECAR: function node_set_name external plain void (struct node *, char *)
// SIDECAR-NEXT: summary
// SIDECAR-NEXT:   effect param 0 *.name written
// SIDECAR-NEXT:   store param 0 *.name copy param 1
// SIDECAR-NEXT: end
// SIDECAR: function node_vp external plain int *(struct node *)
// SIDECAR-NEXT: summary
// SIDECAR-NEXT:   return borrow param 0 *.v
// SIDECAR-NEXT: end

// MAIN: import node_free
// MAIN: import node_new
// MAIN: import node_vp
// MAIN: unknown node_free
// MAIN-NOT: function
// MAIN-NOT: reported

// DEFERRED: unknown blob_close
// DEFERRED: unknown blob_open

#ifdef BOUNDARY
struct blob;
struct blob *blob_open(const char *path);
void blob_close(struct blob *b);

int main(void) {
  // BOUNDARY: rfc0005-weavec-cc.c:[[@LINE+1]]:20: warning: call to 'blob_open' is not checked: it has no definition or ownership annotations here [weavec::annotation-required]
  struct blob *b = blob_open("x");
  // BOUNDARY: rfc0005-weavec-cc.c:[[@LINE+1]]:3: warning: call to 'blob_close' is not checked
  blob_close(b);
  node_free(node_new());
  return 0;
}
#else
int main(void) {
  struct node *n = node_new();
  int *p = node_vp(n);
  // LINK: rfc0005-weavec-cc.c:[[@LINE+1]]:3: error: cannot free 'n' while it is borrowed [weavec::conflicting-borrow]
  node_free(n);
  // LINK: rfc0005-weavec-cc.c:[[@LINE+1]]:3: error: 'n' is freed twice [weavec::double-free]
  node_free(n);
  return *p;
}
#endif

// STALE: weavec-cc: warning: ignoring '{{.*}}main.o.weavec': older than '{{.*}}main.o'
// STALE-NOT: error:
