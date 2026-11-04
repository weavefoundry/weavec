// The generic __atomic_load the large-atomic cases call, defined here
// because glibc's lives in libatomic, which a link without -latomic lacks
// (Darwin's is in compiler-rt). The cases run on one thread, so a copy is
// atomic enough; the guards under test are those at the call.
#include <stddef.h>
#include <string.h>

void atomicLoad(size_t size, const void *source, void *destination,
                int order) __asm__("__atomic_load");
void atomicLoad(size_t size, const void *source, void *destination,
                int order) {
  (void)order;
  memcpy(destination, source, size);
}
