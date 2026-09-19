// Engine pin converted from test/Analysis/rfc0006-elements.c; markers are the v0.10.0 golden diagnostics.
// RFC 0006 regression cases, amended by RFC 0015: selected cells retain
// their history across independent updates, scalar writes and joins.
// Distinct unresolved scalar indices are not proof of disjointness.
// RFC 0030 (*Diagnostics*, §15 item 3): `analysis-incomplete` is removed; each
// such pin now has the ledger row that replaces it (`UNRESOLVED`), and is
// listed in test/cases/KNOWN-DIFFERENCES.md.
#include "Inputs/prelude.h"

// Reported: same witness.
void same_constant(void) {
  int *arr[4];
  arr[0] = malloc(4);
  arr[1] = malloc(4);
  free(arr[0]);
  use(arr[1]); free(arr[1]); // another element: fine
  use(arr[0]); // BUG: use-after-free definite
}

void same_variable(char **a, int i, int j) {
  free(a[i]);
  use(a[j]); // may select the released cell // BUG: use-after-free definite
  a[i][0] = 0; // BUG: use-after-free definite
}

void double_free_element(char **a) {
  free(a[0]);
  free(a[0]); // BUG: double-free definite
}

void whole_access_matches(char **a, int i) {
  free(a[i]);
  use(*a); // BUG: use-after-free definite
}

void first_element(void) {
  int *arr[2];
  arr[0] = malloc(4);
  free(*arr); // `*arr` on an array is `arr[0]`
  use(arr[0]); // BUG: use-after-free definite
}

void joined_on_both_sides(char **a, int i, int c) {
  if (c)
    free(a[i]);
  else
    free(a[i]);
  use(a[i]); // BUG: use-after-free definite
}

// A complete cleanup loop, and preservation of old index values.
void loop_free(char **a, int n) {
  for (int i = 0; i < n; i++)
    free(a[i]);
  free(a);
}

void null_out(char **a, int n) {
  for (int i = 0; i < n; i++) {
    free(a[i]);
    a[i] = NULL;
  }
  use(a[0]); // BUG: analysis-incomplete possible // UNRESOLVED: temporal:unanalysed
}

void incremented(char **a, int i) {
  free(a[i]);
  i++;
  use(a[i]);
}

void reassigned(char **a, int i, int j) {
  free(a[i]);
  i = j;
  use(a[i]); // BUG: use-after-free definite
}

void unrecognised_index(char **a, int i) {
  free(a[i + 1]);
  use(a[i + 1]); // BUG: use-after-free definite
}

void joined_with_different_witnesses(char **a, int i, int j, int c) {
  if (c)
    free(a[i]);
  else
    free(a[j]);
  use(a[i]); // BUG: use-after-free definite
}
