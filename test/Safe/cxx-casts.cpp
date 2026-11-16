// RFC 0047 §3, §4: an object's bytes are viewed as `const` characters (and
// written as characters when it is trivially copyable and holds no
// pointer), and a block `new` makes in the expression that hands it to a
// smart pointer has an owner (`pointer-cast`, `raw-owner`).
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <weavec.h>

struct Header {
  std::uint32_t magic;
  std::uint32_t size;
};
struct Node {
  Node *next = nullptr;
  int v = 0;
};
struct Base {
  virtual ~Base() = default;
};
struct Derived : Base {
  int x = 0;
};
std::string encode(const Header &h) {
  const char *bytes = reinterpret_cast<const char *>(&h);
  return std::string(bytes, sizeof h);
}
void decode(Header &h, const char *WEAVEC_COUNTED_BY(n) in, std::size_t n) {
  char *bytes = reinterpret_cast<char *>(&h);
  for (std::size_t i = 0; i < sizeof h && i < n; ++i)
    bytes[i] = in[i];
}
unsigned char first(const Node &n) {
  return reinterpret_cast<const unsigned char *>(&n)[0];
}
const std::byte *raw(const Header &h) { return reinterpret_cast<const std::byte *>(&h); }
void forge(Node &n) {
  char *b = reinterpret_cast<char *>(&n); // expected-error {{a pointer is converted to a pointer to another type [weavec-safe:pointer-cast]}}
  b[0] = 1;
}
Header *back(char *p) {
  return reinterpret_cast<Header *>(p); // expected-error {{a pointer is converted to a pointer to another type [weavec-safe:pointer-cast]}}
}
char *unconst(const Header &h) {
  return (char *)(&h); // expected-error {{a pointer is converted to a pointer to another type [weavec-safe:pointer-cast]}}
}
int down(Base &b) {
  return static_cast<Derived &>(b).x; // expected-error {{a base is converted to a derived class without a check [weavec-safe:pointer-cast]}}
}
std::unique_ptr<Node> make() { return std::unique_ptr<Node>(new Node); }
void remake(std::unique_ptr<Node> &p) { p.reset(new Node); }
std::shared_ptr<Node> shared() { return std::shared_ptr<Node>(new Node); }
std::unique_ptr<int[]> array(std::size_t n) { return std::unique_ptr<int[]>(new int[n]); }
Node *leak() {
  return new Node; // expected-error {{'new' makes a block no object owns [weavec-safe:raw-owner]}}
}
void keep(Node *&out) {
  out = new Node; // expected-error {{'new' makes a block no object owns [weavec-safe:raw-owner]}}
}
std::unique_ptr<Node> handed(Node *n) {
  return std::unique_ptr<Node>(n); // expected-error {{a raw pointer is handed to 'unique_ptr' to own [weavec-safe:raw-owner]}}
}
// The smart pointer deletes as it was declared.
std::unique_ptr<int> arrayIntoSingle(std::size_t n) {
  return std::unique_ptr<int>(new int[n]); // expected-error {{'new' makes a block no object owns [weavec-safe:raw-owner]}}
}
std::unique_ptr<int[]> singleIntoArray() {
  return std::unique_ptr<int[]>(new int); // expected-error {{'new' makes a block no object owns [weavec-safe:raw-owner]}}
}
struct Plain {
  ~Plain() {}
};
struct Extended : Plain {
  int y = 0;
};
std::unique_ptr<Plain> sliced() {
  return std::unique_ptr<Plain>(new Extended); // expected-error {{'new' makes a block no object owns [weavec-safe:raw-owner]}}
}
std::unique_ptr<Base> derived() { return std::unique_ptr<Base>(new Derived); }
std::shared_ptr<Plain> sharedDerived() { return std::shared_ptr<Plain>(new Extended); }
// A string or a container copies what it is made from.
std::string copied(char *text) { return std::string(text); }
