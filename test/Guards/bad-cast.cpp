// RFC 0054 §2.2: a checked downcast of a safe C++ function stops with
// `bad-cast`: a class with virtual functions by the type information its
// vtable names (and the C++ ABI's dynamic cast for a class derived further),
// any other class by the runtime's records; and a `static_cast` from
// `void *`.
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe %s -o %t.fast
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -fweavec-safe-checks=full %s -o %t.full
// RUN: %t.fast | FileCheck %s --check-prefix=OK
// RUN: %t.full | FileCheck %s --check-prefix=OK
// RUN: not --crash %t.fast 1 2>&1 | FileCheck %s --check-prefix=VIRTUAL
// RUN: not --crash %t.full 1 2>&1 | FileCheck %s --check-prefix=VIRTUAL
// RUN: not --crash %t.fast 2 2>&1 | FileCheck %s --check-prefix=PLAIN
// RUN: not --crash %t.fast 3 2>&1 | FileCheck %s --check-prefix=VOID
// RUN: not --crash %t.fast 4 2>&1 | FileCheck %s --check-prefix=SECOND
// RUN: not --crash %t.fast 5 2>&1 | FileCheck %s --check-prefix=GROWN
// RUN: not --crash %t.full 5 2>&1 | FileCheck %s --check-prefix=GROWN
#include <cstdio>
#include <memory>
#include <weavec.h>

enum class Kind { Circle, Square };
struct Shape { Kind kind; explicit Shape(Kind k) : kind(k) {} virtual ~Shape() = default; };
struct Circle : Shape { double r = 2; Circle() : Shape(Kind::Circle) {} };
struct Ring final : Circle { double inner = 1; };
struct Square final : Shape { double s = 3; Square() : Shape(Kind::Square) {} };
struct Plain { int kind = 0; };
struct Wide : Plain { long a = 7, b = 8; };
struct Server { long ticks = 0; };
// A secondary vtable names its class too: the check also asks that the
// object start where its vtable says (offset to top 0).
struct B1 { virtual ~B1() = default; long one = 1; };
struct B2 { virtual ~B2() = default; long two = 2; };
struct D final : B1, B2 { long d = 3; };
struct Twin { D d; B2 extra; };
struct Client { double load = 0; };
// A class with virtual functions derived from one without: the check asks
// the records, since the base has no vtable to read.
struct Data { long first = 0x41414141; };
struct Active : Data { virtual ~Active() = default; long v = 9; };
// A class template's instance derives from a class too, so a local of that
// class is recorded.
struct Root { long tag = 1; };
template <class T> struct Grown : Root { T more[8] = {}; };
// An array `new` of a class with a destructor puts a cookie before the
// elements: the block records no type, and its elements convert back.
struct Item { long a = 1, b = 2; ~Item() { a = 0; } };

__attribute__((noinline)) double radius(const Shape &shape) {
  // VIRTUAL: weavec: bad-cast at {{.*}}bad-cast.cpp:[[@LINE+2]]:{{[0-9]+}}: 0x{{[0-9a-f]+}} is not a 'Circle'
  // VIRTUAL: of type 'Square'
  return static_cast<const Circle &>(shape).r;
}
__attribute__((noinline)) long wide(Plain *p) {
  // PLAIN: weavec: bad-cast at {{.*}}bad-cast.cpp:[[@LINE+1]]:{{[0-9]+}}: 0x{{[0-9a-f]+}} is not a 'Wide'
  return static_cast<Wide *>(p)->b;
}
__attribute__((noinline)) long ticks(void *context) {
  // VOID: weavec: bad-cast at {{.*}}bad-cast.cpp:[[@LINE+1]]:{{[0-9]+}}: 0x{{[0-9a-f]+}} is not a 'Server'
  return static_cast<Server *>(context)->ticks;
}

__attribute__((noinline)) long fromSecond(B2 &b) {
  // SECOND: weavec: bad-cast at {{.*}}bad-cast.cpp:[[@LINE+1]]:{{[0-9]+}}: 0x{{[0-9a-f]+}} is not a 'D'
  return static_cast<D &>(b).d;
}

__attribute__((noinline)) long active(Data &data) {
  return static_cast<Active &>(data).v;
}
__attribute__((noinline)) long grown(Root &root) {
  // GROWN: weavec: bad-cast at {{.*}}bad-cast.cpp:[[@LINE+1]]:{{[0-9]+}}: 0x{{[0-9a-f]+}} is not a 'Grown<long>'
  return static_cast<Grown<long> &>(root).more[7];
}
__attribute__((noinline)) long notGrown() {
  Root root;
  return grown(root);
}
__attribute__((noinline)) long item(void *context) {
  return static_cast<Item *>(context)->b;
}

WEAVEC_GUARDED int main(int argc, char **argv) {
  const char mode = argc > 1 ? argv[1][0] : '0';
  auto circle = std::make_unique<Circle>();
  auto ring = std::make_unique<Ring>();
  auto square = std::make_unique<Square>();
  auto wideOne = std::make_unique<Wide>();
  auto server = std::make_unique<Server>();
  server->ticks = 5;
  if (mode == '1')
    return (int)radius(*square);
  if (mode == '2') {
    auto plain = std::make_unique<Plain>();
    return (int)wide(plain.get());
  }
  if (mode == '4') {
    auto twin = std::make_unique<Twin>();
    return (int)fromSecond(twin->extra);
  }
  if (mode == '3') {
    auto client = std::make_unique<Client>();
    return (int)ticks(client.get());
  }
  if (mode == '5')
    return (int)notGrown();
  auto activeOne = std::make_unique<Active>();
  auto grownOne = std::make_unique<Grown<long>>();
  Item *items = new Item[4];
  // OK: 2 2 8 5 9 0 2 2
  std::printf("%g %g %ld %ld %ld %ld %ld %ld\n", radius(*circle), radius(*ring),
              wide(wideOne.get()), ticks(server.get()), active(*activeOne),
              grown(*grownOne), item(items), item(items + 3));
  delete[] items;
  return 0;
}
