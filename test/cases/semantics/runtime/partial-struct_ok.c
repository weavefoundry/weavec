// RFC 0032, Accepted false positives: a guarded access covers the bytes it touches, not the pointee type.
// STAGE: S3
// A node allocated without its unused tail is read through its struct type. 'n->kind' and
// 'n->len' lie inside the short block; a guard of 'sizeof(struct node)' would trap falsely.
// (When the allocation's size is visible, the static rule of RFC 0030 §7.4 asks for the
// whole pointee and reports the short block; here the size comes from the command line.)
// CLEAN
// RUN-INPUT: 8
// ASAN
#include <stddef.h>
#include <stdlib.h>
struct node { int kind; int len; char name[64]; double extra[8]; };
struct holder { struct node *n; };
static int kind_of(const struct holder *h) {
  return h->n->kind + h->n->len;
}
int main(int argc, char **argv) {
  struct holder h;
  if (argc < 2 || (size_t)atoi(argv[1]) != offsetof(struct node, name)) return 1;
  h.n = malloc((size_t)atoi(argv[1]));
  if (!h.n) return 1;
  h.n->kind = 3;
  h.n->len = 4;
  int r = kind_of(&h) != 7;
  free(h.n);
  return r;
}
