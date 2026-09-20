// RFC 0030 §4 "Require levels": under -fweavec-require=proven, checked facets are errors too.
// STAGE: S6
// 'second' has two checked null facets and nothing else unproven (examples/checked-null.c),
// so it fails with exactly two unchecked-operation errors.
// FLAGS: -fweavec-require=proven
// EXPECT-LEDGER: /summary/errors == 2
struct node { struct node *next; int v; };

int second(struct node *n) { return n->next->v; } // BUG: unchecked-operation definite
