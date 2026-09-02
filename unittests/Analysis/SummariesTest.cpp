//===- SummariesTest.cpp - Tests for summary resolution -------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `SummaryStore::lookup` order (RFC 0003, provider steps 1-4), the
// annotation-derived summaries, and the shipped library table.
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/Summaries.h"

#include "TestUtils.h"

#include "llvm/ADT/STLExtras.h"

#include <gtest/gtest.h>

namespace weavec::analysis {
namespace {

using core::PlaceEffect;
using core::SummaryPath;
using core::ValueSource;

struct Parsed {
  std::unique_ptr<clang::ASTUnit> ast;

  [[nodiscard]] const clang::FunctionDecl *fn(llvm::StringRef name) const {
    for (const clang::Decl *decl :
         ast->getASTContext().getTranslationUnitDecl()->decls()) {
      const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl);
      if (function != nullptr && function->getName() == name)
        return function;
    }
    return nullptr;
  }
};

} // namespace

static Parsed parse(const std::string &code) {
  Parsed parsed;
  parsed.ast = clang::tooling::buildASTFromCodeWithArgs(
      std::string(weavec::test::Prelude) + code, {"-std=c17", "-x", "c", "-w"},
      "input.c");
  return parsed;
}

namespace {

TEST(Summaries, AnnotationsDeriveASummary) {
  const auto parsed = parse(R"c(
    struct s;
    struct s *OWNED make(int n, struct s *OWNED a, const struct s *BORROWED b,
                         struct s *MUT c, struct s *d);
    struct s *BORROWED peekat(struct s *BORROWED s);
    void plain(struct s *p);
  )c");
  ASSERT_TRUE(parsed.ast);

  const clang::FunctionDecl *make = parsed.fn("make");
  ASSERT_NE(make, nullptr);
  EXPECT_TRUE(hasOwnershipAnnotations(*make));
  const core::FunctionSummary summary = summaryFromAnnotations(*make);
  EXPECT_FALSE(summary.consumes(0)) << "n is not a pointer";
  EXPECT_TRUE(summary.consumes(1));
  EXPECT_FALSE(summary.frees(1)) << "moved, not released";
  EXPECT_EQ(summary.borrowKind(2), core::BorrowKind::Shared);
  EXPECT_EQ(summary.borrowKind(3), core::BorrowKind::Mutable);
  EXPECT_EQ(summary.inferredKind(4), core::OwnershipKind::Unknown);
  EXPECT_EQ(summary.returns, std::set<ValueSource>{ValueSource::fresh()});

  const clang::FunctionDecl *peekat = parsed.fn("peekat");
  ASSERT_NE(peekat, nullptr);
  EXPECT_EQ(summaryFromAnnotations(*peekat).returns,
            std::set<ValueSource>{ValueSource::unknown()})
      << "a borrowed result's source is not known from the signature";

  const clang::FunctionDecl *plain = parsed.fn("plain");
  ASSERT_NE(plain, nullptr);
  EXPECT_FALSE(hasOwnershipAnnotations(*plain));
  EXPECT_TRUE(summaryFromAnnotations(*plain).empty());
}

TEST(Summaries, AnnotationsOnAnyRedeclarationCount) {
  const auto parsed = parse(R"c(
    void take(void *OWNED p);
    void take(void *p) { }
  )c");
  ASSERT_TRUE(parsed.ast);
  // The definition is the last redeclaration; the prototype carried the
  // annotation.
  const clang::FunctionDecl *definition = nullptr;
  for (const clang::Decl *decl :
       parsed.ast->getASTContext().getTranslationUnitDecl()->decls()) {
    const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl);
    if (function != nullptr && function->getName() == "take" &&
        function->doesThisDeclarationHaveABody())
      definition = function;
  }
  ASSERT_NE(definition, nullptr);
  EXPECT_TRUE(hasOwnershipAnnotations(*definition));
  EXPECT_TRUE(summaryFromAnnotations(*definition).consumes(0));
  EXPECT_TRUE(collectAnnotations(*definition).params[0].owned);
}

