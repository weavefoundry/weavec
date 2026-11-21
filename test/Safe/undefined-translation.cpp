// RFC 0051 §1: the entries of C++'s list of core undefined behavior that
// Clang rejects when it translates a unit ("translation" in
// `resources/safe/undefined.md`).
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ftemplate-depth=64 -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s

// class.cdtor.virtual.not.x: a base's constructor names a member of a more
// derived class.
struct Derived;
struct Base {
  Base();
  virtual void hook();
};
struct Derived : Base {
  void hook() override;
};
Base::Base() { Derived::hook(); } // expected-error {{call to non-static member function without an object argument}}

// temp.inst.inf.recursion: instantiation that recurses without end stops at
// the depth limit (a fatal error, last).
template <int N> struct Count {
  static constexpr int value = Count<N + 1>::value; // expected-error {{recursive template instantiation exceeded maximum depth of 64}}
};
int depth() { return Count<0>::value; }
