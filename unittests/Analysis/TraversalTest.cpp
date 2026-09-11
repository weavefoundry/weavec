//===- TraversalTest.cpp - Practical C traversal checks (RFC 0021) -------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"
#include "weavec/Core/CheckedIO.h"

#include <gtest/gtest.h>

#include "../../lib/Analysis/PlaceBuilder.h"

namespace weavec::analysis {

static core::CheckedContract traversalCheck(const std::string &code) {
  AnalysisOptions options;
  options.checkedFunctions.insert("f");
  const auto unit = test::analyze(code, options);
  if (!unit.ast || !unit.summary("f")) {
    ADD_FAILURE() << "could not analyze traversal test";
    return {};
  }
  return unit.summary("f")->checked;
}

TEST(Traversal, SameArrayOperationsIncludeOnePastAndNegativeDifferences) {
  for (const auto *body :
       {"int a[4]; int *p=a+4; return p-a;",
        "int a[4]; int *p=a+4; return a-p;",
        "int a[4]; int *p=a+4; return a<p;",
        "char a[4]; unsigned n=4; char *p=a+n; n=8; return p-a;"}) {
    SCOPED_TRACE(body);
    const auto contract =
        traversalCheck("long f(void){" + std::string(body) + "}");
    EXPECT_TRUE(contract.complete());
    EXPECT_TRUE(contract.requirements.empty());
  }
}

TEST(Traversal, UnrelatedArraysAndSubobjectsDoNotPermitOrdering) {
  for (const auto *body :
       {"int a[4],b[4]; return a-b;", "int a[4],b[4]; return a<b;",
        "struct {int a[4];int b[4];} s; return s.a-s.b;",
        "int a[2][4]; return a[0]-a[1];"}) {
    SCOPED_TRACE(body);
    EXPECT_FALSE(
        traversalCheck("long f(void){" + std::string(body) + "}").complete());
  }
}

TEST(Traversal, SameArrayDifferenceStillRequiresTheTargetIntegerRange) {
  const auto contract = traversalCheck(
      "long f(char *p,unsigned long n){char *end=p+n;return end-p;}");
  EXPECT_FALSE(contract.complete());
  bool found = false;
  for (const auto &[key, obligation] : contract.obligations.entries()) {
    (void)key;
    found |=
        obligation.reason ==
            "pointer difference must fit target ptrdiff_t and element size" &&
        obligation.outcome == core::SafetyOutcome::Unresolved;
  }
  EXPECT_TRUE(found);
}

TEST(Traversal, ExhaustedVariableBudgetRemainsExplicitlyIncomplete) {
  std::string code = "void f(";
  for (unsigned i = 0; i < 65; ++i)
    code += (i ? ",char *p" : "char *p") + std::to_string(i);
  code += "){while(";
  for (unsigned i = 0; i < 65; ++i)
    code += (i ? "||*p" : "*p") + std::to_string(i);
  code += "){break;}}";
  const auto contract = traversalCheck(code);
  EXPECT_FALSE(contract.complete());
  EXPECT_TRUE(contract.limited);
  bool found = false;
  for (const auto &[key, obligation] : contract.obligations.entries()) {
    (void)key;
    found |= obligation.reason == "traversal variable limit reached" &&
             obligation.outcome == core::SafetyOutcome::Unresolved;
  }
  EXPECT_TRUE(found);
}

TEST(Traversal, DirectGotoUsesActualInitializationEdges) {
  EXPECT_TRUE(
      traversalCheck(
          "int f(int flag){int x=3;if(flag)goto done;x=4;done:return x;}")
          .complete());
  EXPECT_FALSE(traversalCheck(
                   "int f(int flag){if(flag)goto done;int x=4;done:return x;}")
                   .complete());
  EXPECT_FALSE(traversalCheck(
                   "int f(void){void *label=&&done;goto *label;done:return 0;}")
                   .complete());
}

TEST(Traversal, ScalarLoopConditionsSupplySufficientAccessBounds) {
  for (const auto *body :
       {"unsigned i=0;while(i<n){p[i]=1;++i;}",
        "for(unsigned i=1;i<n;++i)p[i]=1;",
        "unsigned i=0;while(i<n){unsigned j=i;if(j+1<n)p[j+1]=1;++i;}"}) {
    SCOPED_TRACE(body);
    EXPECT_TRUE(
        traversalCheck("void f(char *p,unsigned n){" + std::string(body) + "}")
            .complete());
  }
}

TEST(Traversal, SkippedStoresAndLoopEntriesCannotInventInitialization) {
  for (const auto *body :
       {"char a[4];for(unsigned i=0;i<4;++i){if(i==2)continue;a[i]=1;}return "
        "a[2];",
        "char a[4];unsigned i=0;while(i<4){if(i==2)break;a[i]=1;++i;}return "
        "a[3];",
        "char a[4];unsigned i=2;goto body;while(i<4){body:a[i]=1;++i;}return "
        "a[0];",
        "char a[4];unsigned i=0;while(i<4){if(flag)a[i]=1;++i;}return a[0];"}) {
    SCOPED_TRACE(body);
    EXPECT_FALSE(traversalCheck("int f(int flag){" + std::string(body) + "}")
                     .complete());
  }
}

TEST(Traversal, CountAndCursorOutputsDescribeTheActualProducedPrefix) {
  const std::string count =
      "static unsigned fill(char *p,unsigned n){unsigned i=0;"
      "while(i<n){if(i==2)break;p[i++]=1;}return i;}";
  const auto countGood =
      traversalCheck(count + "int f(void){char a[4];unsigned "
                             "n=fill(a,4);if(n!=2)return 0;return a[n-1];}");
  EXPECT_TRUE(countGood.complete());
  EXPECT_TRUE(countGood.requirements.empty());
  for (const auto *body : {"char a[4];fill(a,4);return a[3];",
                           "char a[4];unsigned n=fill(a,4);n=4;return a[n-1];"})
    EXPECT_FALSE(
        traversalCheck(count + "int f(void){" + body + "}").complete());
  const std::string cursor = "static char *fill(char *p,unsigned n){char "
                             "*end=p+n;while(p<end)*p++=1;return p;}";
  EXPECT_TRUE(
      traversalCheck(
          cursor + "int f(void){char a[4];char *end=fill(a,4);return end[-1];}")
          .complete());
  EXPECT_FALSE(
      traversalCheck(
          cursor + "int f(void){char a[4];char *end=fill(a,4);return end[0];}")
          .complete());
}

TEST(Traversal, OutPointerWritesReplacePositionsAndPreserveSnapshots) {
  const std::string advance =
      "static void advance(char **p,unsigned n){*p+=n;}";
  EXPECT_TRUE(
      traversalCheck(
          advance +
          "int f(void){char a[4]={0};char *p=a;advance(&p,4);return p[-1];}")
          .complete());
  EXPECT_TRUE(traversalCheck(advance +
                             "int f(void){char a[4]={0};char "
                             "*p=a,*old=p;advance(&p,4);return old[0];}")
                  .complete());
  EXPECT_FALSE(
      traversalCheck(
          advance +
          "int f(void){char a[4]={0};char *p=a;advance(&p,4);return p[0];}")
          .complete());
  EXPECT_FALSE(traversalCheck("extern void mutate(char **p);int f(void){char "
                              "a[4]={0};char *p=a;mutate(&p);return p[0];}")
                   .complete());
}

TEST(Traversal, BoundedDecoderLoopsRetainAccumulatorRanges) {
  EXPECT_TRUE(
      traversalCheck(
          "int f(const unsigned char *p){int value=0;for(unsigned i=0;i<3;++i)"
          "value=(value<<6)+(p[i]&63);return value;}")
          .complete());
  EXPECT_FALSE(
      traversalCheck(
          "int f(const unsigned char *p){int value=0;for(unsigned i=0;i<6;++i)"
          "value=(value<<6)+(p[i]&63);return value;}")
          .complete());
  EXPECT_FALSE(
      traversalCheck("int f(int flag){char a[3];for(unsigned i=0;i<3;++i)"
                     "if(flag)a[i]=1;return a[2];}")
          .complete());
  EXPECT_FALSE(
      traversalCheck(
          "int f(const unsigned char *p,unsigned n){int value=0;"
          "for(unsigned i=0;i<n;++i)value=(value<<6)+(p[i]&63);return value;}")
          .complete());
}

TEST(Traversal, SpecializedOptionalOutputsRetainSuccessfulInitialization) {
  const std::string decoder =
      "static int decode(const char *p,unsigned n,int *out){"
      "if(n!=2)return 0;int value=p[0]&31;"
      "for(unsigned i=1;i<n;++i)value=(value<<6)+(p[i]&63);"
      "if(value<128)return 0;if(out)*out=value;return 1;}";
  EXPECT_TRUE(traversalCheck(decoder +
                             "int f(void){char p[2]={1,2};int "
                             "out;if(!decode(p,2,&out))return 0;return out;}")
                  .complete());
  EXPECT_FALSE(
      traversalCheck(
          decoder +
          "int f(void){char p[2]={1,2};int out;decode(p,2,&out);return out;}")
          .complete());
  EXPECT_FALSE(traversalCheck(decoder +
                              "int f(void){char p[1]={1};int "
                              "out;if(!decode(p,2,&out))return 0;return out;}")
                   .complete());
}

TEST(Traversal, CachedPathsRetainObjectViewRegistration) {
  const auto unit =
      test::analyze("struct A{int x;};struct B{int y;};void f(void *p){}");
  ASSERT_NE(unit.ast, nullptr);
  const auto *function = unit.function("f");
  ASSERT_NE(function, nullptr);
  const auto &context = unit.ast->getASTContext();
  std::vector<const clang::FieldDecl *> fields;
  for (const auto *decl : context.getTranslationUnitDecl()->decls())
    if (const auto *record = llvm::dyn_cast<clang::RecordDecl>(decl);
        record && (record->getName() == "A" || record->getName() == "B"))
      fields.push_back(*record->field_begin());
  ASSERT_EQ(fields.size(), 2U);
  core::PlaceTable places;
  PlaceBuilder builder(places, unit.analyzer->summaries(), context);
  const auto object =
      places.deref(builder.placeForVar(*function->getParamDecl(0)));
  const auto a = builder.fieldPlace(object, *fields.front());
  const auto b = builder.fieldPlace(object, *fields.back());
  const auto path = core::SummaryPath::param(0).deref();
  ASSERT_TRUE(builder.summaryPathOf(a));
  const auto firstView = builder.objectViews.at(path);
  ASSERT_TRUE(builder.summaryPathOf(b));
  EXPECT_NE(builder.objectViews.at(path), firstView);
  ASSERT_TRUE(builder.summaryPathOf(a));
  EXPECT_EQ(builder.objectViews.at(path), firstView);
}

TEST(Traversal, CachedMissingPathsDoNotHideNewDeclarationsOrSelectors) {
  const auto unit = test::analyze("int global;void f(int *p){int local;}");
  ASSERT_NE(unit.ast, nullptr);
  const auto *function = unit.function("f");
  ASSERT_NE(function, nullptr);
  const auto &context = unit.ast->getASTContext();
  const auto *body = llvm::cast<clang::CompoundStmt>(function->getBody());
  const auto *local = llvm::cast<clang::VarDecl>(
      llvm::cast<clang::DeclStmt>(*body->body_begin())->getSingleDecl());
  core::PlaceTable places;
  PlaceBuilder builder(places, unit.analyzer->summaries(), context);
  const auto synthetic = places.create("synthetic");
  const auto localPlace = builder.placeForVar(*local);
  for (unsigned repeat = 0; repeat < 2; ++repeat) {
    EXPECT_FALSE(builder.summaryPathOf(synthetic));
    EXPECT_FALSE(builder.summaryPathOf(localPlace));
    EXPECT_FALSE(builder.summaryPathOf(places.deref(localPlace)));
  }
  const auto param = builder.placeForVar(*function->getParamDecl(0));
  EXPECT_EQ(builder.summaryPathOf(param), core::SummaryPath::param(0));
  for (const auto *decl : context.getTranslationUnitDecl()->decls())
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl);
        var && var->getName() == "global") {
      const auto path = builder.summaryPathOf(builder.placeForVar(*var));
      ASSERT_TRUE(path);
      EXPECT_TRUE(path->isGlobal());
    }
  EXPECT_FALSE(builder.summaryPathOf(synthetic));
  EXPECT_FALSE(builder.summaryPathOf(localPlace));
  const auto element = places.element(param, "index");
  std::string selector = "first";
  builder.summaryIndex = [&](std::string_view) {
    return std::optional(selector);
  };
  EXPECT_EQ(builder.summaryPathOf(element),
            core::SummaryPath::param(0).indexed("first"));
  selector = "second";
  EXPECT_EQ(builder.summaryPathOf(element),
            core::SummaryPath::param(0).indexed("second"));
}

