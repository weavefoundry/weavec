//===- Induction.h - Recursive proof progress (RFC 0029) -------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef WEAVEC_CORE_INDUCTION_H
#define WEAVEC_CORE_INDUCTION_H

#include <span>

namespace weavec::core {

struct InductionEdge {
  unsigned caller = 0;
  unsigned callee = 0;
  bool strict = false;
};

/// Each edge must preserve one established well-founded measure, and a strict
/// edge must decrease it. Analysis proves those premises. This checks that
/// every possible cycle has a decrease, never just one favorable cycle.
[[nodiscard]] bool validInductionProgress(unsigned members,
                                          std::span<const InductionEdge> edges);

} // namespace weavec::core
#endif // WEAVEC_CORE_INDUCTION_H
