// RFC 0031 §4.1: a write to an object that stands for several is weak.
// STAGE: S7
// `mallocZero`'s result is the unknown object (its allocator is a hook the
// unit cannot see), which stands for every object the analysis cannot name,
// so `t->pVtab`, which a constructor set through `&t->pVtab`, points into it
// too. `memset(t->pVtab, 0, ...)` zeroes one of those objects, not all of
// them: it had zeroed the unknown object's cells, `t->pVtab` among them, and
// the dereference after it was a definite null dereference (sqlite's
// `vtabCallConstructor`).
// TOOL
// CLEAN
#include <string.h>

typedef struct vtab {
  const void *pModule;
  int nRef;
  char *zErr;
} vtab;

typedef struct VTable {
  void *db;
  void *pMod;
  vtab *pVtab;
  int nRef;
} VTable;

extern void *(*xAlloc)(unsigned long);

static void *mallocZero(unsigned long n) {
  void *p = xAlloc(n);
  if (p)
    memset(p, 0, n);
  return p;
}

typedef int (*Ctor)(vtab **pp);

int construct(const void *m, Ctor xConstruct) {
  VTable *t = mallocZero(sizeof(VTable));
  if (!t)
    return 7;
  int rc = xConstruct(&t->pVtab);
  if (rc != 0)
    return rc;
  if (t->pVtab) {
    memset(t->pVtab, 0, sizeof(t->pVtab[0]));
    t->pVtab->pModule = m;
    t->nRef = 1;
  }
  return rc;
}