TEST(Traversal, PrivateAntecedentsStrengthenOnlyPortableEntryRequirements) {
  const std::vector<std::pair<std::string, std::string>> cases{
      {"static int hidden;", "hidden"},
      {"static void (*hidden)(void);", "hidden != 0"},
      {"static char *hidden;", "hidden == p"},
      {"static unsigned hidden;", "hidden + 1u < n"}};
  for (const auto &[declaration, condition] : cases) {
    SCOPED_TRACE(condition);
    AnalysisOptions options;
    options.checkedFunctions.insert("f");
    std::string code = "void free(void *);";
    code += declaration;
    code += "void f(char *p,unsigned n){if(n && (";
    code += condition;
    code += "))free(p);}";
    const auto unit = test::analyze(code, options);
    ASSERT_NE(unit.summary("f"), nullptr);
    const auto local = unit.summary("f")->checked;
    ASSERT_TRUE(local.complete());
    const auto exported = unit.analyzer->exports();
    const auto &portable = exported.checkedDefinitions.at("f");
    EXPECT_TRUE(portable.complete());
    ASSERT_FALSE(portable.requirements.empty());
    for (const auto &requirement : portable.requirements) {
      EXPECT_EQ(requirement.kind, core::CheckedRequirementKind::Release);
      EXPECT_FALSE(requirement.when.trivial());
      for (const auto &[path, fact] : requirement.when.conditions) {
        (void)fact;
        // RFC 0022 preserves private scalar callback cells by name. Other
        // private conditions still strengthen only the entry requirement.
        if (path.isGlobal()) {
          EXPECT_EQ(condition, "hidden != 0");
          EXPECT_TRUE(
              exported.globals.nameOf(path.index).starts_with("@weavec-hook:"));
        }
      }
      for (const auto &[pair, equal] : requirement.when.pointers) {
        (void)equal;
        EXPECT_FALSE(pair.first.isGlobal());
        EXPECT_FALSE(pair.second.isGlobal());
      }
      for (const auto &predicate : requirement.when.integers)
        for (const auto *expression : {&predicate.lhs, &predicate.rhs})
          for (const auto &node : expression->all())
            EXPECT_FALSE(node.key && node.key->isGlobal());
    }
    EXPECT_EQ(unit.summary("f")->checked, local);
  }
}

