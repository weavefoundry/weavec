//===- PointerKindTest.cpp - Tests for the RFC 0030 kind lattice ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/PointerKind.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace weavec::core {

/// `param 1 scale 1 plus 0`.
static ExtentTerm paramOne() {
  return ExtentTerm::of(ExtentPath::ofParam(1));
}

TEST(PointerKind, SpellsEveryShape) {
  EXPECT_EQ(PointerKind::single(Nullability::Nonnull).toString(),
            "single nonnull");
  EXPECT_EQ(PointerKind::counted(paramOne()).toString(),
            "counted(param 1 scale 1 plus 0) nullable");
  EXPECT_EQ(PointerKind::counted(ExtentTerm::constant(16), Nullability::Nonnull)
                .toString(),
            "counted(16) nonnull");
  EXPECT_EQ(PointerKind::sized(ExtentTerm::of(ExtentPath::ofField("len"), 4, 0))
                .toString(),
            "sized(.len scale 4 plus 0) nullable");
  EXPECT_EQ(
      PointerKind::endedBy(ExtentPath::ofParam(2), 0, Nullability::Nonnull)
          .toString(),
      "ended-by(param 2) nonnull");
  EXPECT_EQ(PointerKind::endedBy(ExtentPath::ofParam(2), 1).toString(),
            "ended-by(param 2 scale 1 plus 1) nullable");
  EXPECT_EQ(PointerKind::nulTerminated().toString(), "nul-terminated nullable");
  EXPECT_EQ(PointerKind::unknown().toString(), "unknown nullable");
  // argv of main: counted(argc + 1), nonnull (§7.3).
  EXPECT_EQ(PointerKind::counted(ExtentTerm::of(ExtentPath::ofParam(0), 1, 1),
                                 Nullability::Nonnull, KindSource::Declared)
                .toString(),
            "counted(param 0 scale 1 plus 1) nonnull");
}

TEST(PointerKind, SpellingsRoundTrip) {
  const std::vector<PointerKind> kinds{
      PointerKind::single(Nullability::Nonnull),
      PointerKind::counted(paramOne()),
      PointerKind::counted(ExtentTerm::constant(-3)),
      PointerKind::sized(ExtentTerm::of(ExtentPath::ofField("n_bytes"), 8, -1),
                         Nullability::Nonnull),
      PointerKind::endedBy(ExtentPath::ofField("end")),
      PointerKind::endedBy(ExtentPath::ofParam(3), 1, Nullability::Nonnull),
      PointerKind::nulTerminated(Nullability::Nonnull),
      PointerKind::unknown(),
  };
  for (const PointerKind &kind : kinds) {
    const auto parsed = PointerKind::parse(kind.toString());
    ASSERT_TRUE(parsed) << kind.toString();
    EXPECT_EQ(*parsed, kind) << kind.toString();
  }
}

TEST(PointerKind, ParsingAssignsTheSource) {
  const auto parsed =
      PointerKind::parse("single nonnull", KindSource::Declared);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->source, KindSource::Declared);
}

TEST(PointerKind, ParsingIsLenientOnlyAboutBarePaths) {
  EXPECT_EQ(PointerKind::parse("counted(param 1) nonnull"),
            PointerKind::counted(paramOne(), Nullability::Nonnull));
  EXPECT_EQ(PointerKind::parse("sized(.len) nullable"),
            PointerKind::sized(ExtentTerm::of(ExtentPath::ofField("len"))));
  for (const std::string_view bad :
       {"single", "single maybe", "counted() nonnull", "counted nonnull",
        "counted(param x) nonnull", "counted(param 1 scale 1 plus) nonnull",
        "counted(param 1 scale 1 minus 0) nonnull", "ended-by(16) nonnull",
        "sized(.1bad) nonnull", "sized(.) nonnull", "counted(param  1) nonnull",
        "unknown  nullable", "bogus nonnull", "counted(param -1) nonnull"})
    EXPECT_FALSE(PointerKind::parse(bad)) << bad;
}

TEST(PointerKind, ExtentTerms) {
  EXPECT_EQ(ExtentTerm::constant(4).toString(), "4");
  EXPECT_TRUE(ExtentTerm::constant(4).isConstant());
  EXPECT_FALSE(paramOne().isConstant());
  EXPECT_EQ(ExtentTerm::parse("param 2 scale 4 plus -1"),
            ExtentTerm::of(ExtentPath::ofParam(2), 4, -1));
  EXPECT_EQ(ExtentTerm::parse("-7"), ExtentTerm::constant(-7));
  // The scale of a constant is meaningless.
  EXPECT_EQ((ExtentTerm{.path = std::nullopt, .scale = 9, .offset = 4}),
            ExtentTerm::constant(4));
  EXPECT_NE(ExtentTerm::of(ExtentPath::ofParam(2), 4, 0),
            ExtentTerm::of(ExtentPath::ofParam(2), 2, 0));
  EXPECT_EQ(ExtentPath::parse(".count"), ExtentPath::ofField("count"));
  EXPECT_EQ(ExtentPath::parse("param 7"), ExtentPath::ofParam(7));
  EXPECT_FALSE(ExtentPath::parse("param"));
  EXPECT_FALSE(ExtentPath::parse(".a.b"));
}

