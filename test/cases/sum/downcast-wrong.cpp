// RFC 0054 §2.2: a `static_cast` to a derived class the object is not stops
// with `bad-cast` (the vtable's type information, then the ABI's dynamic
// cast), before the wrong member is read.
// FLAGS: -std=c++20 -O2 -fweavec-safe
#include <cstdio>
#include <memory>
#include <string>
#include <weavec.h>

enum class Kind { Name, Count };
struct Node { Kind kind; explicit Node(Kind k) : kind(k) {} virtual ~Node() = default; };
struct Name final : Node { std::string text = "weave"; Name() : Node(Kind::Name) {} };
struct Count final : Node { long n = 0x4141414141; Count() : Node(Kind::Count) {} };

__attribute__((noinline)) std::size_t length(const Node &node) {
  return static_cast<const Name &>(node).text.size(); // TRAP: bad-cast
}

int main() {
  auto count = std::make_unique<Count>();
  std::printf("%zu\n", length(*count));
  return 0;
}
