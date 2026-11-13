// RFC 0044 §4: the rules of single expressions in a safe C++ function:
// raw-owner, index-single, pointer-cast, pointer-bytes, union-pointer,
// int-to-pointer, variadic, no-asm, unsafe-block, bad-annotation and
// not-modeled, each with accepted and rejected forms.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <array>
#include <bit>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <weavec.h>

struct Item {
  int v = 0;
  std::string name;
};
struct Base {
  virtual ~Base() = default;
  int b = 0;
};
struct Derived : Base {
  int d = 0;
};
struct Plain {
  int a = 0;
  int b = 0;
};

// raw-owner
Item *rawNew() {
  return new Item; // expected-error {{'new' makes a block no object owns [weavec-safe:raw-owner]}}
}
int *rawArray(std::size_t n) {
  return new int[n]; // expected-error {{'new' makes a block no object owns [weavec-safe:raw-owner]}}
}
void rawDelete(Item *item) {
  delete item; // expected-error {{'delete' releases a block by hand [weavec-safe:raw-owner]}}
}
void *rawMalloc(std::size_t n) {
  return std::malloc(n); // expected-error {{'malloc' returns a block no object owns [weavec-safe:raw-owner]}}
}
void rawFree(void *p) {
  std::free(p); // expected-error {{'free' releases a block by hand [weavec-safe:raw-owner]}}
}
char *rawDuplicate(const char *s) {
  return strdup(s); // expected-error {{'strdup' returns a block no object owns [weavec-safe:raw-owner]}}
}
Item *released(std::unique_ptr<Item> p) {
  return p.release(); // expected-error {{'release' takes a block away from the object that owns it [weavec-safe:raw-owner]}} // expected-error {{what is returned points into 'p', which does not outlive the function [weavec-safe:escape]}}
}
std::unique_ptr<Item> adopted(Item *raw) {
  return std::unique_ptr<Item>(raw); // expected-error {{a raw pointer is handed to 'unique_ptr' to own [weavec-safe:raw-owner]}}
}
void destroyed(Item &item) {
  item.~Item(); // expected-error {{a destructor is called by hand [weavec-safe:raw-owner]}}
}
std::unique_ptr<Item> made() { return std::make_unique<Item>(); }
std::shared_ptr<Item> madeShared() { return std::make_shared<Item>(); }
std::vector<int> madeArray(std::size_t n) { return std::vector<int>(n); }
Item *borrowed(const std::unique_ptr<Item> &p) { return p.get(); }
void reset(std::unique_ptr<Item> &p, std::optional<Item> &o) {
  p.reset();
  o.reset();
}

// index-single
int indexed(const int *p) {
  return p[1]; // expected-error {{a pointer is indexed, and it points to one object [weavec-safe:index-single]}}
}
int walked(const int *p) {
  ++p; // expected-error {{arithmetic on a pointer, which points to one object [weavec-safe:index-single]}}
  return *(p + 1); // expected-error {{arithmetic on a pointer, which points to one object [weavec-safe:index-single]}}
}
const int *stepped(const int *p, std::size_t n) {
  p += n; // expected-error {{arithmetic on a pointer, which points to one object [weavec-safe:index-single]}}
  return p;
}
int decays(int n) {
  int local[4] = {1, 2, 3, 4};
  const int *p = local; // expected-error {{an array becomes a pointer that has lost its size [weavec-safe:index-single]}}
  return *p + local[n & 3]; // an array is indexed in place
}
std::span<int> forged(int *p, std::size_t n) {
  return std::span<int>(p, n); // expected-error {{a 'span' is made from a pointer and a size nothing checks [weavec-safe:index-single]}}
}
int single(const int *p, const int *q) { return *p + int(q - p) + (p == q ? 1 : 0); }
int spanned(std::span<const int> s) {
  int total = 0;
  for (std::size_t i = 0; i < s.size(); ++i)
    total += s[i];
  for (auto it = s.begin(); it != s.end(); ++it)
    total += *it;
  return total;
}
int viewed(std::string_view s) {
  int n = 0;
  for (auto it = s.begin(); it != s.end(); ++it)
    n += *it == ' ' ? 1 : 0;
  return n;
}
int arrays() {
  std::array<int, 4> a = {1, 2, 3, 4};
  int local[4] = {1, 2, 3, 4};
  std::span<int> whole(local);
  return a[1] + whole[2] + int(std::strlen("literal"));
}