TEST(PointerKind, JoinOfEqualKindsIsTheKind) {
  const PointerKind kind = PointerKind::counted(
      paramOne(), Nullability::Nonnull, KindSource::Inferred);
  EXPECT_EQ(join(kind, kind), kind);
  EXPECT_EQ(join(PointerKind::nulTerminated(), PointerKind::nulTerminated()),
            PointerKind::nulTerminated());
}

TEST(PointerKind, JoinOfDifferentShapesOrTermsIsUnknown) {
  EXPECT_EQ(join(PointerKind::single(), PointerKind::nulTerminated()).shape,
            PointerShape::Unknown);
  EXPECT_EQ(join(PointerKind::counted(ExtentTerm::constant(4)),
                 PointerKind::counted(ExtentTerm::constant(8)))
                .shape,
            PointerShape::Unknown);
  EXPECT_EQ(
      join(PointerKind::counted(paramOne()), PointerKind::sized(paramOne()))
          .shape,
      PointerShape::Unknown);
  EXPECT_EQ(join(PointerKind::unknown(), PointerKind::single()).shape,
            PointerShape::Unknown);
}

TEST(PointerKind, SingleJoinsCountedWhenOneElementIsProven) {
  const PointerKind single = PointerKind::single(Nullability::Nonnull);
  EXPECT_EQ(join(single, PointerKind::counted(ExtentTerm::constant(4),
                                              Nullability::Nonnull))
                .shape,
            PointerShape::Single);
  EXPECT_EQ(join(PointerKind::counted(ExtentTerm::constant(1)), single).shape,
            PointerShape::Single);
  EXPECT_EQ(join(single, PointerKind::counted(ExtentTerm::constant(0))).shape,
            PointerShape::Unknown);
  // A term needs a proof at the join.
  EXPECT_EQ(join(single, PointerKind::counted(paramOne())).shape,
            PointerShape::Unknown);
  const JoinFacts facts{.provesAtLeastOne = [](const ExtentTerm &term) {
    return term == paramOne();
  }};
  EXPECT_EQ(join(single, PointerKind::counted(paramOne()), facts).shape,
            PointerShape::Single);
  EXPECT_EQ(join(PointerKind::counted(paramOne()), single, facts).shape,
            PointerShape::Single);
}

TEST(PointerKind, JoinOfNullabilityAndSource) {
  EXPECT_EQ(join(Nullability::Nonnull, Nullability::Nonnull),
            Nullability::Nonnull);
  EXPECT_EQ(join(Nullability::Nonnull, Nullability::Nullable),
            Nullability::Nullable);
  EXPECT_EQ(join(Nullability::Nullable, Nullability::Nonnull),
            Nullability::Nullable);
  EXPECT_EQ(join(KindSource::Declared, KindSource::Inferred),
            KindSource::Inferred);
  EXPECT_EQ(join(KindSource::Default, KindSource::Declared),
            KindSource::Default);
  const PointerKind declared = PointerKind::counted(
      paramOne(), Nullability::Nonnull, KindSource::Declared);
  const PointerKind inferred = PointerKind::counted(
      paramOne(), Nullability::Nullable, KindSource::Inferred);
  const PointerKind joined = join(declared, inferred);
  EXPECT_EQ(joined.shape, PointerShape::Counted);
  EXPECT_EQ(joined.nullability, Nullability::Nullable);
  EXPECT_EQ(joined.source, KindSource::Inferred);
  EXPECT_EQ(join(inferred, declared), joined);
}

TEST(PointerKind, RequirementsCombineByConjunction) {
  KindRequirements requirements;
  EXPECT_TRUE(requirements.empty());
  EXPECT_EQ(requirements.toString(), "unknown nullable");
  requirements.add(PointerKind::counted(paramOne()));
  requirements.add(PointerKind::single(Nullability::Nonnull));
  EXPECT_EQ(requirements.nullability(), Nullability::Nonnull);
  ASSERT_EQ(requirements.shapes().size(), 2U);
  // Every shape requirement carries the conjunction's nullability.
  EXPECT_EQ(requirements.toString(),
            "single nonnull & counted(param 1 scale 1 plus 0) nonnull");
  // `unknown` adds only its nullability; duplicates merge.
  requirements.add(PointerKind::unknown());
  requirements.add(PointerKind::counted(paramOne()));
  EXPECT_EQ(requirements.shapes().size(), 2U);
}

