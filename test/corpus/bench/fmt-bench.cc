// The fmt workload of the corpus gate's benchmarks (RFC 0039, section 9):
// format integers, floating-point numbers, strings and pointers through
// fmt::format_to into a memory buffer, with positional, width, precision and
// named arguments, repeatedly.
//
//   fmt-bench [iterations]      (default 400)
//
// The gate builds this file with src/format.cc from the pinned checkout (see
// test/corpus/manifest.json) and compares the checksum line it prints
// between the reference and the WeaveC build.
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>

#include "fmt/format.h"

int main(int argc, char **argv) {
  const int iterations = argc > 1 ? std::atoi(argv[1]) : 400;
  unsigned long checksum = 0;
  unsigned long state = 12345;
  std::string names[] = {"alpha", "beta", "gamma", "delta", "epsilon"};
  for (int round = 0; round < iterations; ++round) {
    fmt::memory_buffer out;
    for (int i = 0; i < 2000; ++i) {
      state = state * 6364136223846793005UL + 1442695040888963407UL;
      const auto value = static_cast<long>(state >> 20);
      const double ratio = static_cast<double>(state % 100000) / 7.0;
      fmt::format_to(std::back_inserter(out), "{} {:>12} {:x} {:#o} ", i, value, value, i);
      fmt::format_to(std::back_inserter(out), "{:.3f} {:e} {:g} ", ratio, ratio, ratio * 1e10);
      fmt::format_to(std::back_inserter(out), "{1:<8}|{0:^9}|", names[i % 5], names[(i + 2) % 5]);
      fmt::format_to(std::back_inserter(out), "{name}={value:08d}\n", fmt::arg("name", names[i % 5]),
                     fmt::arg("value", i * 7));
    }
    const std::string text = fmt::to_string(out);
    for (unsigned char c : text)
      checksum = checksum * 31 + c;
    checksum += text.size();
  }
  std::printf("fmt-bench %d %lu\n", iterations, checksum);
  return 0;
}