// pointer-cast
long reinterpreted(Item *item) {
  return *reinterpret_cast<long *>(item); // expected-error {{a pointer is converted to a pointer to another type [weavec-safe:pointer-cast]}}
}
int cStyle(const void *p) {
  return *(const int *)p; // expected-error {{a pointer is converted to a pointer to another type [weavec-safe:pointer-cast]}}
}
int fromVoid(void *p) {
  return *static_cast<int *>(p); // expected-error {{a pointer is converted to a pointer to another type [weavec-safe:pointer-cast]}}
}
void constRemoved(const Item &item) {
  const_cast<Item &>(item).v = 1; // expected-error {{a cast removes 'const' [weavec-safe:pointer-cast]}}
}
int cStyleConst(const Item *item) {
  return ((Item *)item)->v; // expected-error {{a cast removes 'const' [weavec-safe:pointer-cast]}}
}
int downcast(Base *base) {
  return static_cast<Derived *>(base)->d; // expected-error {{a base is converted to a derived class without a check [weavec-safe:pointer-cast]}}
}
int downcastChecked(Base *base) {
  if (auto *derived = dynamic_cast<Derived *>(base))
    return derived->d;
  return 0;
}
int upcast(Derived *derived) {
  Base *base = derived;
  return base->b + int(static_cast<double>(derived->d));
}
std::uint32_t bits(float f) { return std::bit_cast<std::uint32_t>(f); }

// int-to-pointer
Item *fromInteger(std::uintptr_t address) {
  return (Item *)address; // expected-error {{an integer cannot become a pointer in a safe function [weavec-safe:int-to-pointer]}} // expected-error {{a pointer to 'address' is returned, and it does not outlive the function [weavec-safe:escape]}}
}

// pointer-bytes
void bytes(Item &to, const Item &from, Plain &plain, const Plain &other) {
  std::memcpy(&to, &from, sizeof(Item)); // expected-error {{'memcpy' reads or writes the bytes of a 'Item', which is not trivially copyable [weavec-safe:pointer-bytes]}}
  std::memset(&to, 0, sizeof(Item)); // expected-error {{'memset' reads or writes the bytes of a 'Item', which is not trivially copyable [weavec-safe:pointer-bytes]}}
  std::memcpy(&plain, &other, sizeof(Plain));
  to = from;
}

// union-pointer
union Number {
  int i;
  float f;
};
union Either {
  int i;
  const char *text;
};
int unionOfScalars(Number n) { return n.i; }
int unionOfPointer(Either e) {
  return e.i; // expected-error {{union member 'i' shares its bytes with 'text', which holds a pointer or an owner [weavec-safe:union-pointer]}}
}
int variant(const std::variant<int, std::string> &v) {
  return std::holds_alternative<int>(v) ? std::get<int>(v) : int(std::get<std::string>(v).size());
}

// variadic, no-asm, unsafe-block, not-modeled
int sumOf(int count, ...) {
  va_list arguments;
  va_start(arguments, count); // expected-error {{a safe function cannot read its variadic arguments [weavec-safe:variadic]}}
  int total = va_arg(arguments, int); // expected-error {{a safe function cannot read its variadic arguments [weavec-safe:variadic]}}
  va_end(arguments);
  return total;
}
template <typename... T> int sumPack(T... values) { return (0 + ... + values); }
int usesPack() { return sumPack(1, 2, 3) + std::printf("%d\n", 3); }
int assembly() {
  __asm__("nop"); // expected-error {{inline assembly cannot be in a safe function [weavec-safe:no-asm]}}
  return 0;
}
int unsafeBlock(const std::vector<int> &v) {
  WEAVEC_UNSAFE { return v[0]; } // expected-error {{a WEAVEC_UNSAFE block cannot be inside a safe function [weavec-safe:unsafe-block]}}
}
int jumps(int n) {
again:
  if (--n > 0)
    goto again; // expected-error {{'goto' is not followed by the rules [weavec-safe:not-modeled]}}
  return n;
}
int statementExpression(int n) {
  return ({ int m = n; m + 1; }); // expected-error {{a statement expression is not followed by the rules [weavec-safe:not-modeled]}}
}

// bad-annotation
int counted(const int *WEAVEC_COUNTED_BY(n) p, std::size_t n) { // expected-error {{'weavec.counted_by' is one of Safe C's annotations, which a safe C++ function does not use [weavec-safe:bad-annotation]}}
  return n != 0 ? *p : 0;
}
void owned(int *WEAVEC_OWNED p) { // expected-error {{'weavec.owned' is one of Safe C's annotations, which a safe C++ function does not use [weavec-safe:bad-annotation]}}
  (void)p;
}
