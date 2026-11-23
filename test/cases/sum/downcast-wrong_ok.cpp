// RFC 0054 §2.2: the twin of downcast-wrong.cpp: the cast follows a test of
// the kind.
// CLEAN
// FLAGS: -std=c++20 -O2 -fweavec-safe
#include <cstdio>
#include <memory>
#include <string>
#include <weavec.h>

enum class Kind { Name, Count };
struct Node { Kind kind; explicit Node(Kind k) : kind(k) {} virtual ~Node() = default; };
struct Name final : Node { std::string text = "weave"; Name() : Node(Kind::Name) {} };
struct Count final : Node { long n = 3; Count() : Node(Kind::Count) {} };

__attribute__((noinline)) std::size_t length(const Node &node) {
  switch (node.kind) {
  case Kind::Name: return static_cast<const Name &>(node).text.size();
  case Kind::Count: return static_cast<std::size_t>(static_cast<const Count &>(node).n);
  }
  return 0;
}

int main() {
  auto name = std::make_unique<Name>();
  auto count = std::make_unique<Count>();
  std::printf("%zu %zu\n", length(*name), length(*count));
  return 0;
}
