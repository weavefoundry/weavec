// The input of fix-assumed.cpp: what --weavec-fix edits.
#include <array>
#include <cstddef>
#include <vector>

long at(const std::array<long, 8> &cells, std::size_t i) {
  [[assume(i < 8)]];
  return cells[i];
}

[[gnu::returns_nonnull]] const long *first(const std::vector<long> &v) {
  return v.empty() ? nullptr : &v[0];
}

const long *aligned(const std::vector<long> &v) {
  return static_cast<const long *>(__builtin_assume_aligned(v.data(), 64));
}

long looped(const std::vector<long> &v) {
  long sum = 0;
#pragma clang loop vectorize(assume_safety)
  for (std::size_t i = 0; i < v.size(); ++i)
    sum += v[i];
  return sum;
}