TEST(Traversal, PrivateRequiredIntervalsStillInvalidatePortableCompleteness) {
  AnalysisOptions options;
  options.checkedFunctions.insert("f");
  const auto unit = test::analyze(
      "void *memset(void *,int,__SIZE_TYPE__);static unsigned hidden;"
      "void f(char *p){if(hidden)memset(p,0,hidden);}",
      options);
  ASSERT_NE(unit.summary("f"), nullptr);
  ASSERT_TRUE(unit.summary("f")->checked.complete());
  const auto exported = unit.analyzer->exports();
  EXPECT_FALSE(exported.checkedDefinitions.at("f").complete());
  EXPECT_TRUE(exported.checkedDefinitions.at("f").limited);
  EXPECT_FALSE(
      traversalCheck("void free(void *);static int hidden;"
                     "static void release(char *p){if(hidden)free(p);}"
                     "void f(void){char buffer[1];hidden=1;release(buffer);}")
          .complete());
}

TEST(Traversal, RepeatedConditionalRequirementsDoNotLeakEntryFacts) {
  const std::string fill =
      "static void fill(char *p,int flag){if(flag==3){p[0]=1;p[1]=2;}}";
  for (const auto *body : {"int f(void){fill((char*)0,0);return 0;}",
                           "int f(void){char a[2];fill(a,3);return a[1];}"}) {
    SCOPED_TRACE(body);
    const auto checked = traversalCheck(fill + body);
    EXPECT_TRUE(checked.complete());
    EXPECT_TRUE(checked.requirements.empty());
  }
  EXPECT_FALSE(traversalCheck(fill + "int f(void){fill((char*)0,3);return 0;}")
                   .complete());
  EXPECT_FALSE(
      traversalCheck(fill +
                     "int f(int flag){char a[2];fill(a,flag);return a[1];}")
          .complete());
}

