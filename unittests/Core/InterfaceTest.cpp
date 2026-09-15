//===- InterfaceTest.cpp - Portable C metadata (RFC 0028) -----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Interface.h"

#include <gtest/gtest.h>

namespace weavec::core {

static InterfaceType chainInterface() {
  InterfaceNode record;
  record.kind = InterfaceKind::Record;
  record.bytes = 16;
  record.alignment = 8;
  record.name = "node";
  record.view = "node-layout";
  record.fields = {{.name = "value", .type = 1, .offset = 0},
                   {.name = "next", .type = 2, .offset = 8}};
  InterfaceNode integer;
  integer.kind = InterfaceKind::Integer;
  integer.bytes = 4;
  integer.alignment = 4;
  integer.name = "unsigned int";
  InterfaceNode pointer;
  pointer.kind = InterfaceKind::Pointer;
  pointer.bytes = 8;
  pointer.alignment = 8;
  pointer.element = 0;
  return {{record, integer, pointer}};
}

TEST(InterfaceType, CyclicPointerGraphsRoundTripCanonically) {
  const auto type = chainInterface();
  ASSERT_TRUE(type.valid());
  const auto text = type.encode();
  ASSERT_FALSE(text.empty());
  EXPECT_EQ(InterfaceType::decode(text), type);
  for (std::size_t i = 0; i < text.size(); ++i)
    EXPECT_FALSE(InterfaceType::decode(text.substr(0, i))) << i;
  EXPECT_FALSE(InterfaceType::decode(text + " "));
  EXPECT_FALSE(InterfaceType::decode("it1;03;" + text.substr(6)));
  EXPECT_FALSE(InterfaceType::decode("it0;" + text.substr(4)));
  EXPECT_FALSE(InterfaceType::decode("it1;18446744073709551616;"));
  EXPECT_FALSE(InterfaceType::decode(std::string(MaxInterfaceBytes + 1, 'x')));
}

TEST(InterfaceType, InvalidEdgesLayoutsAndByValueCyclesAreRejected) {
  auto type = chainInterface();
  type.nodes[0].fields[1].type = 3;
  EXPECT_FALSE(type.valid());
  type = chainInterface();
  type.nodes[2].element = 3;
  EXPECT_FALSE(type.valid());
  type = chainInterface();
  type.nodes[0].fields[1].type = 0;
  EXPECT_FALSE(type.valid());
  type = chainInterface();
  type.nodes[0].fields[1].offset = 2;
  EXPECT_FALSE(type.valid());
  type = chainInterface();
  type.nodes[0].fields[1].offset = 16;
  EXPECT_FALSE(type.valid());
  type = chainInterface();
  type.nodes[0].fields[1].name = "value";
  EXPECT_FALSE(type.valid());
  type = chainInterface();
  type.nodes[0].alignment = 3;
  EXPECT_FALSE(type.valid());
  type = chainInterface();
  type.nodes[0].fields[1].name = "next\n";
  EXPECT_FALSE(type.valid());
  type = chainInterface();
  type.nodes[1].qualifiers = 8;
  EXPECT_FALSE(type.valid());
  type = chainInterface();
  // Deliberately malformed in-memory metadata must fail before encoding.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  type.nodes[1].kind = static_cast<InterfaceKind>(255);
  EXPECT_FALSE(type.valid());
  EXPECT_TRUE(type.encode().empty());
}

TEST(InterfaceType, ArraysValidateCountsAndElementLayoutWithoutOverflow) {
  auto type = chainInterface();
  InterfaceNode array;
  array.kind = InterfaceKind::Array;
  array.element = 1;
  array.count = 4;
  array.bytes = 16;
  array.alignment = 4;
  type.nodes.push_back(array);
  ASSERT_TRUE(type.valid());
  EXPECT_EQ(InterfaceType::decode(type.encode()), type);
  type.nodes.back().count = UINT64_MAX;
  EXPECT_FALSE(type.valid());
  type.nodes.back().count = 0;
  EXPECT_FALSE(type.valid());
  type.nodes.back().count = 4;
  type.nodes.back().alignment = 8;
  EXPECT_FALSE(type.valid());
}

TEST(InterfaceType, IncompleteRecordsAreAllowedOnlyBehindPointers) {
  auto type = chainInterface();
  type.nodes[0].bytes = 0;
  type.nodes[0].alignment = 0;
  EXPECT_FALSE(type.valid());
  type.nodes[0].fields.clear();
  type.nodes[0].view.clear();
  EXPECT_TRUE(type.valid());
  EXPECT_EQ(InterfaceType::decode(type.encode()), type);
}

TEST(InterfaceType, ConflictsAreAbsorbingAndOrderIndependent) {
  const auto first = chainInterface();
  auto second = first;
  second.nodes[1].name = "int";
  const InterfaceTypes a{{"node", first}};
  const InterfaceTypes b{{"node", second}};
  auto left = a;
  mergeInterfaceTypes(left, b);
  EXPECT_FALSE(left.at("node"));
  mergeInterfaceTypes(left, a);
  EXPECT_FALSE(left.at("node"));
  auto right = b;
  mergeInterfaceTypes(right, a);
  EXPECT_EQ(left, right);
  auto same = a;
  mergeInterfaceTypes(same, a);
  EXPECT_EQ(same, a);
  mergeInterfaceTypes(same, {{"other", second}});
  EXPECT_EQ(same.size(), 2U);
  EXPECT_EQ(same.at("node"), first);
}

TEST(InterfaceType, DescriptorBudgetsAreFinite) {
  auto type = chainInterface();
  type.nodes.resize(MaxInterfaceNodes + 1);
  EXPECT_FALSE(type.valid());
  type = chainInterface();
  type.nodes[0].fields.resize(MaxInterfaceFields + 1);
  EXPECT_FALSE(type.valid());
  type = chainInterface();
  type.nodes[0].view.resize(MaxInterfaceBytes, 'x');
  EXPECT_FALSE(type.valid());
}

} // namespace weavec::core
