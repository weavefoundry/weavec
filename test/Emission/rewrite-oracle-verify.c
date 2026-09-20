// RFC 0030, section 10.6, gate G8: verify mode checks unproven facets with the __weavec_chk_ family.
// The -O0 IR equals that of Inputs/rewrite-oracle-verify.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-verify.expected.c %t -- -fweavec-checks=verify

int load(int *p, int i) {
  int a[4] = {0};
  return *p + a[i];
}
