// RFC 0054 §3.2 (amendments): in C++ the tag is not bound to a reference,
// stored through a pointer, a conditional or a library call, nor named by
// a member pointer, and a member is not bound to a reference. Each case is
// one of G6's second round.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <utility>
#include <weavec.h>

struct S {
  int kind = 0;
  union { long n WEAVEC_WHEN(0); long *p WEAVEC_WHEN(1); } u WEAVEC_TAGGED_BY(kind);
};

void byReference(S &s) {
  int &k = s.kind; // expected-error {{'kind' is the tag of 'u': it is changed only by an assignment, which clears the union [weavec-safe:union-pointer]}}
  k = 1;
}
void byConditional(S &a, S &b, bool c) {
  (c ? a.kind : b.kind) = 1; // expected-error 2 {{'kind' is the tag of 'u': it is changed only by an assignment, which clears the union [weavec-safe:union-pointer]}}
}
int byLibrary(S &s) {
  return std::exchange(s.kind, 1); // expected-error {{'kind' is the tag of 'u': it is changed only by an assignment, which clears the union [weavec-safe:union-pointer]}}
}
void byMemberPointer(S &s) {
  int S::*m = &S::kind; // expected-error {{a member pointer to 'kind' would reach a tagged union apart from its check [weavec-safe:union-pointer]}}
  s.*m = 1;
}
long &keep(long &n WEAVEC_LIFETIMEBOUND) { return n; }
void memberByReference(S &s) {
  long &n = keep(s.u.n); // expected-error {{the address of 'n' would outlive the check of 'kind' [weavec-safe:union-pointer]}}
  s.kind = 1;
  n = 5;
}
long fine(S &s) {
  s.kind = 0;
  s.u.n = 5;
  ++s.u.n;
  return s.u.n + s.kind;
}