TEST(Traversal, FixedLoopExpansionDoesNotDisableLaterLoopWidening) {
  const auto contract =
      traversalCheck("int f(char *p,unsigned n){char a[3];"
                     "for(unsigned i=0;i<3;++i)a[i]=1;"
                     "unsigned j=0;while(j<n){p[j]=1;++j;}return a[2];}");
  EXPECT_TRUE(contract.complete());
  EXPECT_FALSE(contract.limited);
  EXPECT_FALSE(
      traversalCheck("int f(unsigned n){char a[3];"
                     "for(unsigned i=0;i<3;++i)a[i]=1;"
                     "unsigned j=0;while(j<n){a[j]=1;++j;}return a[2];}")
          .complete());
}

TEST(Traversal, CursorLowerBoundsRequireEveryEntryAndAValidUpdate) {
  for (const auto *body :
       {"char a[4]={0};char *p=a;--p;return *p;",
        "char a[4]={0};char *p=a+2;if(flag)p=a;--p;return *p;",
        "char a[4]={0};char *p=a;while(*p)++p;p=a+4;return *p;"}) {
    SCOPED_TRACE(body);
    EXPECT_FALSE(traversalCheck("int f(int flag){" + std::string(body) + "}")
                     .complete());
  }
}

TEST(Traversal, DerivedPointerOutputsPreserveBoundedSameArrayOperations) {
  const std::string scan =
      "static char *scan(char *p,unsigned n,unsigned step){"
      "if(step>n)return 0;return p+step;}";
  EXPECT_TRUE(
      traversalCheck(scan +
                     "long f(unsigned step){char a[4];char *end=scan(a,4,step);"
                     "if(!end)return 0;return end-a;}")
          .complete());
  EXPECT_FALSE(
      traversalCheck(
          scan +
          "long f(unsigned step){char a[4],b[4];char *end=scan(a,4,step);"
          "if(!end)return 0;return end-b;}")
          .complete());
  EXPECT_FALSE(
      traversalCheck(
          scan + "int f(unsigned step){char a[4]={0};char *end=scan(a,4,step);"
                 "if(!end)return 0;return end[0];}")
          .complete());
}

