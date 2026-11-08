// RFC 0039 §5: a std::vector's and a std::deque's storage past their
// elements is not addressable (the C++ library's container annotations);
// growth, shrinking and copies keep it right, and WEAVEC_RT_CONTAINERS=0
// turns the check off.
// RUN: %weavec_cxx -O1 -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_NONE -U_GLIBCXX_ASSERTIONS %s -o %t
// RUN: %t
// RUN: not --crash %t past 2>&1 | FileCheck --check-prefix=PAST %s
// RUN: not --crash %t popped 2>&1 | FileCheck --check-prefix=POPPED %s
// RUN: env WEAVEC_RT_CONTAINERS=0 %t past
#include <cstring>
#include <deque>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  const char *how = argc > 1 ? argv[1] : "";
  std::vector<int> v;
  v.reserve(8);
  v.push_back(1);
  std::vector<std::string> names;
  for (int i = 0; i < 50; ++i)
    names.emplace_back(40, static_cast<char>('a' + i % 26));
  names.erase(names.begin() + 3, names.begin() + 9);
  std::vector<int> copy = v;
  copy.insert(copy.begin(), 5, 7);
  std::deque<int> d;
  for (int i = 0; i < 1000; ++i)
    i % 3 == 0 ? d.push_front(i) : d.push_back(i);
  while (d.size() > 10)
    d.pop_front();
  int sum = d[3] + copy[5] + static_cast<int>(names[10].size());
  if (strcmp(how, "past") == 0)
    // PAST: weavec: container-overflow at {{.*}}container-overflow.cpp:[[@LINE+2]]:{{[0-9]+}}: read of 4 bytes
    // PAST-NEXT: weavec: 0x{{[0-9a-f]+}} is 16 bytes past the elements of a container in the 32-byte heap object at 0x{{[0-9a-f]+}}
    sum += v.data()[5];
  if (strcmp(how, "popped") == 0) {
    v.push_back(2);
    v.pop_back();
    // POPPED: weavec: container-overflow at {{.*}}container-overflow.cpp:[[@LINE+1]]:{{[0-9]+}}: read of 4 bytes
    sum += v.data()[1];
  }
  return sum == 0;
}
