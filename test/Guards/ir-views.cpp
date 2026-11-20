// RFC 0050 §2: in a fast unit the library's checked views are trusted as
// its owners are. A safe function's index into a std::string_view or a
// std::span, `front`, `substr` and the element of a range-based `for` over
// a container, an array or a view read no shadow: the library's comparison
// is the check (§2.1, §2.4). An iterator held in a variable keeps its guard
// (§2.4); a unit whose library does not check (here libc++'s hardening set
// to `none`) trusts nothing (§2.5); verify mode keeps every trusted access
// as a monitor (§4).
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_NONE -U_GLIBCXX_ASSERTIONS -S -emit-llvm %s -o - | FileCheck %s --check-prefix=NONE
// A mode given as a number (2: none) is taken as none.
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -D_LIBCPP_HARDENING_MODE=2 -U_GLIBCXX_ASSERTIONS -S -emit-llvm %s -o - | FileCheck %s --check-prefix=NONE
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -fweavec-checks=verify -S -emit-llvm %s -o - | FileCheck %s --check-prefix=VERIFY
#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

// CHECK-LABEL: define {{.*}} @_Z6digits{{[^(]*}}(
// CHECK-NOT: __weavec_rt_shadow
// CHECK: ret
// NONE-LABEL: define {{.*}} @_Z6digits{{[^(]*}}(
// NONE: __weavec_rt_shadow
// VERIFY-LABEL: define {{.*}} @_Z6digits{{[^(]*}}(
// VERIFY: __weavec_rt_shadow
std::size_t digits(std::string_view text, std::size_t at) {
  while (at < text.size() && text[at] >= '0' && text[at] <= '9')
    ++at;
  return at;
}

// NONE-LABEL: define {{.*}} @_Z5ended{{[^(]*}}(
// CHECK-LABEL: define {{.*}} @_Z5ended{{[^(]*}}(
// CHECK-NOT: __weavec_rt_shadow
// CHECK: ret
char ended(std::string_view text) {
  return text.empty() ? '\0' : text.substr(1).front();
}

// CHECK-LABEL: define {{.*}} @_Z3sum{{[^(]*}}(
// CHECK-NOT: __weavec_rt_shadow
// CHECK: ret
long sum(std::span<const long> values) {
  long total = 0;
  for (std::size_t i = 0; i < values.size(); ++i)
    total += values[i];
  return total;
}

// CHECK-LABEL: define {{.*}} @_Z6looped{{[^(]*}}(
// CHECK-NOT: __weavec_rt_shadow
// CHECK: ret
long looped(const std::vector<long> &values, std::string_view text,
            const std::array<long, 4> &four) {
  long total = 0;
  for (long value : values)
    total += value;
  for (char c : text)
    total += c;
  for (long value : four)
    total += value;
  return total;
}

// CHECK-LABEL: define {{.*}} @_Z7stepped{{[^(]*}}(
// CHECK: __weavec_rt_shadow
long stepped(const std::vector<long> &values) {
  long total = 0;
  for (auto at = values.begin(); at != values.end(); ++at)
    total += *at;
  return total;
}

// RFC 0050 §3.1: a safe C++ unit lets released pooled slots keep their
// shadow; one in verify mode tells the runtime not to.
// CHECK-NOT: __weavec_rt_tests_released
// VERIFY: define internal void @weavec.released(
// VERIFY-NEXT: entry:
// VERIFY-NEXT: call void @__weavec_rt_tests_released()
