// RFC 0039 §4, amendment 18: an allocation the arena cannot serve fails as
// the C++ library's does: a nothrow form returns null, the others run the
// new-handler until it is removed and then throw std::bad_alloc.
// CLEAN
// FLAGS: -O1
#include <cstddef>
#include <new>

static int calls;

static void handler() {
  if (++calls == 2)
    std::set_new_handler(nullptr);
}

int main() {
  const volatile std::size_t huge = std::size_t(1) << 62;
  char *volatile quiet = new (std::nothrow) char[huge];
  if (quiet != nullptr)
    return 1;
  void *volatile raw = ::operator new(huge, std::nothrow);
  if (raw != nullptr)
    return 2;
  try {
    raw = ::operator new(huge);
    return 3;
  } catch (const std::bad_alloc &) {
  }
  std::set_new_handler(handler);
  try {
    quiet = new char[huge];
    return 4;
  } catch (const std::bad_alloc &) {
  }
  return calls == 2 ? 0 : 5;
}