TEST(Traversal, TerminatedScansAndCompactionUseInitializedWitnesses) {
  const std::string compact =
      "static void compact(char *p){char *out=p;while(*p){"
      "if(*p!=' ')*out++=*p;++p;}*out=0;}";
  EXPECT_TRUE(
      traversalCheck(compact +
                     "int f(void){char a[]=\"a b\";compact(a);return a[1];}")
          .complete());
  EXPECT_TRUE(
      traversalCheck(compact +
                     "int f(void){char a[]=\"\";compact(a);return a[0];}")
          .complete());
  EXPECT_TRUE(traversalCheck(compact +
                             "int f(void){char a[]=\"a b\";compact(a);char "
                             "*p=a;while(*p)++p;return *p;}")
                  .complete());
  EXPECT_FALSE(
      traversalCheck(
          compact + "int f(void){char a[3]={'a',' ','b'};compact(a);return 0;}")
          .complete());
  EXPECT_FALSE(
      traversalCheck(
          compact +
          "int f(void){char a[3];a[0]='a';a[2]=0;compact(a);return 0;}")
          .complete());
}

TEST(Traversal, AdvancedStringRequirementsRetainPortableMinimums) {
  for (const auto *definition :
       {"size_t f(const char *p){return strlen(p+2);}",
        "size_t f(const char *p,unsigned n){return strlen(p+n);}",
        "struct Buffer{const char *p;unsigned n;};"
        "size_t f(struct Buffer *b){return strlen(b->p+b->n);}",
        "static size_t suffix(const char *p){return strlen(p+2);}"
        "size_t f(const char *p){return suffix(p+1);}"}) {
    SCOPED_TRACE(definition);
    const auto contract = traversalCheck("size_t strlen(const char *);" +
                                         std::string(definition));
    ASSERT_TRUE(contract.complete());
    bool found = false;
    for (const auto &requirement : contract.requirements) {
      if (requirement.kind != core::CheckedRequirementKind::Terminated)
        continue;
      found = true;
      EXPECT_EQ(requirement.end, core::PathAffine::ofConstant(0));
      EXPECT_NE(requirement.begin, core::PathAffine::ofConstant(0));
    }
    EXPECT_TRUE(found);
    EXPECT_EQ(core::parseCheckedContract(
                  core::printCheckedContract(contract, {}), {}),
              contract);
  }
  const std::string helper =
      "size_t strlen(const char *);"
      "static size_t suffix(const char *p){return strlen(p+2);}";
  const auto good = traversalCheck(
      helper +
      "size_t f(void){char a[]={'a',0,'b',0};a[3]=0;return suffix(a);}");
  EXPECT_TRUE(good.complete());
  EXPECT_TRUE(good.requirements.empty());
  EXPECT_FALSE(
      traversalCheck(helper +
                     "size_t f(void){char a[]={'a',0,'b'};return suffix(a);}")
          .complete());
  EXPECT_FALSE(
      traversalCheck(
          helper +
          "size_t f(void){char a[4];a[0]='a';a[3]=0;return suffix(a);}")
          .complete());
  for (const auto *definition :
       {"size_t f(const char *p){return strlen(p-1);}",
        "size_t f(const char *p,int n){return strlen(p+n);}"})
    EXPECT_FALSE(
        traversalCheck("size_t strlen(const char *);" + std::string(definition))
            .complete());
}