TEST(Summaries, LookupOrder) {
  const auto parsed = parse(R"c(
    void *calloc(size_t, size_t);
    void take(void *OWNED p);
    void mystery(void *p);
    static void helper(void *p) {}
    __attribute__((annotate("weavec.unsafe"))) void raw(void *p) {}
  )c");
  ASSERT_TRUE(parsed.ast);
  SummaryStore store;

  const auto builtin = store.lookup(*parsed.fn("calloc"));
  ASSERT_TRUE(builtin);
  EXPECT_EQ(builtin->source, SummarySource::Builtin);
  EXPECT_TRUE(builtin->summary->returns.contains(ValueSource::fresh()));

  const auto annotated = store.lookup(*parsed.fn("take"));
  ASSERT_TRUE(annotated);
  EXPECT_EQ(annotated->source, SummarySource::Annotation);
  EXPECT_TRUE(annotated->summary->consumes(0));

  EXPECT_FALSE(store.lookup(*parsed.fn("mystery"))) << "nothing known";
  EXPECT_FALSE(store.lookup(*parsed.fn("helper"))) << "not analysed yet";

  // An unsafe definition's signature is its contract: known, and empty.
  const auto unsafe = store.lookup(*parsed.fn("raw"));
  ASSERT_TRUE(unsafe);
  EXPECT_TRUE(unsafe->summary->empty());

  // Once inferred, the helper resolves; setting the same summary again
  // reports no change.
  core::FunctionSummary inferred;
  inferred.addEffect(SummaryPath::param(0), PlaceEffect{.freed = true});
  EXPECT_TRUE(store.setInferred(*parsed.fn("helper"), inferred));
  EXPECT_FALSE(store.setInferred(*parsed.fn("helper"), inferred));
  const auto resolved = store.lookup(*parsed.fn("helper"));
  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->source, SummarySource::Inferred);
  EXPECT_TRUE(resolved->summary->frees(0));

  // A user definition shadows the library table.
  core::FunctionSummary userCalloc;
  EXPECT_TRUE(store.setInferred(*parsed.fn("calloc"), userCalloc));
  const auto shadowed = store.lookup(*parsed.fn("calloc"));
  ASSERT_TRUE(shadowed);
  EXPECT_EQ(shadowed->source, SummarySource::Inferred);
  EXPECT_FALSE(shadowed->summary->returns.contains(ValueSource::fresh()));

  EXPECT_TRUE(store.noteUnknownCallee(*parsed.fn("mystery")));
  EXPECT_FALSE(store.noteUnknownCallee(*parsed.fn("mystery")))
      << "reported once per callee";
}

TEST(Summaries, AnnotationsOverrideInferredRootsOnly) {
  const auto parsed = parse(R"c(
    struct b { char *data; };
    void init(struct b *MUT b, char *p);
  )c");
  ASSERT_TRUE(parsed.ast);
  const clang::FunctionDecl *init = parsed.fn("init");
  ASSERT_NE(init, nullptr);

  // Pretend the body freed both `b` and `p` and stored into `b->data`.
  core::FunctionSummary inferred;
  inferred.addEffect(SummaryPath::param(0), PlaceEffect{.freed = true});
  inferred.addEffect(SummaryPath::param(1), PlaceEffect{.freed = true});
  inferred.addStore(
      core::Store{.dest = SummaryPath::param(0).deref().field("data"),
                  .value = ValueSource::fresh()});
  SummaryStore store;
  store.setInferred(*init, inferred);

  const auto resolved = store.lookup(*init);
  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->source, SummarySource::Annotation);
  EXPECT_FALSE(resolved->summary->consumes(0)) << "MUT says borrowed";
  EXPECT_EQ(resolved->summary->borrowKind(0), core::BorrowKind::Mutable);
  EXPECT_TRUE(resolved->summary->consumes(1)) << "unannotated root: inferred";
  EXPECT_EQ(resolved->summary->stores.size(), 1U)
      << "stores through a borrowed parameter are kept";
}

TEST(Summaries, GlobalTableInternsCanonicalDecls) {
  const auto parsed = parse(R"c(
    extern int g;
    int g;
    int h;
  )c");
  ASSERT_TRUE(parsed.ast);
  std::vector<const clang::VarDecl *> vars;
  for (const clang::Decl *decl :
       parsed.ast->getASTContext().getTranslationUnitDecl()->decls()) {
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
      vars.push_back(var);
  }
  ASSERT_EQ(vars.size(), 3U);
  GlobalTable table;
  const std::uint32_t g1 = table.idFor(*vars[0]);
  const std::uint32_t g2 = table.idFor(*vars[1]);
  const std::uint32_t h = table.idFor(*vars[2]);
  EXPECT_EQ(g1, g2) << "redeclarations share an id";
  EXPECT_NE(g1, h);
  EXPECT_EQ(table.size(), 2U);
  EXPECT_EQ(table.nameOf(g1), "g");
  EXPECT_EQ(table.nameOf(h), "h");
  EXPECT_EQ(table.nameOf(99), "<global>");
  EXPECT_EQ(table.declFor(99), nullptr);
}

