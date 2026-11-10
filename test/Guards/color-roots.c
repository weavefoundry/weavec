// RFC 0038 §3: a color check's root is the pointer the program computed
// from, through the optimizer's integer arithmetic, phis and selects.
//
// zstd's double-fast match finder reads `dictBase + index - delta` or
// `base + index`; the optimizer selects between `dictBase - delta` and
// `base` and adds the index after. That biased value is no pointer the
// program holds, and it can be another block's start: the access, inside
// the dictionary's block, runs clean. An address computed through integer
// arithmetic on one pointer is checked against that pointer's object, as is
// one computed in a loop from a pointer that a phi carries.
//
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t zstd 1
// RUN: %t integer 0
// RUN: not --crash %t integer 1 2>&1 | FileCheck --check-prefix=INTEGER %s
// RUN: %t loop 0
// RUN: not --crash %t loop 1 2>&1 | FileCheck --check-prefix=LOOP %s
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck --check-prefix=IR %s
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static unsigned read4(const unsigned char *p) {
  unsigned value;
  memcpy(&value, p, sizeof value);
  return value;
}

// The optimizer hoists `dictBase - delta` out of the loop and selects it
// or `base` before adding the index.
// IR-LABEL: define {{.*}}@match(
// IR: [[BIASED:%[a-zA-Z0-9._]+]] = getelementptr i8, ptr %dictBase, i64 %{{[a-zA-Z0-9._]+}}
// IR: select i1 %{{[a-zA-Z0-9._]+}}, ptr [[BIASED]], ptr %base
// IR-NOT: __weavec_rt_jump
// IR-LABEL: define {{.*}}@integer(
// IR: __weavec_rt_jump
__attribute__((noinline)) unsigned match(const unsigned char *dictBase,
                                         const unsigned char *base,
                                         unsigned first, unsigned n,
                                         unsigned lowest, unsigned delta) {
  unsigned sum = 0;
  for (unsigned index = first; index < first + n; index += 4) {
    const unsigned char *at =
        index < lowest ? dictBase + index - delta : base + index;
    sum += read4(at);
  }
  return sum;
}

__attribute__((noinline)) unsigned integer(unsigned char *p, long offset) {
  return read4((const unsigned char *)((uintptr_t)p + (uintptr_t)offset));
}

__attribute__((noinline)) unsigned loop(unsigned char *p, long n, long jump) {
  unsigned sum = 0;
  for (unsigned char *at = p; at < p + n; at += 4)
    sum += read4(at + jump);
  return sum;
}

static volatile long zero;

int main(int argc, char **argv) {
  // Two neighbors of one size class: the second is the dictionary.
  unsigned char *window = malloc(64);
  unsigned char *dictionary = malloc(64);
  const long apart = dictionary - window;
  const long far = atol(argv[2]) != 0 ? apart : 0;
  memset(window, 1, 64);
  memset(dictionary, 2, 64);
  if (argc < 3)
    return 2;
  if (strcmp(argv[1], "zstd") == 0)
    // dictBase - delta is the window's start.
    return match(dictionary, window, (unsigned)apart + 8, 16, 1u << 30,
                 (unsigned)apart + zero) != 4 * 0x02020202u;
  if (strcmp(argv[1], "integer") == 0)
    // INTEGER: weavec: heap-buffer-overflow at {{.*}}color-roots.c:{{[0-9]+}}:{{[0-9]+}}: read of 4 bytes
    // INTEGER: weavec: 0x{{[0-9a-f]+}} is {{[0-9]+}} bytes after its base 0x{{[0-9a-f]+}}, in another object
    return integer(window, far + 8) == 0;
  // LOOP: weavec: heap-buffer-overflow at {{.*}}color-roots.c:{{[0-9]+}}:{{[0-9]+}}: read of 4 bytes
  // LOOP: weavec: 0x{{[0-9a-f]+}} is {{[0-9]+}} bytes after its base 0x{{[0-9a-f]+}}, in another object
  return loop(window, 16, far) == 0;
}
