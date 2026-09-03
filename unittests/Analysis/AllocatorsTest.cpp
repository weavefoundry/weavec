//===- AllocatorsTest.cpp - Tests for call classification -----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/Allocators.h"

#include "TestUtils.h"

#include "clang/AST/RecursiveASTVisitor.h"

#include <gtest/gtest.h>

#include <optional>

namespace weavec::analysis {
namespace {

/// Collects every call inside the function named `f`, in source order.
class CallCollector : public clang::RecursiveASTVisitor<CallCollector> {
public:
  std::vector<const clang::CallExpr *> calls;

  bool TraverseFunctionDecl(clang::FunctionDecl *function) { // NOLINT
    if (function->getName() != "f")
      return true;
    return RecursiveASTVisitor::TraverseFunctionDecl(function);
  }
  bool VisitCallExpr(clang::CallExpr *call) { // NOLINT
    calls.push_back(call);
    return true;
  }
};

struct Parsed {
  std::unique_ptr<clang::ASTUnit> ast;
  std::vector<const clang::CallExpr *> calls;
  SummaryStore store;

  std::optional<CallEffects> classify(std::size_t index) {
    return classifyCall(*calls[index], store);
  }
};

} // namespace

static Parsed parseCalls(const std::string &code) {
  Parsed parsed;
  parsed.ast = clang::tooling::buildASTFromCodeWithArgs(
      std::string(weavec::test::Prelude) + code, {"-std=c17", "-x", "c", "-w"},
      "input.c");
  if (parsed.ast) {
    CallCollector collector;
    collector.TraverseDecl(
        parsed.ast->getASTContext().getTranslationUnitDecl());
    parsed.calls = std::move(collector.calls);
  }
  return parsed;
}

namespace {

TEST(Allocators, MallocProducesOwned) {
  auto parsed = parseCalls("void f(void) { use(malloc(4)); }");
  ASSERT_EQ(parsed.calls.size(), 2U);
  const auto use = parsed.classify(0);
  ASSERT_TRUE(use) << "`use` is annotated in the prelude";
  EXPECT_EQ(use->source, SummarySource::Annotation);
  EXPECT_FALSE(use->producesOwned);
  ASSERT_EQ(use->borrowedArgs.size(), 1U);
  EXPECT_EQ(use->borrowedArgs[0].second, core::BorrowKind::Shared);
  const auto effects = parsed.classify(1);
  ASSERT_TRUE(effects);
  EXPECT_EQ(effects->source, SummarySource::Builtin);
  EXPECT_TRUE(effects->producesOwned);
  EXPECT_TRUE(effects->consumedArgs.empty());
}

TEST(Allocators, FreeConsumesAndReleases) {
  auto parsed = parseCalls("void f(void *p) { free(p); }");
  ASSERT_EQ(parsed.calls.size(), 1U);
  const auto effects = parsed.classify(0);
  ASSERT_TRUE(effects);
  EXPECT_FALSE(effects->producesOwned);
  EXPECT_TRUE(effects->consumes(0));
  EXPECT_TRUE(effects->frees(0));
}

TEST(Allocators, ReallocConsumesAndProduces) {
  auto parsed = parseCalls("void f(void *p) { use(realloc(p, 8)); }");
  ASSERT_EQ(parsed.calls.size(), 2U);
  const auto effects = parsed.classify(1);
  ASSERT_TRUE(effects);
  EXPECT_TRUE(effects->producesOwned);
  EXPECT_TRUE(effects->consumes(0));
  EXPECT_FALSE(effects->frees(0)) << "moved, not released";
  // RFC 0006: the move is conditional on a non-null result.
  EXPECT_FALSE(
      effects->summary->consumesUnconditionally(core::SummaryPath::param(0)));
  EXPECT_TRUE(effects->summary->outcomes.contains(core::Outcome::Null));
}

TEST(Allocators, AnnotatedParametersAreAuthoritative) {
  auto parsed = parseCalls(R"c(
    void *OWNED make(void);
    void sink(int n, void *OWNED a, const void *BORROWED b, void *MUT c);
    void f(void *x, void *y, void *z) { sink(1, x, y, z); use(make()); }
  )c");
  ASSERT_EQ(parsed.calls.size(), 3U);

  const auto sink = parsed.classify(0);
  ASSERT_TRUE(sink);
  EXPECT_FALSE(sink->producesOwned);
  EXPECT_FALSE(sink->frees(1)) << "moved to the callee, not released";
  EXPECT_EQ(sink->consumedArgs, std::vector<unsigned>{1});
  ASSERT_EQ(sink->borrowedArgs.size(), 2U);
  EXPECT_EQ(sink->borrowedArgs[0].first, 2U);
  EXPECT_EQ(sink->borrowedArgs[0].second, core::BorrowKind::Shared);
  EXPECT_EQ(sink->borrowedArgs[1].first, 3U);
  EXPECT_EQ(sink->borrowedArgs[1].second, core::BorrowKind::Mutable);

  const auto make = parsed.classify(2);
  ASSERT_TRUE(make);
  EXPECT_TRUE(make->producesOwned);
}

TEST(Allocators, IndirectAndStaticCallsAreNotRecognised) {
  auto parsed = parseCalls(R"c(
    static void free_local(void *p) {}
    static char *strdup(const char *s) { return 0; } /* file-local, not libc */
    void f(void (*fp)(void *), void *p) {
      fp(p);
      free_local(p);
      use(strdup(""));
    }
  )c");
  ASSERT_EQ(parsed.calls.size(), 4U);
  // Nothing has been analysed, so the static helpers have no summary yet.
  EXPECT_FALSE(parsed.classify(0)) << "indirect call";
  EXPECT_FALSE(parsed.classify(1)) << "static helper, not analysed";
  EXPECT_FALSE(parsed.classify(3)) << "file-local strdup is not libc";
  EXPECT_FALSE(isKnownAllocator(*parsed.calls[3]->getDirectCallee()));
}

TEST(Allocators, KnownNames) {
  const auto parsed = parseCalls(R"c(
    void *calloc(size_t, size_t);
    char *strdup(const char *);
    void f(void) { use(calloc(1, 1)); use(strdup("")); free(0); }
  )c");
  ASSERT_EQ(parsed.calls.size(), 5U);
  EXPECT_TRUE(isKnownAllocator(*parsed.calls[1]->getDirectCallee()));
  EXPECT_TRUE(isKnownAllocator(*parsed.calls[3]->getDirectCallee()));
  EXPECT_FALSE(isKnownAllocator(*parsed.calls[4]->getDirectCallee()));
  EXPECT_TRUE(isKnownReleaser(*parsed.calls[4]->getDirectCallee()));
  EXPECT_FALSE(isKnownReleaser(*parsed.calls[0]->getDirectCallee()));
}

} // namespace
} // namespace weavec::analysis
