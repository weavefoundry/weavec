//===- AttributeReaderTest.cpp - Tests for AttributeReader and KindTable --===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §7.2: declared kinds, their precedence, and the level-4 marking
// of system-header attributes.
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/AttributeReader.h"

#include "SiteTestUtils.h"
#include "weavec/Analysis/KindTable.h"

#include <gtest/gtest.h>

#include <string>

namespace weavec::analysis {

using test::collectUnit;

/// `<kind spelling> <shape level>/<nullability level>` of an entry.
static std::string spell(const KindEntry *entry) {
  if (entry == nullptr)
    return "none";
  const auto level = [](const std::optional<KindLevel> &which) {
    return which ? std::string(toString(*which)) : std::string("-");
  };
  return entry->kind.toString() + " " + level(entry->shapeLevel) + "/" +
         level(entry->nullabilityLevel);
}

static std::string param(const test::CollectedUnit &unit,
                         llvm::StringRef function, unsigned index) {
  const clang::FunctionDecl *decl = unit.function(function);
  return decl == nullptr ? "no function"
                         : spell(unit.kinds.param(*decl, index));
}

namespace {

TEST(AttributeReader, WeaveCAnnotations) {
  const auto unit = collectUnit(R"c(
void f(int *SIZED_BY(n) p, int n, char *SIZED_BY(n) c, void *SIZED_BY(n) v,
       int *NONNULL q, int *NULLABLE r, int *SIZED_BY(missing) bad);
NONNULL char *g(void);
void use(void) { f(0, 0, 0, 0, 0, 0, 0); (void)g(); }
)c");
  EXPECT_EQ(param(unit, "f", 0),
            "counted(param 1 scale 1 plus 0) nullable annotation/-");
  EXPECT_EQ(param(unit, "f", 2),
            "sized(param 1 scale 1 plus 0) nullable annotation/-");
  EXPECT_EQ(param(unit, "f", 3),
            "sized(param 1 scale 1 plus 0) nullable annotation/-");
  EXPECT_EQ(param(unit, "f", 4), "unknown nonnull -/annotation");
  EXPECT_EQ(param(unit, "f", 5), "unknown nullable -/annotation");
  EXPECT_EQ(param(unit, "f", 6), "none");
  EXPECT_EQ(spell(unit.kinds.result(*unit.function("g"))),
            "unknown nonnull -/annotation");
  ASSERT_EQ(unit.kinds.problems().size(), 1U);
  EXPECT_EQ(unit.kinds.problems().front().message,
            "'missing' in WEAVEC_SIZED_BY does not name a parameter or field");
  const KindEntry *counted = unit.kinds.param(*unit.function("f"), 0);
  ASSERT_NE(counted, nullptr);
  EXPECT_EQ(counted->extentClass, core::ExtentClass::Declared);
  EXPECT_TRUE(counted->isCheckOperand());
  EXPECT_TRUE(counted->hasShape());
  EXPECT_EQ(counted->kind.source, core::KindSource::Declared);
}

TEST(AttributeReader, EcosystemAttributes) {
  const auto unit = collectUnit(R"c(
void all(int *a, int *b) __attribute__((nonnull));
void some(int *a, int *b) __attribute__((nonnull(2)));
void param(int *a __attribute__((nonnull)), int *_Nonnull b, int *_Nullable c);
void arrays(int n, int s[static 4], int v[n], int w[n + 1], int k[4],
            int t[static n]);
int *ret(void) __attribute__((returns_nonnull));
void *alloc(unsigned long n, unsigned long size) __attribute__((alloc_size(1, 2)));
void use(void) { all(0, 0); some(0, 0); param(0, 0, 0); arrays(0, 0, 0, 0, 0, 0); (void)ret(); (void)alloc(1, 1); }
)c");
  EXPECT_EQ(param(unit, "all", 0), "unknown nonnull -/ecosystem");
  EXPECT_EQ(param(unit, "all", 1), "unknown nonnull -/ecosystem");
  EXPECT_EQ(param(unit, "some", 0), "none");
  EXPECT_EQ(param(unit, "some", 1), "unknown nonnull -/ecosystem");
  EXPECT_EQ(param(unit, "param", 0), "unknown nonnull -/ecosystem");
  EXPECT_EQ(param(unit, "param", 1), "unknown nonnull -/ecosystem");
  EXPECT_EQ(param(unit, "param", 2), "unknown nullable -/ecosystem");
  EXPECT_EQ(param(unit, "arrays", 1), "counted(4) nonnull ecosystem/ecosystem");
  EXPECT_EQ(param(unit, "arrays", 2),
            "counted(param 0 scale 1 plus 0) nullable ecosystem/-");
  EXPECT_EQ(param(unit, "arrays", 3),
            "counted(param 0 scale 1 plus 1) nullable ecosystem/-");
  // A constant bound without `static` is no requirement (§7.2).
  EXPECT_EQ(param(unit, "arrays", 4), "none");
  EXPECT_EQ(param(unit, "arrays", 5),
            "counted(param 0 scale 1 plus 0) nonnull ecosystem/ecosystem");
  EXPECT_EQ(spell(unit.kinds.result(*unit.function("ret"))),
            "unknown nonnull -/ecosystem");
  const KindEntry *allocated = unit.kinds.result(*unit.function("alloc"));
  EXPECT_EQ(spell(allocated),
            "sized(param 0 scale 1 plus 0) nullable ecosystem/-");
  ASSERT_NE(allocated, nullptr);
  EXPECT_EQ(allocated->extentFactor, std::optional<std::uint32_t>(1));
}

TEST(AttributeReader, Fields) {
  const auto unit = collectUnit(R"c(
struct s {
  int n;
  int *__attribute__((counted_by(n))) counted;
  char *__attribute__((sized_by_or_null(n))) sized;
  int *SIZED_BY(n) annotated;
  char *SIZED_BY(n) bytes;
  int *_Nonnull never;
  int tail[] __attribute__((counted_by(n)));
};
)c");
  const clang::RecordDecl *record = nullptr;
  for (const clang::Decl *decl :
       unit.context().getTranslationUnitDecl()->decls())
    if (const auto *tag = llvm::dyn_cast<clang::RecordDecl>(decl);
        tag != nullptr && tag->getName() == "s")
      record = tag;
  ASSERT_NE(record, nullptr);
  std::vector<std::string> kinds;
  for (const clang::FieldDecl *field : record->fields())
    kinds.push_back(field->getNameAsString() + ": " +
                    spell(unit.kinds.field(*field)));
  EXPECT_EQ(
      kinds,
      (std::vector<std::string>{
          "n: none",
          "counted: counted(.n scale 1 plus 0) nonnull ecosystem/ecosystem",
          "sized: sized(.n scale 1 plus 0) nullable ecosystem/ecosystem",
          "annotated: counted(.n scale 1 plus 0) nullable annotation/-",
          "bytes: sized(.n scale 1 plus 0) nullable annotation/-",
          "never: unknown nonnull -/ecosystem",
          "tail: counted(.n scale 1 plus 0) nullable ecosystem/-"}));
}

// §7.2 precedence: WEAVEC_* annotations, then ecosystem attributes; shape
// and nullability separately; a conflict within a level uses the weaker
// kind and is recorded.
TEST(AttributeReader, Precedence) {
  const auto unit = collectUnit(R"c(
void a(int *NULLABLE p __attribute__((nonnull)));
void b(int n, int p[static 4] SIZED_BY(n));
void c(int *SIZED_BY(n) p, int n, int m);
void c(int *SIZED_BY(m) p, int n, int m);
void d(int *NONNULL p);
void d(int *NULLABLE p);
void use(void) { a(0); b(0, 0); c(0, 0, 0); d(0); }
)c");
  EXPECT_EQ(param(unit, "a", 0), "unknown nullable -/annotation");
  EXPECT_EQ(param(unit, "b", 1),
            "counted(param 0 scale 1 plus 0) nonnull annotation/ecosystem");
  EXPECT_EQ(param(unit, "c", 0), "unknown nullable annotation/-");
  EXPECT_EQ(param(unit, "d", 0), "unknown nullable -/annotation");
  EXPECT_EQ(unit.kinds.problems().size(), 2U);
}

// §7.2 level 4: attributes in system headers mark their kinds; a
// LibrarySpec row outranks them.
TEST(AttributeReader, SystemHeaders) {
  const auto unit = collectUnit(R"c(
#include <sys.h>
void use(char *d, const char *s) { sysfn(d); (void)memcpy(d, s, 1); }
)c",
                                R"c(
typedef unsigned long size_t;
void sysfn(char *p) __attribute__((nonnull));
void *memcpy(void *, const void *, size_t) __attribute__((nonnull));
)c",
                                /*prelude=*/false);
  const KindEntry *system = unit.kinds.param(*unit.function("sysfn"), 0);
  EXPECT_EQ(spell(system), "unknown nonnull -/system-header");
  ASSERT_NE(system, nullptr);
  EXPECT_TRUE(system->nullabilityFromSystemHeader());
  EXPECT_FALSE(system->isCheckOperand());
  EXPECT_TRUE(unit.kinds.governedByLibrary(*unit.function("memcpy")));
  EXPECT_EQ(param(unit, "memcpy", 0), "none");
  // The call site's null facet rests on the level-4 attribute alone.
  const SiteIndex::FunctionSites *function =
      unit.sites.function(*unit.function("use"));
  ASSERT_NE(function, nullptr);
  ASSERT_FALSE(function->sites.empty());
  EXPECT_EQ(function->sites.front().kind, core::SiteKind::Call);
  EXPECT_TRUE(function->sites.front().nullSystemApi);
}

TEST(KindTable, EntryHelpers) {
  KindEntry entry;
  EXPECT_FALSE(entry.hasShape());
  EXPECT_FALSE(entry.isNonnull());
  entry.kind = core::PointerKind::counted(core::ExtentTerm::constant(4),
                                          core::Nullability::Nonnull,
                                          core::KindSource::Declared);
  entry.extentClass = core::extentClassOf(entry.kind);
  entry.shapeLevel = KindLevel::SystemHeader;
  EXPECT_TRUE(entry.hasShape());
  EXPECT_TRUE(entry.isNonnull());
  EXPECT_TRUE(entry.shapeFromSystemHeader());
  EXPECT_FALSE(entry.isCheckOperand());
  entry.shapeLevel = KindLevel::Ecosystem;
  EXPECT_TRUE(entry.isCheckOperand());
  entry.kind = core::PointerKind::single();
  entry.extentClass = core::extentClassOf(entry.kind);
  EXPECT_FALSE(entry.isCheckOperand());
  EXPECT_EQ(toString(KindLevel::LibrarySpec), "library-spec");
}

} // namespace
} // namespace weavec::analysis
