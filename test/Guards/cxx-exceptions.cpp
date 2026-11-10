// RFC 0039 §3: an exception marks only the frames it unwound as returned. A
// live caller's local keeps its redzones after a throw, a pointer into an
// unwound frame traps as a use after return, and a frame that later reuses
// the bytes (a callback from the C library, a stream buffer the C++
// library calls) runs clean. No clearing of the stack before a throw; a
// landing pad calls the runtime.
// RUN: %weavec_cxx -O1 -S -emit-llvm %s -o - | FileCheck --check-prefix=IR %s
// RUN: %weavec_cxx -O1 %s -o %t
// RUN: %t
// RUN: not --crash %t overflow 2>&1 | FileCheck --check-prefix=OVERFLOW %s
// RUN: not --crash %t unwound 2>&1 | FileCheck --check-prefix=UNWOUND %s
#include <cstdio>
#include <cstring>
#include <ftw.h>
#include <sstream>
#include <stdexcept>
#include <string>

// IR-NOT: call void @__weavec_rt_unpoison_stack
// IR: landingpad
// IR-NEXT: {{(cleanup|catch)}}
// IR: call void @__weavec_rt_landed()

static int *escaped;
__attribute__((noinline)) static void keep(int *p) { escaped = p; }

__attribute__((noinline)) static int risky(int depth) {
  int local[4] = {depth, 0, 0, 0};
  keep(local);
  std::string s(64, 'k');
  if (depth == 0)
    throw std::runtime_error(s);
  return risky(depth - 1) + local[0];
}

__attribute__((noinline)) static void fill(char *p, int n) {
  for (int i = 0; i < n; ++i)
    p[i] = 'x';
}

static int visit(const char *, const struct stat *, int, struct FTW *) {
  return 0;
}

int main(int argc, char **argv) {
  char buffer[16];
  for (int round = 0; round < 3; ++round) {
    try {
      (void)risky(5);
    } catch (const std::exception &) {
    }
  }
  // Frames of code built without WeaveC over the unwound bytes.
  (void)nftw("/dev/null", visit, 4, FTW_PHYS);
  std::ostringstream out;
  out << 12345 << ' ' << 2.5;
  if (argc > 1 && strcmp(argv[1], "overflow") == 0)
    // OVERFLOW: weavec: stack-buffer-overflow at {{.*}}cxx-exceptions.cpp:{{[0-9]+}}:{{[0-9]+}}: write of 1 bytes
    fill(buffer, 40);
  if (argc > 1 && strcmp(argv[1], "unwound") == 0) {
    try {
      (void)risky(0);
    } catch (const std::exception &) {
    }
    // UNWOUND: weavec: stack-use-after-return at {{.*}}cxx-exceptions.cpp:[[@LINE+2]]:{{[0-9]+}}: read of 4 bytes
    // UNWOUND: weavec: 0x{{[0-9a-f]+}} is in a frame that returned
    return escaped[0];
  }
  fill(buffer, 16);
  std::printf("%s %c\n", out.str().c_str(), buffer[0]);
  return 0;
}
