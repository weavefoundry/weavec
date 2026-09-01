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
};

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

TEST(Allocators, MallocProducesOwned) {
  const auto parsed = parseCalls("void f(void) { use(malloc(4)); }");
  ASSERT_EQ(parsed.calls.size(), 2U);
  EXPECT_FALSE(classifyCall(*parsed.calls[0])) << "`use` has no effects";
  const auto effects = classifyCall(*parsed.calls[1]);
  ASSERT_TRUE(effects);
  EXPECT_TRUE(effects->producesOwned);
  EXPECT_FALSE(effects->isRealloc);
  EXPECT_TRUE(effects->consumedArgs.empty());
}

TEST(Allocators, FreeConsumesAndReleases) {
  const auto parsed = parseCalls("void f(void *p) { free(p); }");
  ASSERT_EQ(parsed.calls.size(), 1U);
  const auto effects = classifyCall(*parsed.calls[0]);
  ASSERT_TRUE(effects);
  EXPECT_FALSE(effects->producesOwned);
  EXPECT_TRUE(effects->releasesArgs);
  EXPECT_TRUE(effects->consumes(0));
}

TEST(Allocators, ReallocConsumesAndProduces) {
  const auto parsed = parseCalls("void f(void *p) { use(realloc(p, 8)); }");
  ASSERT_EQ(parsed.calls.size(), 2U);
  const auto effects = classifyCall(*parsed.calls[1]);
  ASSERT_TRUE(effects);
  EXPECT_TRUE(effects->producesOwned);
  EXPECT_TRUE(effects->isRealloc);
  EXPECT_TRUE(effects->consumes(0));
  EXPECT_FALSE(effects->releasesArgs) << "moved, not released";
}

TEST(Allocators, AnnotatedParametersAreAuthoritative) {
  const auto parsed = parseCalls(R"c(
    void *OWNED make(void);
    void sink(int n, void *OWNED a, const void *BORROWED b, void *MUT c);
    void f(void *x, void *y, void *z) { sink(1, x, y, z); use(make()); }
  )c");
  ASSERT_EQ(parsed.calls.size(), 3U);

  const auto sink = classifyCall(*parsed.calls[0]);
  ASSERT_TRUE(sink);
  EXPECT_FALSE(sink->producesOwned);
  EXPECT_FALSE(sink->releasesArgs);
  EXPECT_EQ(sink->consumedArgs, std::vector<unsigned>{1});
  ASSERT_EQ(sink->borrowedArgs.size(), 2U);
  EXPECT_EQ(sink->borrowedArgs[0].first, 2U);
  EXPECT_EQ(sink->borrowedArgs[0].second, core::BorrowKind::Shared);
  EXPECT_EQ(sink->borrowedArgs[1].first, 3U);
  EXPECT_EQ(sink->borrowedArgs[1].second, core::BorrowKind::Mutable);

  const auto make = classifyCall(*parsed.calls[2]);
  ASSERT_TRUE(make);
  EXPECT_TRUE(make->producesOwned);
}

TEST(Allocators, IndirectAndStaticCallsAreNotRecognised) {
  const auto parsed = parseCalls(R"c(
    static void free_local(void *p) {}
    static char *strdup(const char *s) { return 0; } /* file-local, not libc */
    void f(void (*fp)(void *), void *p) {
      fp(p);
      free_local(p);
      use(strdup(""));
    }
  )c");
  ASSERT_EQ(parsed.calls.size(), 4U);
  for (const clang::CallExpr *call : parsed.calls)
    EXPECT_FALSE(classifyCall(*call));
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
