// RFC 0040 §5: C++ frames are demangled, a frame inlined into another
// repeats its number, and a delete of an array names its kind and line.
// RUN: %weavec_cxx -O2 %s -o %t
// RUN: not --crash %t index 2>&1 | FileCheck %s --check-prefix=INDEX
// RUN: not --crash %t delete 2>&1 | FileCheck %s --check-prefix=DELETE
#include <cstring>
#include <vector>

namespace app {
struct Widget {
  int v[4];
};
Widget *volatile last;
__attribute__((noinline)) Widget *make(int n) {
  Widget *w = new Widget[n];
  last = w;
  return w;
}
template <class T> __attribute__((noinline)) int peek(std::vector<T> &v, int i) {
  return v[i];
}
} // namespace app

int main(int argc, char **argv) {
  if (std::strcmp(argv[1], "index") == 0) {
    std::vector<int> v(3);
    return app::peek(v, argc + 2);
  }
  app::Widget *w = app::make(argc);
  delete w;
  return 0;
}
// INDEX: weavec: library-assertion at {{.*}}report-cxx.cpp:20:{{[0-9]+}}:
// INDEX-NEXT: #0 0x[[PC:[0-9a-f]+]] in {{.*}}operator[]{{.*}} {{.*}}vector{{.*}}
// INDEX-NEXT: #0 0x[[PC]] in int app::peek<int>(std::{{.*}}vector<int{{.*}}>&, int) {{.*}}report-cxx.cpp:20
// INDEX-NEXT: #1 0x{{[0-9a-f]+}} in main {{.*}}report-cxx.cpp:27
// INDEX: weavec: summary: library-assertion at {{.*}} in {{.*}}operator[]

// DELETE: weavec: alloc-release-mismatch at {{.*}}report-cxx.cpp:30:{{[0-9]+}}: operator delete of 0x{{[0-9a-f]+}}: allocated by operator new[]
// DELETE: is the 32-byte heap object at 0x{{[0-9a-f]+}}, allocated by thread T0:
// DELETE-NEXT: #0 0x{{[0-9a-f]+}} in app::make(int) {{.*}}report-cxx.cpp:15
