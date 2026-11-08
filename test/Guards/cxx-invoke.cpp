// RFC 0039 §2: a call that can unwind (an invoke: here a call of a C
// library function made while an object with a destructor is alive) is
// guarded as a call is (unittests/Frontend/GuardPassTest.cpp pins a check
// after one, on its normal path).
// RUN: %weavec_cxx -O1 -S -emit-llvm %s -o - | FileCheck --check-prefix=IR %s
// RUN: %weavec_cxx -O1 %s -o %t
// RUN: %t 8
// RUN: not --crash %t 64 2>&1 | FileCheck %s
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>

// IR-LABEL: define {{.*}}@main(
// IR: call {{.*}}@__weavec_rt_range(
// IR: invoke {{.*}}read{{.*}}(
int main(int argc, char **argv) {
  std::string keep(40, 'k');
  char *buffer = static_cast<char *>(malloc(8));
  int fd = open("/dev/zero", O_RDONLY);
  // CHECK: weavec: heap-buffer-overflow at {{.*}}cxx-invoke.cpp:[[@LINE+1]]:{{[0-9]+}}: write of 64 bytes
  ssize_t got = read(fd, buffer, static_cast<size_t>(atoi(argv[1])));
  return got < 0 || keep.size() != 40;
}
