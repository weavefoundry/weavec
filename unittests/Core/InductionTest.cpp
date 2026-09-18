//===- InductionTest.cpp - Recursive progress oracle (RFC 0029) ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Core/Induction.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace weavec::core {

TEST(Induction, EveryCycleMustContainProgress) {
  // Exhaust every absent/non-strict/strict edge assignment for three members.
  // The independent oracle computes transitive closure of non-strict edges.
  for (unsigned encoding = 0; encoding < 19683; ++encoding) {
    unsigned remaining = encoding;
    std::array<std::array<bool, 3>, 3> reachable{};
    std::vector<InductionEdge> edges;
    for (unsigned from = 0; from < 3; ++from)
      for (unsigned to = 0; to < 3; ++to) {
        const auto kind = remaining % 3;
        remaining /= 3;
        if (kind)
          edges.push_back({.caller = from, .callee = to, .strict = kind == 2});
        reachable.at(from).at(to) = kind == 1;
      }
    for (unsigned via = 0; via < 3; ++via)
      for (unsigned from = 0; from < 3; ++from)
        for (unsigned to = 0; to < 3; ++to)
          reachable.at(from).at(to) |=
              reachable.at(from).at(via) && reachable.at(via).at(to);
    const bool acyclic = !reachable.at(0).at(0) && !reachable.at(1).at(1) &&
                         !reachable.at(2).at(2);
    ASSERT_EQ(validInductionProgress(3, edges), acyclic) << encoding;
  }
}

TEST(Induction, MissingMembersAndExhaustedBoundsSupplyNoProof) {
  EXPECT_FALSE(validInductionProgress(0, {}));
  EXPECT_TRUE(validInductionProgress(32, {}));
  EXPECT_FALSE(validInductionProgress(33, {}));
  const std::array bad{InductionEdge{.caller = 0, .callee = 1, .strict = true}};
  EXPECT_FALSE(validInductionProgress(1, bad));
  std::vector<InductionEdge> edges(256, {.strict = true});
  EXPECT_TRUE(validInductionProgress(1, edges));
  edges.push_back({.strict = true});
  EXPECT_FALSE(validInductionProgress(1, edges));
}

} // namespace weavec::core
