//===- Induction.cpp - Recursive proof progress (RFC 0029) ---------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Core/Induction.h"

#include <vector>

namespace weavec::core {

bool validInductionProgress(unsigned members,
                            std::span<const InductionEdge> edges) {
  if (members == 0 || members > 32 || edges.size() > 256)
    return false;
  std::vector<unsigned> incoming(members);
  std::vector<std::vector<unsigned>> next(members);
  for (const auto &edge : edges) {
    if (edge.caller >= members || edge.callee >= members)
      return false;
    if (!edge.strict) {
      next[edge.caller].push_back(edge.callee);
      ++incoming[edge.callee];
    }
  }
  std::vector<unsigned> ready;
  for (unsigned i = 0; i < members; ++i)
    if (incoming[i] == 0)
      ready.push_back(i);
  for (unsigned i = 0; i < ready.size(); ++i)
    for (const auto callee : next[ready[i]])
      if (--incoming[callee] == 0)
        ready.push_back(callee);
  return ready.size() == members;
}

} // namespace weavec::core
