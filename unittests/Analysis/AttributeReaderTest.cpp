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
#include "weavec/Core/Diagnostic.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

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

/// The RFC 0030 macros, spelled as `weavec.h` spells them.
constexpr const char *KindMacros = R"c(
#define COUNTED_BY(n) __attribute__((annotate("weavec.counted_by." #n)))
#define ENDED_BY(q) __attribute__((annotate("weavec.ended_by." #q)))
#define STRING __attribute__((annotate("weavec.string")))
#define REQUIRE_SAFE __attribute__((annotate("weavec.require_safe")))
)c";

// §7.2: the new macros resolve by name to any sibling, in any position.
TEST(AttributeReader, KindMacros) {
  const auto unit = collectUnit(std::string(KindMacros) + R"c(
struct s {
  int *COUNTED_BY(n) items;
  void *COUNTED_BY(n) bytes;
  const char *STRING name;
  int *ENDED_BY(stop) start;
  int *stop;
  int n;
  int *COUNTED_BY(stop) wrong;
  int *ENDED_BY(n) wrong_end;
};
void f(int *COUNTED_BY(n) p, const char *STRING s, int *ENDED_BY(e) b,
       int *e, int n);
STRING const char *g(void);
void h(int *COUNTED_BY(nope) p, int COUNTED_BY(n) x, int n);
void use(void) { f(0, 0, 0, 0, 0); (void)g(); h(0, 0, 0); }
)c");
  EXPECT_EQ(param(unit, "f", 0),
            "counted(param 4 scale 1 plus 0) nullable annotation/-");
  EXPECT_EQ(param(unit, "f", 1), "nul-terminated nullable annotation/-");
  EXPECT_EQ(param(unit, "f", 2), "ended-by(param 3) nullable annotation/-");
  EXPECT_EQ(spell(unit.kinds.result(*unit.function("g"))),
            "nul-terminated nullable annotation/-");
  EXPECT_EQ(param(unit, "h", 0), "none");
  const clang::RecordDecl *record = nullptr;
  for (const clang::Decl *decl :
       unit.context().getTranslationUnitDecl()->decls())
    if (const auto *tag = llvm::dyn_cast<clang::RecordDecl>(decl);
        tag != nullptr && tag->getName() == "s")
      record = tag;
  ASSERT_NE(record, nullptr);
  std::vector<std::string> fields;
  for (const clang::FieldDecl *field : record->fields())
    fields.push_back(field->getNameAsString() + ": " +
                     spell(unit.kinds.field(*field)));
  EXPECT_EQ(fields,
            (std::vector<std::string>{
                "items: counted(.n scale 1 plus 0) nullable annotation/-",
                "bytes: sized(.n scale 1 plus 0) nullable annotation/-",
                "name: nul-terminated nullable annotation/-",
                "start: ended-by(.stop) nullable annotation/-", "stop: none",
                "n: none", "wrong: none", "wrong_end: none"}));
  std::vector<std::string> problems;
  for (const KindProblem &problem : unit.kinds.problems())
    problems.push_back(problem.message);
  EXPECT_EQ(problems,
            (std::vector<std::string>{
                "'stop' in WEAVEC_COUNTED_BY is not an integer parameter or "
                "field",
                "'n' in WEAVEC_ENDED_BY is not a pointer parameter or field",
                "'nope' in WEAVEC_COUNTED_BY does not name a parameter or "
                "field",
                "'x' is declared WEAVEC_COUNTED_BY(n) but is not a pointer"}));
  const KindEntry *counted = unit.kinds.param(*unit.function("f"), 0);
  ASSERT_NE(counted, nullptr);
  EXPECT_TRUE(counted->hasDeclaredShape());
  EXPECT_TRUE(counted->isCheckOperand());
}

