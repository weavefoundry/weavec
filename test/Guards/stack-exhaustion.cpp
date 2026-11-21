// RFC 0051 §3.3: a safe function's frame larger than a page is probed page
// by page, so a frame larger than its thread's stack stops at the guard
// page below the stack, and the fault handler, on the thread's alternate
// stack, says so (`stack-exhaustion`), on a thread that never allocated and
// in recursion that fills the main thread's stack.
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe %s -o %t
// RUN: %t 1 | FileCheck %s --check-prefix=OK
// RUN: not --crash %t 2 2>&1 | FileCheck %s --check-prefix=THREAD
// RUN: not --crash %t 3 2>&1 | FileCheck %s --check-prefix=DEEP
#include <array>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <string>
#include <weavec.h>

__attribute__((noinline)) long touch(long *slot, long seed) {
  *slot = seed;
  return seed + 1;
}

// THREAD: weavec: stack-exhaustion at {{.*}}: thread T1's stack of {{[0-9]+}} KiB is full
__attribute__((noinline)) long big(long seed) {
  std::array<long, 200000> table; // 1.6 MiB: more than a thread's stack
  return touch(&table[3], seed) + touch(&table[100000], seed);
}

// DEEP: weavec: stack-exhaustion at {{.*}}stack-exhaustion.cpp:{{[0-9]+}}{{(:[0-9]+)?}}: thread T0's stack of {{[0-9]+}} KiB is full
__attribute__((noinline)) long depth(const std::string &text, std::size_t at) {
  if (at >= text.size())
    return 0;
  std::array<long, 64> frame{};
  frame[at % 64] = static_cast<long>(at);
  // The frame lives across the call: the recursion is not a loop.
  const long below = depth(text, at + 1);
  touch(&frame[(at + 1) % 64], below);
  return frame[at % 64] + frame[(at + 1) % 64] - at + 1;
}

// A thread with a stack of 256 KiB on every target (Linux's default is the
// stack limit, 8 MiB), started by code that is not safe.
WEAVEC_GUARDED static void *runBig(void *result) {
  *static_cast<long *>(result) = big(7);
  return nullptr;
}
WEAVEC_GUARDED static long onSmallThread() {
  long result = 0;
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, 256 * 1024);
  pthread_t thread;
  pthread_create(&thread, &attributes, runBig, &result);
  pthread_join(thread, nullptr);
  return result;
}

int main(int argc, char **argv) {
  const int mode = argc > 1 ? std::atoi(argv[1]) : 1;
  long result = 0;
  if (mode == 2) {
    result = onSmallThread();
  } else if (mode == 3) {
    result = depth(std::string(100000000, 'x'), 0);
  } else {
    result = depth(std::string(100, 'x'), 0);
  }
  // OK: 100
  std::printf("%ld\n", result);
  return 0;
}
