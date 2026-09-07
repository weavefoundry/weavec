// RFC 0006 regression cases, amended by RFC 0015: selected cells retain
// their history across independent updates, scalar writes and joins.
// Distinct unresolved scalar indices are not proof of disjointness.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"

// Reported: same witness.
void same_constant(void) {
  int *arr[4];
  arr[0] = malloc(4);
  arr[1] = malloc(4);
  free(arr[0]);
  use(arr[1]); free(arr[1]); // another element: fine
  // CHECK: rfc0006-elements.c:[[@LINE+1]]:7: error: use of 'arr[0]' after it was freed [weavec::use-after-free]
  use(arr[0]);
}

void same_variable(char **a, int i, int j) {
  free(a[i]);
  // CHECK: rfc0006-elements.c:[[@LINE+1]]:7: error: use of 'a[j]' after it was freed [weavec::use-after-free]
  use(a[j]); // may select the released cell
  // CHECK: rfc0006-elements.c:[[@LINE+1]]:3: error: use of 'a[i]' after it was freed [weavec::use-after-free]
  a[i][0] = 0;
}

void double_free_element(char **a) {
  free(a[0]);
  // CHECK: rfc0006-elements.c:[[@LINE+1]]:3: error: 'a[0]' is freed twice [weavec::double-free]
  free(a[0]);
}

void whole_access_matches(char **a, int i) {
  free(a[i]);
  // CHECK: rfc0006-elements.c:[[@LINE+1]]:7: error: use of 'a[0]' after it was freed [weavec::use-after-free]
  use(*a);
}

void first_element(void) {
  int *arr[2];
  arr[0] = malloc(4);
  free(*arr); // `*arr` on an array is `arr[0]`
  // CHECK: rfc0006-elements.c:[[@LINE+1]]:7: error: use of 'arr[0]' after it was freed [weavec::use-after-free]
  use(arr[0]);
}

void joined_on_both_sides(char **a, int i, int c) {
  if (c)
    free(a[i]);
  else
    free(a[i]);
  // CHECK: rfc0006-elements.c:[[@LINE+1]]:7: error: use of 'a[i]' after it was freed [weavec::use-after-free]
  use(a[i]);
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
  // CHECK: rfc0006-elements.c:[[@LINE+1]]:7: warning: analysis is incomplete: array cleanup membership is unresolved [weavec::analysis-incomplete]
  use(a[0]);
}

void incremented(char **a, int i) {
  free(a[i]);
  i++;
  use(a[i]);
}

void reassigned(char **a, int i, int j) {
  free(a[i]);
  i = j;
  // CHECK: rfc0006-elements.c:[[@LINE+1]]:7: error: use of 'a[i]' after it was freed [weavec::use-after-free]
  use(a[i]);
}

void unrecognised_index(char **a, int i) {
  free(a[i + 1]);
  // CHECK: rfc0006-elements.c:[[@LINE+1]]:7: error: use of 'a[i+1]' after it was freed [weavec::use-after-free]
  use(a[i + 1]);
}

void joined_with_different_witnesses(char **a, int i, int j, int c) {
  if (c)
    free(a[i]);
  else
    free(a[j]);
  // CHECK: rfc0006-elements.c:[[@LINE+1]]:7: error: use of 'a[i]' after it was freed [weavec::use-after-free]
  use(a[i]);
}

// CHECK: 1 warning and 10 errors generated.
