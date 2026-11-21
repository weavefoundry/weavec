#include "entry.h"
#include <array>
#include <cstdio>
#include <memory>
#ifdef PRIVATE
namespace {
#endif
struct Entry {
  std::array<long, 6> slots{};
  long hits = 0;
  void touch() { hits += 0x41414141; }
};
#ifdef PRIVATE
}
#endif
long touchCache(unsigned long i);
int main() {
  auto entry = std::make_unique<Entry>();
  entry->touch();
  Shared shared;
  std::printf("%ld %ld %ld\n", touchCache(1), tally(shared), limits[3]);
  return 0;
}