// -- Builtins -----------------------------------------------------------------

TEST(Builtins, TableCoversTheAllocatorList) {
  const std::vector<llvm::StringRef> names = builtinNames();
  for (const char *expected :
       {"malloc", "calloc", "realloc", "free", "strdup", "strndup",
        "aligned_alloc", "fopen", "fclose", "strchr", "memcpy", "strtol"})
    EXPECT_TRUE(llvm::is_contained(names, expected)) << expected;
}

TEST(Builtins, Entries) {
  const auto parsed = parse(R"c(
    typedef struct FILE FILE;
    FILE *fopen(const char *, const char *);
    int fclose(FILE *);
    char *strchr(const char *, int);
    void *memcpy(void *, const void *, size_t);
    long strtol(const char *, char **, int);
    size_t strlen(const char *);
    void *bsearch(const void *, const void *, size_t, size_t, int (*)(const void *, const void *));
    char *getenv(const char *);
    static char *strdup(const char *s) { return 0; }
  )c");
  ASSERT_TRUE(parsed.ast);

  const auto *fopenSummary = builtinSummary(*parsed.fn("fopen"));
  ASSERT_NE(fopenSummary, nullptr);
  EXPECT_TRUE(fopenSummary->returns.contains(ValueSource::fresh()));
  EXPECT_EQ(fopenSummary->borrowKind(0), core::BorrowKind::Shared);

  const auto *fcloseSummary = builtinSummary(*parsed.fn("fclose"));
  ASSERT_NE(fcloseSummary, nullptr);
  EXPECT_TRUE(fcloseSummary->frees(0));

  const auto *strchrSummary = builtinSummary(*parsed.fn("strchr"));
  ASSERT_NE(strchrSummary, nullptr);
  EXPECT_EQ(strchrSummary->returns,
            std::set<ValueSource>{ValueSource::copy(SummaryPath::param(0))});
  EXPECT_EQ(strchrSummary->borrowKind(0), core::BorrowKind::Shared);

  const auto *memcpySummary = builtinSummary(*parsed.fn("memcpy"));
  ASSERT_NE(memcpySummary, nullptr);
  EXPECT_EQ(memcpySummary->borrowKind(0), core::BorrowKind::Mutable);
  EXPECT_EQ(memcpySummary->borrowKind(1), core::BorrowKind::Shared);
  EXPECT_EQ(memcpySummary->returns,
            std::set<ValueSource>{ValueSource::copy(SummaryPath::param(0))});

  const auto *strtolSummary = builtinSummary(*parsed.fn("strtol"));
  ASSERT_NE(strtolSummary, nullptr);
  EXPECT_TRUE(strtolSummary->returns.empty());
  ASSERT_EQ(strtolSummary->stores.size(), 1U);
  EXPECT_EQ(strtolSummary->stores.begin()->dest, SummaryPath::param(1).deref());
  EXPECT_EQ(strtolSummary->stores.begin()->value,
            ValueSource::copy(SummaryPath::param(0)));

  const auto *strlenSummary = builtinSummary(*parsed.fn("strlen"));
  ASSERT_NE(strlenSummary, nullptr);
  EXPECT_EQ(strlenSummary->borrowKind(0), core::BorrowKind::Shared);
  EXPECT_TRUE(strlenSummary->returns.empty());

  const auto *bsearchSummary = builtinSummary(*parsed.fn("bsearch"));
  ASSERT_NE(bsearchSummary, nullptr);
  EXPECT_EQ(bsearchSummary->returns,
            std::set<ValueSource>{ValueSource::copy(SummaryPath::param(1))});

  const auto *getenvSummary = builtinSummary(*parsed.fn("getenv"));
  ASSERT_NE(getenvSummary, nullptr);
  EXPECT_TRUE(getenvSummary->returns.empty()) << "static storage: unknown";

  EXPECT_EQ(builtinSummary(*parsed.fn("strdup")), nullptr)
      << "a file-local function is not libc";
}

} // namespace
} // namespace weavec::analysis
