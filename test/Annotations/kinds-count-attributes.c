// RFC 0030 §7.2, row 5: Clang's `counted_by(n)`, `counted_by_or_null(n)`,
// `sized_by(n)` and `sized_by_or_null(n)` on fields (and parameters where
// Clang accepts them) are `counted(n)` or `sized(n)` at the ecosystem level,
// nonnull unless `_or_null`. A counted flexible array member has no
// nullability. `sized_by` counts bytes, unlike WEAVEC_SIZED_BY.
// RUN: %weavec --dump-kinds %s -- | FileCheck %s

struct packet {
  int n;
  // CHECK: field packet.counted: counted(.n scale 1 plus 0) nonnull [declared, declared, shape ecosystem, nullability ecosystem]
  int *__attribute__((counted_by(n))) counted;
  // CHECK: field packet.maybe: counted(.n scale 1 plus 0) nullable [declared, declared, shape ecosystem, nullability ecosystem]
  int *__attribute__((counted_by_or_null(n))) maybe;
  // CHECK: field packet.bytes: sized(.n scale 1 plus 0) nonnull [declared, declared, shape ecosystem, nullability ecosystem]
  int *__attribute__((sized_by(n))) bytes;
  // CHECK: field packet.maybe_bytes: sized(.n scale 1 plus 0) nullable [declared, declared, shape ecosystem, nullability ecosystem]
  void *__attribute__((sized_by_or_null(n))) maybe_bytes;
  // CHECK: field packet.payload: counted(.n scale 1 plus 0) nullable [declared, declared, shape ecosystem]
  char payload[] __attribute__((counted_by(n)));
};

// (This Clang accepts the attributes on fields only.)
int use(struct packet *p) { return p->n; }
