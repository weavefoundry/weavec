// Guarded code that hands a stack address to a safe function that keeps it
// (stack-escape-entry.cpp).
#include <vector>
struct Counter {
  long hits = 0;
};
struct Registry {
  std::vector<Counter *> counters;
};
void enroll(Registry &r, Counter &c);
long fromGuarded() {
  Registry r;
  Counter mine;
  enroll(r, mine);
  return mine.hits;
}