TEST(Traversal, StringWitnessesDoNotPermitUnprovedLookaheadOrDestroyedZeros) {
  const std::string scan =
      "static int scan(const char *p){while(*p)++p;return p[1];}";
  EXPECT_FALSE(
      traversalCheck(scan + "int f(void){char a[]=\"a\";return scan(a);}")
          .complete());
  EXPECT_FALSE(traversalCheck("int f(void){char a[]=\"a\";a[1]='b';char "
                              "*p=a;while(*p)++p;return 0;}")
                   .complete());
}

TEST(Traversal, PairedCursorCallsPreserveOrderAcrossSkippedInput) {
  const std::string helpers =
      "static void skip(char **p){while(**p && **p==' ')++*p;}"
      "static void copy(char **p,char **out){while(**p && **p!=' '){"
      "**out=**p;++*p;++*out;}}"
      "static void compact(char *p){char *out=p;while(*p){"
      "if(*p==' ')skip(&p);else copy(&p,&out);}*out=0;}";
  const auto good = traversalCheck(
      helpers + "int f(void){char a[]=\"ab cd ef\";compact(a);return a[1];}");
  EXPECT_TRUE(good.complete());
  EXPECT_TRUE(good.requirements.empty());
  EXPECT_FALSE(
      traversalCheck(
          helpers + "int f(void){char a[3]={'a','b','c'};compact(a);return 0;}")
          .complete());
}

TEST(Traversal, OverlappingWritesCannotRecreateAnEntryTerminator) {
  const std::string scan = "static void copy(char **p,char "
                           "**out){while(**p){**out=**p;++*p;++*out;}}";
  EXPECT_FALSE(traversalCheck(scan + "int f(void){char a[]=\"ab\";char "
                                     "*p=a,*out=a+1;copy(&p,&out);return 0;}")
                   .complete());
  EXPECT_FALSE(traversalCheck("int f(void){char a[]=\"ab\";char "
                              "*p=a;while(*p){p[1]='x';++p;}return 0;}")
                   .complete());
  const auto shifted =
      traversalCheck("static void scan(char *p){while(*p)++p;}"
                     "int f(void){char a[]=\"ab\";scan(a+1);return 0;}");
  EXPECT_TRUE(shifted.complete());
  EXPECT_TRUE(shifted.requirements.empty());
}

} // namespace weavec::analysis
