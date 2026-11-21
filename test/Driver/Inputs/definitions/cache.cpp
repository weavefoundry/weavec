#include "entry.h"
#include <vector>
// A helper of this file's own, under a name another file uses for its own.
#ifdef PRIVATE
namespace {
#endif
struct Entry {
  long hits = 0;
  void touch() { hits += 1; }
};
#ifdef PRIVATE
}
#endif
long touchCache(unsigned long i) {
  std::vector<Entry> entries(4);
  entries.at(i).touch();
  return entries.at(i).hits;
}
long tally(const Shared &shared) { return shared.count; }
const long limits[LIMITS] = {1, 2, 3, 4};
