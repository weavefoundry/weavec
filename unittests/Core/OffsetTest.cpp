//===- OffsetTest.cpp - Tests for PointerOffset ---------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Offset.h"

#include <gtest/gtest.h>

namespace weavec::core {
namespace {

// RFC 0011, *Pointer offsets*: composition.
TEST(PointerOffset, Composes) {
  const PointerOffset zero = PointerOffset::zero();
  const PointerOffset two = PointerOffset::ofElements(2);
  const PointerOffset minusOne = PointerOffset::ofElements(-1);
  const PointerOffset in = PointerOffset::ofField("struct outer .in");
  const PointerOffset unknown = PointerOffset::unknown();
  const PointerOffset inside = PointerOffset::inside();

  EXPECT_EQ(zero.plus(two), two);
  EXPECT_EQ(two.plus(zero), two);
  EXPECT_EQ(two.plus(minusOne), PointerOffset::ofElements(1));
  EXPECT_EQ(two.plus(PointerOffset::ofElements(-2)), zero);
  EXPECT_EQ(in.plus(in.negated()), zero) << "container_of undoes the field";
  EXPECT_EQ(in.negated().plus(in), zero);
  EXPECT_EQ(in.plus(in), inside) << "two field steps";
  EXPECT_EQ(in.plus(two), inside) << "an element step onto a field";
  EXPECT_EQ(inside.plus(two), inside) << "inside absorbs";
  EXPECT_EQ(unknown.plus(inside), inside);
  EXPECT_EQ(unknown.plus(zero), unknown);
  EXPECT_EQ(zero.plus(unknown), unknown);
  EXPECT_EQ(two.negated(), PointerOffset::ofElements(-2));
  EXPECT_EQ(zero.negated(), zero);
  EXPECT_EQ(unknown.negated(), unknown);
  EXPECT_EQ(inside.negated(), inside);
  EXPECT_EQ(PointerOffset::ofElements(0), zero);
  EXPECT_EQ(PointerOffset::ofElements(INT64_MAX).plus(two), unknown)
      << "saturates rather than wraps";
}

TEST(PointerOffset, JoinsToUnknownWhenTheSidesDiffer) {
  PointerOffset a = PointerOffset::ofElements(1);
  EXPECT_FALSE(a.join(PointerOffset::ofElements(1)));
  EXPECT_TRUE(a.join(PointerOffset::ofElements(2)));
  EXPECT_TRUE(a.isUnknown());
  EXPECT_FALSE(a.join(PointerOffset::zero())) << "unknown absorbs elements";
  PointerOffset stepped = PointerOffset::zero();
  EXPECT_TRUE(stepped.join(PointerOffset::ofElements(1)))
      << "`if (c) p++;`: an unknown element";
  EXPECT_TRUE(stepped.isUnknown());
  // A field against anything else is not an element of `*p`: inside.
  PointerOffset z = PointerOffset::zero();
  EXPECT_TRUE(z.join(PointerOffset::ofField("struct s .f")));
  EXPECT_TRUE(z.isInside());
  EXPECT_FALSE(z.join(PointerOffset::unknown())) << "inside absorbs";
  PointerOffset u = PointerOffset::unknown();
  EXPECT_TRUE(u.join(PointerOffset::inside()));
  EXPECT_TRUE(u.isInside());
  EXPECT_TRUE(u.isIndefinite());
}

TEST(PointerOffset, SpellsAndParses) {
  for (const PointerOffset &offset :
       {PointerOffset::zero(), PointerOffset::unknown(),
        PointerOffset::inside(), PointerOffset::ofElements(4),
        PointerOffset::ofElements(-2),
        PointerOffset::ofField("struct outer .in"),
        PointerOffset::ofField("struct outer .in", /*negative=*/true)}) {
    const auto parsed = PointerOffset::parse(offset.toString());
    ASSERT_TRUE(parsed) << offset.toString();
    EXPECT_EQ(*parsed, offset) << offset.toString();
  }
  EXPECT_EQ(PointerOffset::zero().toString(), "0");
  EXPECT_EQ(PointerOffset::unknown().toString(), "?");
  EXPECT_EQ(PointerOffset::inside().toString(), "~");
  EXPECT_EQ(PointerOffset::ofElements(4).toString(), "+4");
  EXPECT_EQ(PointerOffset::ofElements(-2).toString(), "-2");
  EXPECT_EQ(PointerOffset::ofField("struct outer .in").toString(),
            "+struct outer .in");
  EXPECT_EQ(PointerOffset::ofField("struct outer .in", true).toString(),
            "-struct outer .in");
  EXPECT_FALSE(PointerOffset::parse(""));
  EXPECT_FALSE(PointerOffset::parse("4"));
  EXPECT_FALSE(PointerOffset::parse("+"));
}

} // namespace
} // namespace weavec::core
