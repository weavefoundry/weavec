// RFC 0039 §4: the runtime's operator new and delete record which form
// allocated a block; a release by another form, or a sized delete of
// another size, stops the program as an invalid release. One the heap
// cannot serve throws std::bad_alloc (the C++ library's own operator new
// does), and a nothrow one returns null. An array of a type with a
// destructor carries its count before the elements; Clang's Itanium ABI
// (not Apple's arm64 one) stores and reads it through ASan's
// __asan_poison_cxx_array_cookie and __asan_load_cxx_array_cookie under
// ASan's contract, which the runtime defines (RFC 0039, amendment 15).
// RUN: %weavec_cxx -O1 %s -o %t
// RUN: %t
// RUN: not --crash %t array 2>&1 | FileCheck --check-prefix=ARRAY %s
// RUN: not --crash %t free 2>&1 | FileCheck --check-prefix=FREE %s
// RUN: not --crash %t malloc 2>&1 | FileCheck --check-prefix=MALLOC %s
// RUN: not --crash %t sized 2>&1 | FileCheck --check-prefix=SIZED %s
// RUN: not --crash %t realloc 2>&1 | FileCheck --check-prefix=REALLOC %s
// RUN: %t huge
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

struct Base {
  int base[4];
};
struct Derived : Base {
  int derived[8];
};
struct Counted {
  static int live;
  Counted() { ++live; }
  ~Counted() { --live; }
};
int Counted::live;

static void *volatile sink;
__attribute__((noinline)) static void use(void *p) { sink = p; }

int main(int argc, char **argv) {
  const char *how = argc > 1 ? argv[1] : "";
  // Every form, released by its own.
  int *one = new int(1);
  int *many = new int[8];
  auto *aligned = new (std::align_val_t(64)) double[4];
  int *quiet = new (std::nothrow) int[2];
  use(one), use(many), use(aligned), use(quiet);
  delete one;
  delete[] many;
  ::operator delete[](aligned, std::align_val_t(64));
  delete[] quiet;
  Counted *counted = new Counted[5];
  use(counted);
  delete[] counted;
  if (Counted::live != 0)
    return 1;
  std::vector<std::unique_ptr<Base>> owned;
  for (int i = 0; i < 100; ++i)
    owned.push_back(std::make_unique<Base>());
  if (strcmp(how, "array") == 0) {
    // ARRAY: weavec: invalid release of 0x{{[0-9a-f]+}}: allocated by operator new[] and released by operator delete
    Base *p = new Base[2];
    use(p);
    delete p;
  } else if (strcmp(how, "free") == 0) {
    // FREE: weavec: invalid release of 0x{{[0-9a-f]+}}: allocated by operator new and released by free
    int *p = new int(3);
    use(p);
    free(p);
  } else if (strcmp(how, "malloc") == 0) {
    // MALLOC: weavec: invalid release of 0x{{[0-9a-f]+}}: allocated by malloc and released by operator delete
    int *p = static_cast<int *>(malloc(4));
    use(p);
    delete p;
  } else if (strcmp(how, "sized") == 0) {
    // SIZED: weavec: invalid release of 0x{{[0-9a-f]+}}: released by operator delete with size 16 for a 48-byte object
    Base *p = new Derived;
    use(p);
    delete p;
  } else if (strcmp(how, "huge") == 0) {
    static volatile std::size_t huge = ~static_cast<std::size_t>(0) >> 2;
    char *none = new (std::nothrow) char[huge];
    use(none);
    if (sink != nullptr)
      return 1;
    try {
      use(new char[huge]);
      return 1;
    } catch (const std::bad_alloc &) {
    }
  } else if (strcmp(how, "realloc") == 0) {
    // REALLOC: weavec: invalid release of 0x{{[0-9a-f]+}}: allocated by operator new[] and released by realloc
    char *p = new char[16];
    use(p);
    use(realloc(p, 64));
  }
  return 0;
}
