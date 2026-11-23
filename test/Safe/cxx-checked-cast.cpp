// RFC 0054 §2.1, §2.2, §3.1: in safe C++, a downcast and a `static_cast`
// from `void *` are checked where they are made, and a union that says its
// tag is used under it; the other conversions stay pointer-cast.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <string>
#include <weavec.h>

enum class Kind { Circle, Square };
struct Shape { Kind kind; explicit Shape(Kind k) : kind(k) {} virtual ~Shape() = default; };
struct Circle final : Shape { double r = 1; Circle() : Shape(Kind::Circle) {} };
struct Plain { int kind = 0; };
struct Wide : Plain { long a = 0; };
struct Context { long ticks = 0; };

double radius(const Shape &shape) {
  return shape.kind == Kind::Circle ? static_cast<const Circle &>(shape).r : 0;
}
const Circle *circle(const Shape *shape) { return static_cast<const Circle *>(shape); }
long wide(Plain *p) { return static_cast<Wide *>(p)->a; }
long ticks(void *context) { return static_cast<Context *>(context)->ticks; }
int value(void *p) { return *static_cast<int *>(p); }

long pun(Context *c) {
  return *reinterpret_cast<long *>(c); // expected-error {{a pointer is converted to a pointer to another type [weavec-safe:pointer-cast]}}
}

struct Token {
  int kind;
  union { long number WEAVEC_WHEN(0); const char *text WEAVEC_WHEN(1); } as WEAVEC_TAGGED_BY(kind);
};
long number(const Token &t) { return t.as.number; }
// A union with a member that has a destructor stays an error.
struct Owning {
  int kind;
  union U { std::string s WEAVEC_WHEN(0); long n WEAVEC_WHEN(1); U() : n(0) {} ~U() {} } u WEAVEC_TAGGED_BY(kind);
};
long owning(const Owning &o) {
  return o.u.n; // expected-error {{shares its bytes with 's', which holds a pointer or an owner [weavec-safe:union-pointer]}}
}
