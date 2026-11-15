// A header converted once: every unit that includes it checks its code.
#pragma once
#include <cstddef>
#include <weavec.h>

inline int before(const int *p) { return p[1]; } // before the marker: guarded

WEAVEC_SAFE_FILE

inline int after(const int *p) {
  return p[1]; // expected-error {{'p' points to one object and cannot be indexed or offset [weavec-safe:index-single]}}
}
class Counter {
public:
  int next() { return ++count_; }
  int *leak() {
    return new int(count_); // expected-error {{'new' makes a block no object owns [weavec-safe:raw-owner]}}
  }

private:
  int count_ = 0;
};
WEAVEC_GUARDED inline int kept(const int *p) { return p[2]; }
