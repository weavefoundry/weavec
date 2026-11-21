// RFC 0052 §7: a WEAVEC_ALIASED class owns no storage that changes type in
// place, at any depth of its members. The round of 2026-11-21's
// `types/p23`.
// RUN: %weavec_cxx -std=c++23 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <string>
#include <variant>
#include <weavec.h>

using V = std::variant<std::string, long>;
struct L0 {
  V v = std::string(48, 'x');
};
struct L1 {
  L0 x;
};
struct L2 {
  L1 x;
};
struct WEAVEC_ALIASED Doc {
  L2 x; // expected-error {{'Doc' is WEAVEC_ALIASED and owns 'x', whose contents change type in place [weavec-safe:owner-path]}}
};
struct S0 {
  std::string name;
  long n = 0;
};
struct WEAVEC_ALIASED Plain {
  S0 x;
};