TEST(PointerKind, RequirementConjunctionIsCanonical) {
  const KindRequirements a(PointerKind::counted(ExtentTerm::constant(4)));
  KindRequirements b(PointerKind::counted(ExtentTerm::constant(8)));
  b.add(PointerKind::nulTerminated(Nullability::Nonnull));
  const KindRequirements ab = conjoin(a, b);
  EXPECT_EQ(ab, conjoin(b, a));
  // Two constants of one shape and source: the larger implies the smaller.
  EXPECT_EQ(ab.toString(), "counted(8) nonnull & nul-terminated nonnull");
  // Different sources are enforced differently and stay apart.
  KindRequirements c(PointerKind::counted(
      ExtentTerm::constant(4), Nullability::Nullable, KindSource::Declared));
  c.add(PointerKind::counted(ExtentTerm::constant(8)));
  EXPECT_EQ(c.shapes().size(), 2U);
  // Equal requirements keep the stronger source.
  KindRequirements d(PointerKind::counted(paramOne()));
  d.add(PointerKind::counted(paramOne(), Nullability::Nullable,
                             KindSource::Declared));
  ASSERT_EQ(d.shapes().size(), 1U);
  EXPECT_EQ(d.shapes()[0].source, KindSource::Declared);
}

TEST(PointerKind, RequirementSpellingsRoundTrip) {
  KindRequirements requirements(PointerKind::single(Nullability::Nonnull));
  requirements.add(PointerKind::endedBy(ExtentPath::ofParam(2), 1));
  const auto parsed = KindRequirements::parse(requirements.toString());
  ASSERT_TRUE(parsed);
  EXPECT_EQ(*parsed, requirements);
  const auto onlyNonnull = KindRequirements::parse("unknown nonnull");
  ASSERT_TRUE(onlyNonnull);
  EXPECT_FALSE(onlyNonnull->empty());
  EXPECT_TRUE(onlyNonnull->shapes().empty());
  EXPECT_EQ(onlyNonnull->nullability(), Nullability::Nonnull);
  EXPECT_FALSE(KindRequirements::parse("single nonnull &"));
  EXPECT_FALSE(KindRequirements::parse(""));
}

TEST(PointerKind, ExtentClasses) {
  EXPECT_FALSE(extentClassOf(PointerKind::unknown()));
  EXPECT_FALSE(extentClassOf(
      PointerKind::nulTerminated(Nullability::Nonnull, KindSource::Declared)));
  // Single is a lower bound, whatever its source.
  EXPECT_EQ(extentClassOf(PointerKind::single(Nullability::Nonnull,
                                              KindSource::Declared)),
            ExtentClass::LowerBound);
  EXPECT_EQ(extentClassOf(PointerKind::counted(
                paramOne(), Nullability::Nullable, KindSource::Declared)),
            ExtentClass::Declared);
  EXPECT_EQ(extentClassOf(PointerKind::counted(
                paramOne(), Nullability::Nullable, KindSource::Inferred)),
            ExtentClass::LowerBound);
  EXPECT_EQ(extentClassOf(PointerKind::endedBy(ExtentPath::ofParam(1), 0,
                                               Nullability::Nullable,
                                               KindSource::Default)),
            ExtentClass::LowerBound);
  EXPECT_TRUE(isCheckOperand(ExtentClass::Exact));
  EXPECT_TRUE(isCheckOperand(ExtentClass::Declared));
  EXPECT_FALSE(isCheckOperand(ExtentClass::LowerBound));
  EXPECT_TRUE(canProveViolation(ExtentClass::Exact));
  EXPECT_FALSE(canProveViolation(ExtentClass::Declared));
  EXPECT_EQ(toString(ExtentClass::LowerBound), "lower-bound");
}

TEST(PointerKind, EnumSpellings) {
  EXPECT_EQ(toString(PointerShape::EndedBy), "ended-by");
  EXPECT_EQ(toString(PointerShape::NulTerminated), "nul-terminated");
  EXPECT_EQ(parseNullability("nonnull"), Nullability::Nonnull);
  EXPECT_FALSE(parseNullability("nonnull "));
  EXPECT_EQ(toString(KindSource::Inferred), "inferred");
  EXPECT_EQ(parseKindSource("declared"), KindSource::Declared);
  EXPECT_FALSE(parseKindSource("Declared"));
}

} // namespace weavec::core
