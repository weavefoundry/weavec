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

// RFC 0010, *Annotations*.
TEST(ParseAnnotation, RecognisesShareSpellings) {
  EXPECT_EQ(parseAnnotation("weavec.retains"), Annotation::Retains);
  EXPECT_EQ(parseAnnotation("weavec.releases"), Annotation::Releases);
  EXPECT_EQ(parseAnnotation("weavec.refcount"), Annotation::Refcount);
  EXPECT_EQ(parseAnnotation("weavec.family.fclose"), Annotation::Family);
  EXPECT_EQ(parseAnnotation("weavec.family.my_free2"), Annotation::Family);
  EXPECT_EQ(parseAnnotation("weavec.family."), Annotation::Invalid);
  EXPECT_EQ(parseAnnotation("weavec.family.not an id"), Annotation::Invalid);
  EXPECT_EQ(parseAnnotation("weavec.family.a(b)"), Annotation::Invalid);
}

// RFC 0011, *Annotations*: `WEAVEC_SIZED_BY(n)`.
TEST(ParseAnnotation, RecognisesSizedBy) {
  EXPECT_EQ(parseAnnotation("weavec.sized_by.n"), Annotation::SizedBy);
  EXPECT_EQ(parseAnnotation("weavec.sized_by.count_2"), Annotation::SizedBy);
  EXPECT_EQ(parseAnnotation("weavec.sized_by."), Annotation::Invalid);
  EXPECT_EQ(parseAnnotation("weavec.sized_by.n + 1"), Annotation::Invalid);
}

TEST(GetAnnotations, CollectsSizedBy) {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      R"c(
      void f(char *__attribute__((annotate("weavec.sized_by.n"))) p,
             unsigned long n,
             char *__attribute__((annotate("weavec.sized_by.n")))
             __attribute__((annotate("weavec.sized_by.m"))) q,
             unsigned long m);
      )c",
      {"-x", "c"}, "input.c");
  ASSERT_TRUE(ast);
  const clang::FunctionDecl *f = nullptr;
  for (const clang::Decl *d :
       ast->getASTContext().getTranslationUnitDecl()->decls())
    if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(d))
      f = fn;
  ASSERT_NE(f, nullptr);
  const AnnotationSet p = getAnnotations(*f->getParamDecl(0));
  EXPECT_EQ(p.sizedBy, "n");
  EXPECT_TRUE(p.any());
  EXPECT_FALSE(p.ownership()) << "an extent is not an ownership kind";
  EXPECT_FALSE(p.invalid);
  EXPECT_TRUE(getAnnotations(*f->getParamDecl(1)).sizedBy.empty());
  EXPECT_TRUE(getAnnotations(*f->getParamDecl(2)).invalid)
      << "two counters contradict";
}

TEST(GetAnnotations, CollectsShareAnnotationsAndFamilies) {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      R"c(
      struct obj;
      void f(struct obj *__attribute__((annotate("weavec.retains"))) a,
             struct obj *__attribute__((annotate("weavec.releases"))) b,
             struct obj *__attribute__((annotate("weavec.owned")))
             __attribute__((annotate("weavec.family.obj_free"))) c,
             struct obj *__attribute__((annotate("weavec.family.x")))
             __attribute__((annotate("weavec.family.y"))) d);
      struct obj { int __attribute__((annotate("weavec.refcount"))) rc; };
      )c",
      {"-x", "c"}, "input.c");
  ASSERT_TRUE(ast);

  const clang::FunctionDecl *f = nullptr;
  const clang::RecordDecl *obj = nullptr;
  for (const clang::Decl *d :
       ast->getASTContext().getTranslationUnitDecl()->decls()) {
    if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(d))
      f = fn;
    if (const auto *record = llvm::dyn_cast<clang::RecordDecl>(d);
        record != nullptr && record->isCompleteDefinition())
      obj = record;
  }
  ASSERT_NE(f, nullptr);
  ASSERT_NE(obj, nullptr);

  const AnnotationSet a = getAnnotations(*f->getParamDecl(0));
  EXPECT_TRUE(a.retains);
  EXPECT_TRUE(a.ownership());
  EXPECT_FALSE(a.owned);
  const AnnotationSet b = getAnnotations(*f->getParamDecl(1));
  EXPECT_TRUE(b.releases);
  EXPECT_TRUE(b.ownership());
  const AnnotationSet c = getAnnotations(*f->getParamDecl(2));
  EXPECT_TRUE(c.owned);
  EXPECT_EQ(c.family, "obj_free");
  EXPECT_FALSE(c.invalid);
  const AnnotationSet d = getAnnotations(*f->getParamDecl(3));
  EXPECT_TRUE(d.invalid) << "two families contradict";

  const clang::FieldDecl *rc = *obj->field_begin();
  const AnnotationSet onField = getAnnotations(*rc);
  EXPECT_TRUE(onField.refcount);
  EXPECT_FALSE(onField.ownership()) << "a count is not an ownership kind";
  EXPECT_TRUE(onField.any());

  // Merging keeps the family and flags; a disagreement is invalid.
  AnnotationSet merged = a;
  merged.merge(c);
  EXPECT_TRUE(merged.retains);
  EXPECT_TRUE(merged.owned);
  EXPECT_EQ(merged.family, "obj_free");
  AnnotationSet other;
  other.family = "elsewhere";
  merged.merge(other);
  EXPECT_TRUE(merged.invalid);
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
