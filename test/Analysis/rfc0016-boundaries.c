// RFC 0016: every failed projection retains an explicit coverage reason.
// RUN: %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"

#define PARAMS_13 char *a, char *b, char *c, char *d, char *e, char *f, char *g, char *h, char *i, char *j, char *k, char *l, char *m
#define READ_13 *a=1; *b=1; *c=1; *d=1; *e=1; *f=1; *g=1; *h=1; *i=1; *j=1; *k=1; *l=1; *m=1
static void many_relations(PARAMS_13) { READ_13; free(a); }
void relation_limit(void) {
  char *p = malloc(4); if (!p) return;
  // CHECK: warning: analysis is incomplete: call context relationship limit reached [weavec::analysis-incomplete]
  many_relations(p,p,p,p,p,p,p,p,p,p,p,p,p);
}

#define PARAMS_33 PARAMS_13, char *n, char *o, char *p, char *q, char *r, char *s, char *t, char *u, char *v, char *w, char *x, char *y, char *z, char *aa, char *ab, char *ac, char *ad, char *ae, char *af, char *ag
static void many_inputs(PARAMS_33) {
  READ_13; *n=1; *o=1; *p=1; *q=1; *r=1; *s=1; *t=1; *u=1; *v=1; *w=1;
  *x=1; *y=1; *z=1; *aa=1; *ab=1; *ac=1; *ad=1; *ae=1; *af=1; *ag=1;
  free(a);
}
void input_limit(void) {
  char *p = malloc(4); if (!p) return;
  // CHECK: warning: analysis is incomplete: call context input path limit reached [weavec::analysis-incomplete]
  many_inputs(p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p);
}

static void uncertain(char *a, char *b, char *c) { *b=1; *c=1; free(a); }
void missing_relation(char *q) {
  char *p = malloc(4); if (!p) return;
  // CHECK: warning: analysis is incomplete: unresolved call alias relationship [weavec::analysis-incomplete]
  uncertain(p, p, q);
}

struct Box { char *data; };
static void children(struct Box *a, struct Box *b) { *b->data=1; free(a->data); }
void missing_view(void *p) {
  // CHECK: warning: analysis is incomplete: unrepresentable call context input path [weavec::analysis-incomplete]
  children(p, p);
}