// §6.3 and §7.2: `WEAVEC_REQUIRE_SAFE` marks the function; the ownership
// attributes become its contract; a constant array bound without `static`
// is a suggestion.
TEST(AttributeReader, RequireSafeContractsAndSuggestions) {
  const auto unit = collectUnit(std::string(KindMacros) + R"c(
REQUIRE_SAFE int strict(void) { return 0; }
int lax(void) { return 0; }
REQUIRE_SAFE int flag;
void *pool_get(unsigned long n) __attribute__((ownership_returns(pool)));
void pool_put(int t, void *p) __attribute__((ownership_takes(pool, 2)));
void pool_keep(void *p) __attribute__((ownership_holds(pool, 1)));
void *fresh(unsigned long n) __attribute__((malloc));
void loose(int k[4]) { (void)k; }
void use(void) { pool_put(0, pool_get(1)); pool_keep(fresh(1)); }
)c");
  EXPECT_TRUE(unit.kinds.requireSafe(*unit.function("strict")));
  EXPECT_FALSE(unit.kinds.requireSafe(*unit.function("lax")));
  ASSERT_EQ(unit.kinds.problems().size(), 1U);
  EXPECT_EQ(unit.kinds.problems().front().message,
            "WEAVEC_REQUIRE_SAFE on 'flag', which is not a function");
  const OwnershipContract *get =
      unit.kinds.ownership(*unit.function("pool_get"));
  ASSERT_NE(get, nullptr);
  EXPECT_EQ(get->freshResult, std::optional<std::string>("pool"));
  const OwnershipContract *put =
      unit.kinds.ownership(*unit.function("pool_put"));
  ASSERT_NE(put, nullptr);
  ASSERT_EQ(put->arguments.size(), 1U);
  EXPECT_EQ(put->arguments.front(),
            (OwnershipContract::Argument{
                .index = 1, .family = "pool", .retains = false}));
  const OwnershipContract *keep =
      unit.kinds.ownership(*unit.function("pool_keep"));
  ASSERT_NE(keep, nullptr);
  EXPECT_TRUE(keep->arguments.front().retains);
  const OwnershipContract *heap = unit.kinds.ownership(*unit.function("fresh"));
  ASSERT_NE(heap, nullptr);
  EXPECT_EQ(heap->freshResult, std::optional<std::string>("free"));
  EXPECT_EQ(heap->level, KindLevel::Ecosystem);
  ASSERT_EQ(unit.kinds.suggestions().size(), 1U);
  EXPECT_EQ(unit.kinds.suggestions().front().insert, "static ");
  EXPECT_EQ(param(unit, "loose", 0), "none");
  // The problems become `invalid-annotation` warnings.
  const auto diagnostics =
      kindProblemDiagnostics(unit.kinds, unit.context().getSourceManager());
  ASSERT_EQ(diagnostics.size(), 1U);
  EXPECT_EQ(diagnostics.front().id, core::diag::InvalidAnnotation);
  EXPECT_EQ(diagnostics.front().severity, core::Severity::Warning);
}

// The table's inferred entries never make required positions.
TEST(KindTable, DeclaredShapesAndRequirements) {
  KindEntry entry;
  entry.kind = core::PointerKind::single(core::Nullability::Nonnull,
                                         core::KindSource::Default);
  EXPECT_TRUE(entry.hasShape());
  EXPECT_FALSE(entry.hasDeclaredShape());
  EXPECT_TRUE(entry.isNonnull());
  EXPECT_FALSE(entry.declaresNonnull());
  entry.shapeLevel = KindLevel::Annotation;
  entry.nullabilityLevel = KindLevel::Ecosystem;
  EXPECT_TRUE(entry.hasDeclaredShape());
  EXPECT_TRUE(entry.declaresNonnull());
  EXPECT_FALSE(entry.hasEnforcedRequirement());
  MustAccessRequirement requirement{
      .kind = core::PointerKind::counted(
          core::ExtentTerm::of(core::ExtentPath::ofParam(1), 1, 1),
          core::Nullability::Nonnull, core::KindSource::Inferred),
      .guard = RequirementGuard{.lhs = core::ExtentTerm::constant(0),
                                .relation = RequirementGuard::Relation::Less,
                                .rhs = core::ExtentTerm::of(
                                    core::ExtentPath::ofParam(1))},
      .rule = MustAccessRule::R2,
      .access = nullptr};
  EXPECT_EQ(requirement.toString(),
            "0 < param 1 scale 1 plus 0 -> counted(param 1 scale 1 plus 1) "
            "nonnull (R2)");
  entry.mustAccess.push_back(requirement);
  entry.enforcement = RequirementEnforcement::CallerContract;
  EXPECT_FALSE(entry.hasEnforcedRequirement());
  entry.enforcement = RequirementEnforcement::CallSites;
  EXPECT_TRUE(entry.hasEnforcedRequirement());
  EXPECT_EQ(toString(RequirementEnforcement::CallerContract),
            "caller-contract");
  EXPECT_EQ(toString(MustAccessRule::R4), "R4");
  // RFC 0030 §13.1: the guard round-trips through the record, so the link
  // step can decide the requirement at a caller in another unit.
  const auto guard = RequirementGuard::parse(requirement.guard->toString());
  ASSERT_TRUE(guard);
  EXPECT_EQ(*guard, *requirement.guard);
  const auto inclusive = RequirementGuard::parse("1 <= param 2 scale 4 plus 3");
  ASSERT_TRUE(inclusive);
  EXPECT_EQ(inclusive->relation, RequirementGuard::Relation::LessEqual);
  EXPECT_EQ(inclusive->rhs.toString(), "param 2 scale 4 plus 3");
  EXPECT_FALSE(RequirementGuard::parse("param 1 scale 1 plus 0"));
  EXPECT_FALSE(RequirementGuard::parse("0 < not-a-term here"));
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
