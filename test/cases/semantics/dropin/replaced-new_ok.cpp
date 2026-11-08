// RFC 0039, amendment 18: a program that replaces only operator new(size_t)
// and operator delete(void*), here with a header before each block, gets its
// own functions for every form: the sized, array and nothrow forms that the
// compiler and the C++ library call chain to them, as the library's do.
// CLEAN
// FLAGS: -O1
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

static int live;

void *operator new(std::size_t size) {
  char *block = static_cast<char *>(std::malloc(size + 16));
  if (block == nullptr)
    throw std::bad_alloc();
  std::memcpy(block, "weavec-replaced", 16);
  ++live;
  return block + 16;
}

void operator delete(void *p) noexcept {
  if (p == nullptr)
    return;
  char *block = static_cast<char *>(p) - 16;
  if (std::memcmp(block, "weavec-replaced", 16) != 0)
    std::abort();
  --live;
  std::free(block);
}

struct Node {
  long value[3];
  virtual ~Node() = default;
};
struct Leaf : Node {
  long more[5];
};

int main() {
  Node *leaf = new Leaf(); // a sized delete, through the virtual destructor
  delete leaf;
  int *array = new int[40]; // operator new[], then operator delete[]
  array[39] = 1;
  delete[] array;
  int *quiet = new (std::nothrow) int(3);
  delete quiet;
  {
    std::vector<std::string> words(20, std::string(40, 'x'));
    words.resize(3);
  }
  return live == 0 ? 0 : 1;
}
