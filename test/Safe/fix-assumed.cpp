// RFC 0051 §4.1: an `assumed` error's fix is certain, and `--weavec-fix`
// applies it: an attribute alone in its brackets goes with them, `[[assume]]`
// on an empty statement goes with its `;`, `__builtin_assume_aligned` leaves
// its pointer and `assume_safety` becomes `enable`. Afterwards the unit
// compiles.
// RUN: rm -rf %t && mkdir -p %t && cp %S/Inputs/fix-assumed.cpp %t/source.cpp
// RUN: cd %t && %weavec_cxx --weavec-fix -- %weavec_cxx -std=c++23 -fweavec-safe -c source.cpp 2>&1 | FileCheck %s --check-prefix=FIX
// RUN: FileCheck %s --check-prefix=SOURCE < %t/source.cpp
// RUN: cd %t && %weavec_cxx -std=c++23 -fweavec-safe -fsyntax-only source.cpp
// FIX: weavec: fix: round 1: 4 edits in 1 files (4 certain, 0 likely); 0 errors after
// SOURCE: long at(const std::array<long, 8> &cells, std::size_t i) {
// SOURCE-NEXT: {{^  $}}
// SOURCE-NEXT: return cells[i];
// SOURCE: const long *first(const std::vector<long> &v) {
// SOURCE: return static_cast<const long *>(v.data());
// SOURCE: #pragma clang loop vectorize(enable)
