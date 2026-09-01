//===- OwnershipTest.cpp - Tests for the ownership lattice ----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Ownership.h"

#include <gtest/gtest.h>

#include <array>

namespace weavec::core {
namespace {

constexpr std::array<OwnershipKind, 5> AllKinds = {
    OwnershipKind::Unknown, OwnershipKind::Owned, OwnershipKind::Shared,
    OwnershipKind::Mutable, OwnershipKind::Raw};

TEST(OwnershipLattice, JoinIsIdempotent) {
  for (OwnershipKind k : AllKinds)
    EXPECT_EQ(join(k, k), k);
}

TEST(OwnershipLattice, JoinIsCommutative) {
  for (OwnershipKind a : AllKinds)
    for (OwnershipKind b : AllKinds)
      EXPECT_EQ(join(a, b), join(b, a));
}

TEST(OwnershipLattice, JoinIsAssociative) {
  for (OwnershipKind a : AllKinds)
    for (OwnershipKind b : AllKinds)
      for (OwnershipKind c : AllKinds)
        EXPECT_EQ(join(join(a, b), c), join(a, join(b, c)));
}

TEST(OwnershipLattice, UnknownIsBottom) {
  for (OwnershipKind k : AllKinds)
    EXPECT_EQ(join(OwnershipKind::Unknown, k), k);
}

TEST(OwnershipLattice, RawIsTop) {
  for (OwnershipKind k : AllKinds)
    EXPECT_EQ(join(OwnershipKind::Raw, k), OwnershipKind::Raw);
}

TEST(OwnershipLattice, ConflictingConcreteKindsJoinToRaw) {
  EXPECT_EQ(join(OwnershipKind::Owned, OwnershipKind::Shared),
            OwnershipKind::Raw);
  EXPECT_EQ(join(OwnershipKind::Shared, OwnershipKind::Mutable),
            OwnershipKind::Raw);
  EXPECT_EQ(join(OwnershipKind::Owned, OwnershipKind::Mutable),
            OwnershipKind::Raw);
}

TEST(OwnershipLattice, Predicates) {
  EXPECT_TRUE(isBorrow(OwnershipKind::Shared));
  EXPECT_TRUE(isBorrow(OwnershipKind::Mutable));
  EXPECT_FALSE(isBorrow(OwnershipKind::Owned));
  EXPECT_FALSE(isBorrow(OwnershipKind::Raw));

  EXPECT_TRUE(isSafe(OwnershipKind::Owned));
  EXPECT_TRUE(isSafe(OwnershipKind::Shared));
  EXPECT_FALSE(isSafe(OwnershipKind::Unknown));
  EXPECT_FALSE(isSafe(OwnershipKind::Raw));
}

TEST(OwnershipLattice, ToStringIsStable) {
  EXPECT_EQ(toString(OwnershipKind::Unknown), "unknown");
  EXPECT_EQ(toString(OwnershipKind::Owned), "owned");
  EXPECT_EQ(toString(OwnershipKind::Shared), "shared");
  EXPECT_EQ(toString(OwnershipKind::Mutable), "mutable");
  EXPECT_EQ(toString(OwnershipKind::Raw), "raw");
}

} // namespace
} // namespace weavec::core
