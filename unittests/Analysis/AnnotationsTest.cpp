//===- AnnotationsTest.cpp - Tests for annotation parsing -----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/Annotations.h"

#include "TestUtils.h"

#include "clang/AST/Decl.h"
#include "clang/Tooling/Tooling.h"

#include <gtest/gtest.h>

namespace weavec::analysis {
namespace {

TEST(ParseAnnotation, RecognisesKnownSpellings) {
  EXPECT_EQ(parseAnnotation("weavec.owned"), Annotation::Owned);
  EXPECT_EQ(parseAnnotation("weavec.borrowed"), Annotation::Borrowed);
  EXPECT_EQ(parseAnnotation("weavec.mut_borrowed"), Annotation::MutBorrowed);
  EXPECT_EQ(parseAnnotation("weavec.unsafe"), Annotation::Unsafe);
}

TEST(ParseAnnotation, UnknownWeaveCAnnotationIsInvalid) {
  EXPECT_EQ(parseAnnotation("weavec.bogus"), Annotation::Invalid);
  EXPECT_EQ(parseAnnotation("weavec."), Annotation::Invalid);
}

TEST(ParseAnnotation, ForeignAnnotationsAreIgnored) {
  EXPECT_FALSE(parseAnnotation("gsl.owner"));
  EXPECT_FALSE(parseAnnotation(""));
  EXPECT_FALSE(parseAnnotation("weavec_owned"));
}

TEST(GetAnnotations, CollectsFromDeclaration) {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      R"c(
      __attribute__((annotate("weavec.unsafe")))
      __attribute__((annotate("something.else")))
      void f(int *__attribute__((annotate("weavec.owned"))) p) {}
      )c",
      {"-x", "c"}, "input.c");
  ASSERT_TRUE(ast);

  const clang::FunctionDecl *f = nullptr;
  for (const clang::Decl *d :
       ast->getASTContext().getTranslationUnitDecl()->decls())
    if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(d))
      f = fn;
  ASSERT_NE(f, nullptr);

  const AnnotationSet onFunction = getAnnotations(*f);
  EXPECT_TRUE(onFunction.unsafe);
  EXPECT_FALSE(onFunction.owned);
  EXPECT_FALSE(onFunction.invalid);

  const AnnotationSet onParam = getAnnotations(*f->getParamDecl(0));
  EXPECT_TRUE(onParam.owned);
  EXPECT_FALSE(onParam.unsafe);
  EXPECT_TRUE(onParam.any());
}

} // namespace
} // namespace weavec::analysis
