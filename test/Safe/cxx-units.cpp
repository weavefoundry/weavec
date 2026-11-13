// RFC 0044 §1: which C++ functions are safe. Under -fweavec-safe, every
// function the unit defines, each lambda in one and each instantiation of
// its templates, unless WEAVEC_GUARDED or WEAVEC_UNSAFE says otherwise on
// the function or its class. Without the flag, what WEAVEC_SAFE marks.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -ferror-limit=0 -Xclang -verify=unit,both -Xclang -verify-ignore-unexpected=note %s
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -ferror-limit=0 -Xclang -verify=marked,both -Xclang -verify-ignore-unexpected=note %s
// The first error of a compile says how to read its rule.
// RUN: not %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe %s 2>&1 | FileCheck %s --check-prefix=NOTE
// NOTE: note: 'weavec-c++ --weavec-explain raw-owner' explains this rule
// NOTE-NOT: explains this rule
#include <string>
#include <vector>
#include <weavec.h>

int *plain() {
  return new int(1); // unit-error {{'new' makes a block no object owns [weavec-safe:raw-owner]}}
}
WEAVEC_SAFE int *marked() {
  return new int(2); // both-error {{'new' makes a block no object owns [weavec-safe:raw-owner]}}
}
WEAVEC_GUARDED int *guarded() { return new int(3); }
WEAVEC_UNSAFE int *unsafe() { return new int(4); }

struct WEAVEC_SAFE Checked {
  std::vector<int> data;
  int first() {
    int &r = data[0];
    data.push_back(1);
    return r; // both-error {{'r' points into 'data', which 'push_back' may have changed [weavec-safe:stale-borrow]}}
  }
  WEAVEC_GUARDED int *raw() { return new int(5); }
};
struct WEAVEC_GUARDED Buffer {
  int *data = nullptr;
  explicit Buffer(int n) : data(new int[static_cast<unsigned>(n)]) {}
  ~Buffer() { delete[] data; }
  Buffer(const Buffer &) = delete;
  Buffer &operator=(const Buffer &) = delete;
  WEAVEC_SAFE int at(int i) const {
    return data[i]; // both-error {{a pointer is indexed, and it points to one object [weavec-safe:index-single]}}
  }
};

// A template is checked where it is instantiated, each error once.
template <typename T> const T &firstAfterGrowing(std::vector<T> &v, const T &value) {
  const T &first = v[0];
  v.push_back(value);
  return first; // unit-error {{'first' points into 'v', which 'push_back' may have changed [weavec-safe:stale-borrow]}}
}
template <typename T> T neverInstantiated(T *p) { return p[3]; }
int instantiates(std::vector<int> &a, std::vector<std::string> &b) {
  return firstAfterGrowing(a, 1) + int(firstAfterGrowing(b, std::string("x")).size());
}

// A lambda is checked with the function it is written in.
int lambdas(std::vector<int> &v) {
  auto grow = [&v] {
    int &r = v[0];
    v.push_back(1);
    return r; // unit-error {{'r' points into 'v', which 'push_back' may have changed [weavec-safe:stale-borrow]}}
  };
  auto generic = [](auto &items) {
    auto &r = items[0];
    items.clear();
    return r; // unit-error {{'r' points into 'items', which 'clear' may have changed [weavec-safe:stale-borrow]}}
  };
  return grow() + generic(v);
}
