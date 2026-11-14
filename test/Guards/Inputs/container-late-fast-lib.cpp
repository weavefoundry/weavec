// A guarded library whose vector is annotated before the fast program's
// constructor runs.
#include <vector>
static std::vector<int> values = [] {
  std::vector<int> made;
  made.reserve(64);
  made.push_back(1);
  return made;
}();
int grow() {
  values.push_back(2);
  return values[1];
}
