//===- CheckedCodeTest.cpp - Checked proof coverage (RFC 0018) ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "TestUtils.h"

#include <gtest/gtest.h>

namespace weavec::analysis {
static core::CheckedContract check(const std::string &code,
                                   const std::string &name = "f") {
  AnalysisOptions options;
  options.checkedFunctions.insert(name);
  const auto result = test::analyze(code, options);
  if (!result.ast || !result.summary(name)) {
    ADD_FAILURE() << "could not analyze test";
    return {};
  }
  return result.summary(name)->checked;
}
TEST(CheckedCode, UnknownIndexFailsAndGuardProvesIt) {
  EXPECT_FALSE(check("int f(int i) { int a[4]={0}; return a[i]; }").complete());
  EXPECT_TRUE(
      check(
          "int f(int i) { int a[4]={0}; if(i<0||i>=4)return 0; return a[i]; }")
          .complete());
}
TEST(CheckedCode, MustInitializationAcrossBranches) {
  EXPECT_FALSE(check("int f(int b) { int x; if(b)x=1; return x; }").complete());
  EXPECT_TRUE(
      check("int f(int b) { int x; if(b)x=1;else x=2; return x; }").complete());
  EXPECT_FALSE(
      check("int f(void) { int a[2]; a[0]=1; return a[1]; }").complete());
  EXPECT_TRUE(
      check("int f(void) { int a[2]; a[0]=1; return a[0]; }").complete());
}
TEST(CheckedCode, ChangingIndexDoesNotInitializeTheNextCell) {
  EXPECT_FALSE(check("int f(void){int a[2];int i=0;a[i]=1;i=1;return a[i];}")
                   .complete());
}
TEST(CheckedCode, CalleeWritesDoNotReinterpretAnInitializedIndex) {
  EXPECT_FALSE(check("void h(int*p){*p=1;} int f(int i){int "
                     "a[2];if(i!=0)return 0;a[i]=1;h(&i);return a[i];}")
                   .complete());
  EXPECT_FALSE(check("void h(int *OWNED p){free(p);} void f(int*p){h(p);}")
                   .requirements.empty());
  EXPECT_FALSE(
      check("void "
            "f(void){int*p=malloc(8);if(!p)return;int*q=p+3;(void)q;free(p);}")
          .complete());
}
TEST(CheckedCode, PointerFormationAccountsForUpdatesAndNullOrigins) {
  EXPECT_FALSE(check("void f(void){int a[2];int*p=a+2;++p;}").complete());
  EXPECT_FALSE(check("void f(void){int a[2];int*p=a;p+=3;}").complete());
  EXPECT_FALSE(check("void f(void){int*p=0;int*q=p+0;(void)q;}").complete());
  EXPECT_TRUE(check("void f(void){int a[2];int*p=a;++p;}").complete());
  EXPECT_FALSE(check("int f(void){int a,b;return &a < &b;}").complete());
}
TEST(CheckedCode, WritesRequireMutableStorageAcrossCallsAndCasts) {
  EXPECT_FALSE(check("void f(void){char*p=\"x\";*p=1;}").complete());
  EXPECT_FALSE(
      check("void f(void){const int x=0;int*p=(int*)&x;*p=1;}").complete());
  EXPECT_FALSE(check("void h(char*p){*p=1;}void f(void){char*p=\"x\";h(p);}")
                   .complete());
  EXPECT_FALSE(
      check("void h(int*p){*p=1;}void f(void){const int x=0;h((int*)&x);}")
          .complete());
  EXPECT_FALSE(check("void*memset(void*,int,__SIZE_TYPE__);void f(void){const "
                     "char a[4]={0};memset((void*)a,0,4);}")
                   .complete());
  EXPECT_FALSE(
      check("void f(void){struct S{const int x;}s={0};int*p=(int*)&s.x;*p=1;}")
          .complete());
  EXPECT_TRUE(check("void f(void){int x=0;const int*p=&x;int*q=(int*)p;*q=1;}")
                  .complete());
  EXPECT_TRUE(
      check("void f(void){int*const p=malloc(4);if(!p)return;*p=1;free(p);}")
          .complete());
  const auto contract = check("void f(char*p){*p=1;}");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(std::ranges::any_of(contract.requirements, [](const auto &r) {
    return r.kind == core::CheckedRequirementKind::Writable;
  }));
}
TEST(CheckedCode, AnnotationsAreVisibleAssumptions) {
  const auto annotated = check("int f(int *BORROWED p){return *p;}");
  EXPECT_TRUE(annotated.obligations.trusted());
  EXPECT_FALSE(annotated.requirements.empty());
  EXPECT_TRUE(check("int f(void){int x=0;int*BORROWED p=&x;return *p;}")
                  .obligations.trusted());
}
TEST(CheckedCode, CheckedPlacementRequiresAFunction) {
  const auto result = test::analyze(
      "__attribute__((annotate(\"weavec.checked\"))) int global;");
  EXPECT_TRUE(std::ranges::any_of(
      result.diagnostics.diagnostics(), [](const auto &diagnostic) {
        return diagnostic.id == core::diag::InvalidAnnotation;
      }));
}
TEST(CheckedCode, HeapInitializationIsSeparateFromOwnership) {
  EXPECT_FALSE(check("int f(void) { int*p=malloc(4); if(!p)return 0; int "
                     "x=*p;free(p);return x; }")
                   .complete());
  EXPECT_TRUE(check("int f(void) { int*p=malloc(4); if(!p)return 0; *p=3;int "
                    "x=*p;free(p);return x; }")
                  .complete());
}
TEST(CheckedCode, ConditionalLoopRequiresPossibleAccesses) {
  const auto contract = check("void f(char*p,unsigned n) {for(unsigned "
                              "i=0;i<n;++i){if(p[i])break;p[i]=1;}}");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(std::ranges::any_of(contract.requirements, [](const auto &r) {
    return r.kind == core::CheckedRequirementKind::Extent &&
           r.end.path == core::SummaryPath::param(1);
  }));
  EXPECT_TRUE(std::ranges::any_of(contract.requirements, [](const auto &r) {
    return r.kind == core::CheckedRequirementKind::Initialized;
  }));
}
TEST(CheckedCode, LoopProjectionRejectsChangingInputsAndNegativeOffsets) {
  EXPECT_FALSE(
      check(
          "void f(char*p,unsigned n) {for(unsigned i=0;i<n;++i){p[i]=0;n++;}}")
          .complete());
  EXPECT_FALSE(
      check("void f(char*p,unsigned n) {for(unsigned i=0;i<n;++i){p[i-1]=0;}}")
          .complete());
}
TEST(CheckedCode, SameArgumentCannotSatisfySeparation) {
  EXPECT_FALSE(check("void h(int*p,int*q){free(p);*q=1;} void "
                     "f(void){int*p=malloc(4);if(!p)return;h(p,p);}")
                   .complete());
}
TEST(CheckedCode, ReleasingNullThroughAHelperIsPermitted) {
  EXPECT_TRUE(
      check("void h(int*p){free(p);} void f(void){h(NULL);}").complete());
}
TEST(CheckedCode, UnknownCalleesRemainCoverageFailures) {
  EXPECT_FALSE(check("void unknown(int*); void f(void){int x=0;unknown(&x);}")
                   .complete());
  EXPECT_FALSE(
      check("int external(void); int f(void){return external();}").complete());
}
TEST(CheckedCode, UnsafeAndAssumptionsAreRecorded) {
  const auto unsafe = check("UNSAFE int f(unsigned long x){return *(int*)x;}");
  EXPECT_TRUE(unsafe.complete());
  EXPECT_TRUE(unsafe.obligations.trusted());
  const auto transitive =
      check("UNSAFE int h(unsigned long x){return *(int*)x;} int f(unsigned "
            "long x){return h(x);}");
  EXPECT_TRUE(transitive.complete());
  EXPECT_TRUE(transitive.obligations.trusted());
}
TEST(CheckedCode, DeepCallProvenanceDoesNotExpandRecursiveIdentities) {
  for (const bool trusted : {false, true}) {
    std::string code =
        trusted ? "UNSAFE int external(void);" : "int external(void);";
    code += "int h0(void){return external();}";
    for (unsigned i = 1; i <= 12; ++i)
      code += "int h" + std::to_string(i) + "(void){return h" +
              std::to_string(i - 1) + "();}";
    const auto contract = check(code, "h12");
    EXPECT_EQ(contract.complete(), trusted);
    EXPECT_EQ(contract.obligations.trusted(), trusted);
    EXPECT_FALSE(contract.obligations.limited());
    EXPECT_LT(contract.obligations.entries().size(), 100U);
    for (const auto &[key, entry] : contract.obligations.entries()) {
      EXPECT_LT(key.size(), 2048U);
      EXPECT_LT(entry.subject.size(), 1024U);
    }
  }
}
TEST(CheckedCode, CheckedAnnotationDoesNotTrustExternalBody) {
  EXPECT_FALSE(check("__attribute__((annotate(\"weavec.checked\"))) int "
                     "unknown(void); int f(void){return unknown();}")
                   .complete());
}
TEST(CheckedCode, InvalidArithmeticAndTypeReinterpretationNeedCoverage) {
  EXPECT_FALSE(check("int f(void){return (int)1e100;}").complete());
  EXPECT_FALSE(check("int f(int n){++n;return n;}").complete());
  EXPECT_FALSE(check("int f(int n){n+=1;return n;}").complete());
  EXPECT_FALSE(check("void f(void){int a[2];int*p=a+3;(void)p;}").complete());
  EXPECT_FALSE(
      check("int f(void){char a[8]={0};return *(int*)(a+1);}").complete());
}
TEST(CheckedCode, UnionAndAssemblyRemainUnsupported) {
  EXPECT_FALSE(
      check("int f(void){union U{int a,b;} u={1};return u.b;}").complete());
  EXPECT_FALSE(check("int f(void){__asm__(\"\");return 0;}").complete());
}
TEST(CheckedCode, WritesEstablishOnlyTheirOwnRange) {
  const auto contract = check("void f(int*p){*p=3;}");
  EXPECT_TRUE(contract.complete());
  EXPECT_EQ(contract.establishes.size(), 1U);
  EXPECT_TRUE(
      check("void h(int*p){*p=3;} int f(void){int*p=malloc(4);if(!p)return "
            "0;h(p);int x=*p;free(p);return x;}")
          .complete());
}
TEST(CheckedCode, ConcreteViolationsAndCoverageHaveDifferentIds) {
  AnalysisOptions options;
  options.checked = true;
  const auto missing =
      test::analyze("int f(int i){int a[1]={0};return a[i];}", options);
  EXPECT_TRUE(
      std::ranges::any_of(missing.diagnostics.diagnostics(), [](const auto &d) {
        return d.id == core::diag::CheckingIncomplete;
      }));
  const auto broken =
      test::analyze("void f(void){int*p=malloc(4);free(p);free(p);}", options);
  EXPECT_TRUE(
      std::ranges::any_of(broken.diagnostics.diagnostics(), [](const auto &d) {
        return d.id == core::diag::CheckingFailed;
      }));
}
} // namespace weavec::analysis
