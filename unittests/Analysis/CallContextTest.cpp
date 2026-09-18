//===- CallContextTest.cpp - RFC 0016 compositional call checking ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"
#include "weavec/Analysis/Allocators.h"
#include "weavec/Frontend/Sidecar.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>

namespace weavec::analysis {

TEST(CompositionalCall, RuntimeComparisonsKeepOnlyTheirEstablishedSign) {
  const auto result = test::analyze(R"c(
    typedef __SIZE_TYPE__ size_t;
    int memcmp(const void *,const void *,size_t);
    int strcmp(const char *,const char *);
    int strncmp(const char *,const char *,size_t);
    int inspect(const unsigned char *p, unsigned n, unsigned mode) {
      if (mode==0 && strncmp((const char*)p,"\xef\xbb\xbf",3)==0) return p[n];
      if (mode==1 && memcmp(p,"\0y",2)>=0) return p[n];
      if (mode==2 && strcmp((const char*)p,"\0y")!=0) return p[n];
      if (mode==3 && memcmp(p,"a",1)<=0) return p[n];
      if (mode==4 && memcmp(p,"abc",3)==1) return p[n];
      return 0;
    }
    int absent(void){const unsigned char p[]="abc";return inspect(p,4,0);}
    int embedded(void){const unsigned char p[]={0,120};return inspect(p,2,1);}
    int terminated(void){const unsigned char p[]={0,120};return inspect(p,2,2);}
    int high(void){const unsigned char p[]={255};return inspect(p,1,3);}
    int present(void){const unsigned char p[]={239,187,191,0};return inspect(p,4,0);}
    int magnitude(void){const unsigned char p[]="dbc";return inspect(p,4,4);}
    int short_input(void){const unsigned char p[]={239};return inspect(p,1,0);}
    int unknown(void){unsigned char p[4];p[0]=97;return inspect(p,4,0);}
  )c",
                                    {.checkContracts = true, .checked = true});
  ASSERT_TRUE(result.ast);
  for (const auto *name : {"absent", "embedded", "terminated", "high"}) {
    ASSERT_NE(result.summary(name), nullptr);
    EXPECT_TRUE(result.summary(name)->checked.complete()) << name;
    EXPECT_TRUE(result.summary(name)->checked.requirements.empty()) << name;
  }
  for (const auto *name : {"present", "magnitude", "short_input", "unknown"}) {
    ASSERT_NE(result.summary(name), nullptr);
    EXPECT_FALSE(result.summary(name)->checked.complete()) << name;
  }
}

TEST(CompositionalCall, CompleteLocalHelperWritesKeepSeparateInputBytes) {
  const auto result = test::analyze(R"c(
    void bump(unsigned *p) { ++*p; }
    int inspect(const unsigned char *p, unsigned n) {
      unsigned counter=0; bump(&counter);
      for (unsigned i=0; i<n; ++i)
        if (p[i]==92) return p[n];
      return 0;
    }
    int good(void) {
      const unsigned char text[]={'a','b','c'};
      return inspect(text,3);
    }
    int changed(void) {
      unsigned char text[]={'a','b','c'}; text[1]=92;
      return inspect(text,3);
    }
  )c",
                                    {.checkContracts = true, .checked = true});
  ASSERT_TRUE(result.ast);
  ASSERT_NE(result.summary("good"), nullptr);
  EXPECT_TRUE(result.summary("good")->checked.complete());
  EXPECT_TRUE(result.summary("good")->checked.requirements.empty());
  ASSERT_NE(result.summary("changed"), nullptr);
  EXPECT_FALSE(result.summary("changed")->checked.complete());
}

TEST(CompositionalCall, ConstantByteObjectsRetainContentsAcrossGlobalWrites) {
  const auto result = test::analyze(R"c(
    static unsigned calls;
    int inspect(const unsigned char *p, unsigned n) {
      ++calls;
      for (unsigned i=0; i<n; ++i)
        if (p[i]==92) return p[n];
      return 0;
    }
    int good(void) {
      static const unsigned char text[]={'a','b','c'};
      return inspect(text,3);
    }
    int automatic(void) {
      const unsigned char text[]={'a','b','c'};
      return inspect(text,3);
    }
    int changed(void) {
      unsigned char text[]={'a','b','c'}; text[1]=92;
      return inspect(text,3);
    }
    int write_const(unsigned char *p) { p[1]=92; return 0; }
    int invalid_write(void) {
      static const unsigned char text[]={'a','b','c'};
      return write_const((unsigned char*)text);
    }
  )c",
                                    {.checkContracts = true, .checked = true});
  ASSERT_TRUE(result.ast);
  for (const auto *name : {"good", "automatic"}) {
    ASSERT_NE(result.summary(name), nullptr);
    EXPECT_TRUE(result.summary(name)->checked.complete()) << name;
    EXPECT_TRUE(result.summary(name)->checked.requirements.empty()) << name;
  }
  for (const auto *name : {"changed", "invalid_write"}) {
    ASSERT_NE(result.summary(name), nullptr);
    EXPECT_FALSE(result.summary(name)->checked.complete()) << name;
  }
}

TEST(CompositionalCall,
     ExactBytesRetainTheirReadableIntervalThroughForwarding) {
  const auto result = test::analyze(R"c(
    int scan(const unsigned char *p, unsigned n) {
      for (unsigned i=0; i<n; ++i)
        if (p[i]==92) return p[n];
      return 0;
    }
    int forward(const unsigned char *p, unsigned n) { return scan(p,n); }
    int good(void) {
      static const unsigned char text[]={'a','b','c'};
      return forward(text,3);
    }
    int changed(void) {
      unsigned char text[]={'a','b','c'}; text[1]=92;
      return forward(text,3);
    }
    int oversized(void) {
      const unsigned char text[]={'a','b','c'};
      return forward(text,4);
    }
    int partial(void) {
      unsigned char text[3]; text[0]='a';
      return forward(text,3);
    }
  )c",
                                    {.checkContracts = true, .checked = true});
  ASSERT_TRUE(result.ast);
  ASSERT_NE(result.summary("good"), nullptr);
  EXPECT_TRUE(result.summary("good")->checked.complete());
  EXPECT_TRUE(result.summary("good")->checked.requirements.empty());
  for (const auto *name : {"changed", "oversized", "partial"}) {
    ASSERT_NE(result.summary(name), nullptr);
    EXPECT_FALSE(result.summary(name)->checked.complete()) << name;
  }
}

TEST(CompositionalCall, ExactLocalPointerCellsShareTheirCurrentCursor) {
  for (const auto *step : {"(*slot)++", "*slot+=3"}) {
    SCOPED_TRACE(step);
    const std::string source =
        "int client(void){const unsigned char text[]=\"abc\";"
        "const unsigned char *p=text;const unsigned char **slot=&p;"
        "while(p<text+4){if(*p==92)return p[4];" +
        std::string(step) + ";}return 0;}";
    const auto result =
        test::analyze(source, {.checkContracts = true, .checked = true});
    ASSERT_TRUE(result.ast);
    ASSERT_NE(result.summary("client"), nullptr);
    EXPECT_EQ(result.summary("client")->checked.complete(),
              std::string(step) == "(*slot)++");
  }
}

TEST(CompositionalCall, ByteContentsUseIndependentlyProvedCursorRelations) {
  for (const bool escape : {false, true}) {
    SCOPED_TRACE(escape);
    const std::string source = R"c(
      int scan(const unsigned char *data, size_t size) {
        const unsigned char *p=data+1,*end=data+1;
        while((size_t)(end-data)<size && *end!=34) ++end;
        if((size_t)(end-data)>=size || *end!=34) return 0;
        while(p<end) { if(*p==92) return p[size]; ++p; }
        return 0;
      }
      int client(void) { const unsigned char text[]={34,97,)c" +
                               std::string(escape ? "92" : "98") +
                               R"c(,99,34,0};
        return scan(text,sizeof text);
      }
    )c";
    const auto result =
        test::analyze(source, {.checkContracts = true, .checked = true});
    ASSERT_TRUE(result.ast);
    ASSERT_NE(result.summary("client"), nullptr);
    EXPECT_EQ(result.summary("client")->checked.complete(), !escape);
  }
}

TEST(CompositionalCall, CompleteCalleeFramesRequireActualConstantObjects) {
  for (const std::string variant : {"static", "local", "wrong", "mutable-write",
                                    "const-write", "partial", "unknown"}) {
    SCOPED_TRACE(variant);
    const bool write = variant == "mutable-write" || variant == "const-write";
    std::string source = "static unsigned config;struct state{unsigned n;};";
    if (variant == "unknown") {
      source += "void change(struct state *,unsigned char *);";
    } else {
      source +=
          "void change(struct state *s,unsigned char *p){s->n=1;config=1;";
      if (write)
        source += "p[0]=98;";
      source += '}';
    }
    source += "int inspect(struct state *s,const unsigned char *p){"
              "change(s,(unsigned char*)p);if(p[0]!=97)return p[8];return 0;}"
              "int client(void){struct state s={0};";
    if (variant == "local")
      source += "const unsigned char p[]={97};";
    else if (variant == "partial")
      source += "unsigned char p[1];";
    else if (variant == "mutable-write")
      source += "unsigned char p[]={97};";
    else if (variant == "wrong")
      source += "static const unsigned char p[]={98};";
    else
      source += "static const unsigned char p[]={97};";
    source += "return inspect(&s,p);}";
    const auto result =
        test::analyze(source, {.checkContracts = true, .checked = true});
    ASSERT_NE(result.summary("client"), nullptr);
    const auto &contract = result.summary("client")->checked;
    EXPECT_EQ(contract.complete(), variant == "static" || variant == "local");
    if (contract.complete())
      EXPECT_TRUE(contract.requirements.empty());
  }
}

TEST(CompositionalCall, ByteLoopPartitionsKeepActualFirstExitAndFallback) {
  for (const std::string variant :
       {"while", "do", "helper", "skipped", "changed", "missing",
        "uninitialized", "late"}) {
    SCOPED_TRACE(variant);
    const std::string step = variant == "skipped" ? "i+=2" : "i++";
    std::string source = "unsigned inner(const unsigned char *p,unsigned n) {";
    if (variant == "do")
      source += "unsigned i=0;if(!n)return 0;do{if(p[i]==34)return i;i++;}"
                "while(i<n);return i;}";
    else
      source += "unsigned i=0;while(i<n&&p[i]!=34)" + step + ";return i;}";
    source +=
        "unsigned scan(const unsigned char *p,unsigned n){return inner(p,n);}"
        "int client(void){";
    if (variant == "late") {
      // This actual out-of-bounds access occurs beyond the last partition.
      source += "static const unsigned char p[40]={";
      for (unsigned i = 0; i < 39; ++i)
        source += "97,";
      source += "34};";
    } else if (variant == "missing") {
      source += "static const unsigned char p[]={97,98,99,100,101,102};";
    } else if (variant == "changed") {
      source += "unsigned char p[]={97,34,98,99,100,34};p[1]=120;";
    } else {
      source += "static const unsigned char p[]={97,34,98,99,100,34};";
    }
    source += variant == "uninitialized" ? "char out[2];out[0]=1;"
                                         : "char out[2]={1,2};";
    source += variant == "helper" ? "return out[scan(p,sizeof p)];}"
                                  : "return out[inner(p,sizeof p)];}";
    const auto result =
        test::analyze(source, {.checkContracts = true, .checked = true});
    ASSERT_NE(result.summary("client"), nullptr);
    const auto &contract = result.summary("client")->checked;
    EXPECT_EQ(contract.complete(),
              variant == "while" || variant == "do" || variant == "helper");
    if (contract.complete())
      EXPECT_TRUE(contract.requirements.empty());
  }
}

TEST(CompositionalCall, ByteSwitchPartitionsRetainFirstExitAndFallback) {
  for (const std::string variant :
       {"direct", "forward", "shifted", "skip", "changed", "missing",
        "uninitialized", "beyond-bound"}) {
    SCOPED_TRACE(variant);
    std::string source = R"c(
      typedef __SIZE_TYPE__ size_t;
      static size_t scan(const unsigned char *p,size_t n,size_t start) {
        size_t i;
        for(i=0;start+i<n;i++) {
          switch(p[start+i]) {case '1':case '2':break;default:
    )c";
    if (variant == "skip")
      source += "i++;";
    source += R"c(
            goto done;
          }
        }
      done:return i;
      }
      static size_t forward(const unsigned char *p,size_t n,size_t start) {
        return scan(p,n,start);
      }
      int client(void) {
    )c";
    if (variant == "shifted")
      source += "const unsigned char p[]=\"q1x2x\";";
    else if (variant == "changed")
      source += "unsigned char p[]=\"1x2x\";p[1]='1';";
    else if (variant == "missing")
      source += "const unsigned char p[]=\"1122\";";
    else if (variant == "uninitialized")
      source += "unsigned char p[5];p[0]='1';";
    else if (variant == "beyond-bound")
      source += "const unsigned char p[]=\"" + std::string(39, '1') + "x\";";
    else
      source += "const unsigned char p[]=\"1x2x\";";
    source += variant == "forward" ? "size_t k=forward(p,sizeof p,"
                                   : "size_t k=scan(p,sizeof p,";
    source += variant == "shifted" ? "1);" : "0);";
    source += "unsigned char out[1]={0};if(k==1)return 0;return out[k+1];}";
    const auto result =
        test::analyze(source, {.checkContracts = true, .checked = true});
    ASSERT_NE(result.summary("client"), nullptr);
    const auto &contract = result.summary("client")->checked;
    EXPECT_EQ(contract.complete(), variant == "direct" ||
                                       variant == "forward" ||
                                       variant == "shifted");
    if (contract.complete())
      EXPECT_TRUE(contract.requirements.empty());
  }
}

TEST(CompositionalCall, ExactBytesRequireCurrentInitializedStorage) {
  const auto result = test::analyze(R"c(
    int inspect(const unsigned char *p, unsigned n) {
      for (unsigned i=0; i<n; ++i)
        if (p[i]=='\\') return p[n];
      return 0;
    }
    int good(void) {
      const unsigned char text[]={'a','b','c'};
      return inspect(text,3);
    }
    int offset(void) {
      const unsigned char text[]={'\\','a','b','c'};
      return inspect(text+1,3);
    }
    int changed(void) {
      unsigned char text[]={'a','b','c'}; text[1]='\\';
      return inspect(text,3);
    }
    int unknown_write(unsigned i) {
      unsigned char text[]={'a','b','c'};
      if(i<3) text[i]='\\';
      return inspect(text,3);
    }
    int partial(void) {
      unsigned char text[3]; text[0]='a';
      return inspect(text,3);
    }
    int local(void) {
      const unsigned char text[]="abc";
      const unsigned char *p=text;
      while(p<text+4) { if(*p=='\\') return p[4]; ++p; }
      return 0;
    }
    int escaped(void) {
      const unsigned char text[]="abc";
      const unsigned char *p=text;
      while(p<text+4) { if(*p=='\\') return p[4]; p+=2; }
      return *p;
    }
  )c",
                                    {.checkContracts = true, .checked = true});
  ASSERT_TRUE(result.ast);
  for (const auto *name : {"good", "offset", "local"}) {
    ASSERT_NE(result.summary(name), nullptr);
    EXPECT_TRUE(result.summary(name)->checked.complete()) << name;
    EXPECT_TRUE(result.summary(name)->checked.requirements.empty()) << name;
  }
  for (const auto *name : {"changed", "unknown_write", "partial", "escaped"}) {
    ASSERT_NE(result.summary(name), nullptr);
    EXPECT_FALSE(result.summary(name)->checked.complete()) << name;
  }
}

static std::size_t countContextDiagnostic(const test::AnalysisResult &result,
                                          std::string_view id) {
  return static_cast<std::size_t>(
      std::ranges::count_if(result.diagnostics.diagnostics(),
                            [id](const core::Diagnostic &diagnostic) {
                              return diagnostic.id == id;
                            }));
}

static void expectCleanContext(const std::string &code) {
  const auto result = test::analyze(code);
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty())
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

static void expectContextError(const std::string &code, std::string_view id) {
  const auto result = test::analyze(code);
  ASSERT_TRUE(result.ast);
  EXPECT_GT(countContextDiagnostic(result, id), 0U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
  EXPECT_EQ(countContextDiagnostic(result, core::diag::AnalysisIncomplete), 0U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

static const std::string Entry = R"c(
void test(void) { char *p = malloc(4); if (!p) return;
)c";

TEST(CompositionalCall, ReleaseBeforeReadAndWriteReportAtTheCallee) {
  for (const auto *operation : {"*b = 1;", "return *b;"}) {
    const auto result = test::analyze(
        std::string("static int zap(char *a, char *b) { free(a); ") +
        operation + " return 0; }" + Entry + "zap(p, p); }");
    ASSERT_TRUE(result.ast);
    EXPECT_EQ(countContextDiagnostic(result, core::diag::UseAfterFree), 1U);
    ASSERT_EQ(result.diagnostics.size(), 1U);
    EXPECT_EQ(result.diagnostics.diagnostics()[0].location.line, 1U);
    EXPECT_FALSE(result.diagnostics.diagnostics()[0].notes.empty());
  }
}

TEST(CompositionalCall, ReadingOrWritingBeforeReleaseRemainsClean) {
  expectCleanContext(R"c(
static void zap(char *a, char *b) { *b = 1; free(a); }
)c" + Entry + "zap(p, p); }");
  expectCleanContext(R"c(
static int zap(char *a, char *b) { int value = *b; free(a); return value; }
)c" + Entry + "(void)zap(p, p); }");
}

TEST(CompositionalCall, TwoSourceReleasesDifferFromTwoPathsOfOneRelease) {
  expectContextError(R"c(
static void zap(char *a, char *b) { free(a); free(b); }
)c" + Entry + "zap(p, p); }",
                     core::diag::DoubleFree);
  expectCleanContext(R"c(
static void zap(char *a, char *b) {
  char *saved = a;
  if (saved == b) free(saved);
  else { free(a); free(b); }
}
)c" + Entry + "zap(p, p); }");
}

TEST(CompositionalCall, IndependentAllocationsKeepIndependentLifetimes) {
  expectCleanContext(R"c(
static void zap(char *a, char *b) { free(a); *b = 1; }
)c" + Entry + R"c(
char *q = malloc(4); if (!q) { free(p); return; }
zap(p, q); free(q); }
)c");
}

TEST(CompositionalCall, AliasedOutputStorageCarriesOrderAndFinalValues) {
  expectContextError(R"c(
static void zap(char **a, char **b) { free(*a); **b = 1; }
)c" + Entry + "zap(&p, &p); }",
                     core::diag::UseAfterFree);
  expectCleanContext(R"c(
static void zap(char **a, char **b) { free(*a); *a = 0; if (*b) **b = 1; }
)c" + Entry + "zap(&p, &p); free(p); }");
  expectCleanContext(R"c(
static void zap(char **a, char **b) {
  free(*a); *a = malloc(4); if (*b) **b = 1;
}
)c" + Entry + "zap(&p, &p); if (p) *p = 2; free(p); }");
}

TEST(CompositionalCall, ReplacingTheCellDoesNotReviveASavedInput) {
  expectContextError(R"c(
static void zap(char **a, char **b) {
  char *saved = *b; free(*a); *a = malloc(4); *saved = 1;
}
)c" + Entry + "zap(&p, &p); free(p); }",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, DifferentOutputCellsMayContainTheSameChild) {
  expectContextError(R"c(
static void zap(char **a, char **b) { free(*a); *a = 0; if (*b) **b = 1; }
)c" + Entry + "char *q = p; zap(&p, &q); }",
                     core::diag::UseAfterFree);
  expectCleanContext(R"c(
static void zap(char **a, char **b) { free(*a); *a = 0; if (*b) **b = 1; }
)c" + Entry + "char *q = malloc(4); zap(&p, &q); free(q); }");
}

TEST(CompositionalCall, PointerValueAndAddressOfItsCellAreDistinct) {
  expectCleanContext(R"c(
static void zap(char **out, char *value) { *out = 0; free(value); }
)c" + Entry + "zap(&p, p); free(p); }");
}

TEST(CompositionalCall, RecordChildrenRetainSharedIdentity) {
  expectContextError(R"c(
struct Box { char *data; };
static void zap(struct Box *a, struct Box *b) { free(a->data); free(b->data); }
)c" + Entry + "struct Box a = {p}, b = {p}; zap(&a, &b); }",
                     core::diag::DoubleFree);
}

TEST(CompositionalCall, EntryScalarFactsPruneOnlyTheirFeasibleBranch) {
  expectCleanContext(R"c(
static void zap(char *a, char *b, int release) {
  if (release) free(a); else { *b = 1; free(a); }
}
)c" + Entry + "zap(p, p, 0); }");
  expectContextError(R"c(
static void zap(char *a, char *b, int release) { if (release) free(a); *b = 1; }
)c" + Entry + "zap(p, p, 1); }",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, EntryFieldFactsAreNotLostWhenAliasesAreInstalled) {
  expectCleanContext(R"c(
struct Box { char *data; int release; };
static void zap(struct Box *a, char *b) {
  if (a->release) free(a->data); else *b = 1;
}
)c" + Entry + "struct Box a = {p, 0}; zap(&a, p); free(p); }");
}

TEST(CompositionalCall,
     InteriorPointersKeepTemporalIdentityAndRelativeOffsets) {
  const auto result = test::analyze(R"c(
static void zap(char *a, char *b) { free(a); *b = 1; }
)c" + Entry + "zap(p, p + 1); }");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countContextDiagnostic(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countContextDiagnostic(result, core::diag::InvalidRelease), 0U);
  expectCleanContext(R"c(
static void zap(char *a, char *b) { *b = 1; free(a); }
)c" + Entry + "zap(p, p + 1); }");
}

TEST(CompositionalCall, ForwardingKeepsNestedDiagnosticsAndCallNotes) {
  const auto result = test::analyze(R"c(
static void zap(char *a, char *b) { free(a); *b = 1; }
static void middle(char *a, char *b) { zap(a, b); }
static void outer(char *a, char *b) { middle(a, b); }
)c" + Entry + "outer(p, p); }");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countContextDiagnostic(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countContextDiagnostic(result, core::diag::AnalysisIncomplete), 0U);
}

TEST(CompositionalCall, CallbackTargetsAndDataAliasesSelectOneContext) {
  expectContextError(R"c(
static void drop(char *p) { free(p); }
static void zap(void (*fn)(char *), char *a, char *b) { fn(a); *b = 1; }
)c" + Entry + "zap(drop, p, p); }",
                     core::diag::UseAfterFree);
  expectCleanContext(R"c(
static void drop(char *p) { free(p); }
static void zap(void (*fn)(char *), char *a, char *b) { *b = 1; fn(a); }
)c" + Entry + "zap(drop, p, p); }");
}

TEST(CompositionalCall, KnownIndirectTargetsUseTheSameArgumentContext) {
  expectContextError(R"c(
static void zap(char *a, char *b) { free(a); *b = 1; }
)c" + Entry + "void (*fn)(char *, char *) = zap; fn(p, p); }",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, GlobalAndParameterInputsCanShareAnAllocation) {
  expectContextError(R"c(
char *global;
static void zap(char *a) { free(a); *global = 1; }
)c" + Entry + "global = p; zap(p); }",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, SelectedCellsKeepTheirPointeeIdentities) {
  expectContextError(R"c(
static void zap(char **a, int i, int j) { free(a[i]); *a[j] = 1; }
)c" + Entry + "char *items[2] = {p, p}; zap(items, 0, 1); }",
                     core::diag::UseAfterFree);
  expectCleanContext(R"c(
static void zap(char **a, int i, int j) { free(a[i]); *a[j] = 1; }
)c" + Entry + R"c(
char *q = malloc(4); if (!q) { free(p); return; }
char *items[2] = {p, q}; zap(items, 0, 1); free(q); }
)c");
}

TEST(CompositionalCall, UnsafeRequestsRetainEffectsWithoutDelayedReports) {
  expectCleanContext(R"c(
static void zap(char *a, char *b) { free(a); *b = 1; }
)c" + Entry + "UNSAFE { zap(p, p); } }");
  expectCleanContext(R"c(
static void UNSAFE zap(char *a, char *b) { free(a); *b = 1; }
)c" + Entry + "zap(p, p); }");
  expectContextError(R"c(
static void zap(char *a, char *b) { free(a); *b = 1; }
)c" + Entry + "UNSAFE { zap(p, p); } *p = 2; }",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, UnsafeAndSafeRequestsDoNotShareReportingState) {
  expectContextError(R"c(
static void zap(char *a, char *b) { free(a); *b = 1; }
static void unchecked(char *p) { UNSAFE { zap(p, p); } }
)c" + Entry + "zap(p, p); }",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, BoundedRecursiveContextsReachTheBaseCase) {
  expectContextError(R"c(
static void zap(char *a, char *b, int n) {
  if (n) zap(a, b, n - 1); else { free(a); *b = 1; }
}
)c" + Entry + "zap(p, p, 2); }",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, AnUnconditionalBodyErrorSurvivesContextSelection) {
  expectContextError(R"c(
static void zap(char *a, char *b) { *b = 1; free(a); free(a); }
)c" + Entry + "zap(p, p); }",
                     core::diag::DoubleFree);
}

static const std::string Shares = R"c(
struct Object { unsigned refs; int value; };
static struct Object *retain(struct Object *p) { p->refs++; return p; }
static void drop(struct Object *p) { if (--p->refs == 0) free(p); }
static void zap(struct Object *a, struct Object *b) { drop(a); drop(b); }
void test(void) {
  struct Object *p = malloc(sizeof *p); if (!p) return; p->refs = 1;
)c";

TEST(CompositionalCall, SameShareAndSeparatelyRetainedSharesDiffer) {
  expectContextError(Shares + "zap(p, p); }", core::diag::DoubleFree);
  expectCleanContext(Shares + "struct Object *q = retain(p); zap(p, q); }");
}

TEST(CompositionalCall, ReanalysisInvalidatesTheResultsOfChangedCallees) {
  auto result = test::analyze(R"c(
static void zap(char *a, char *b) { *b = 1; free(a); }
)c" + Entry + "zap(p, p); }");
  ASSERT_TRUE(result.ast);
  auto &store = result.analyzer->summaries();
  EXPECT_FALSE(store.memorySpecialized.empty());
  core::FunctionSummary changed;
  changed.neverReturns = true;
  ASSERT_TRUE(result.function("zap"));
  store.setInferred(*result.function("zap"), changed);
  EXPECT_TRUE(store.memorySpecialized.empty());
  EXPECT_TRUE(store.memoryDiagnostics.empty());
  EXPECT_FALSE(store.memoryRequests.empty());
}

TEST(CompositionalCall, ExportedRequestsAndResultsSurviveSidecars) {
  auto result = test::analyze(R"c(
static void zap(char *a, char *b) { free(a); *b = 1; }
)c" + Entry + "zap(p, p); }");
  ASSERT_TRUE(result.ast);
  frontend::UnitRecord record;
  record.exports = result.analyzer->exports();
  ASSERT_TRUE(record.exports.functions.contains("zap"));
  EXPECT_FALSE(
      record.exports.functions.at("zap").memorySpecializations.empty());
  const auto parsed =
      frontend::parseUnitRecord(frontend::printUnitRecord(record));
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(record.exports.sameSummariesAs(parsed->exports));
}

TEST(CompositionalCall, WritesThroughAliasesInvalidateEntryFieldFacts) {
  expectContextError(R"c(
struct Box { char *data; int release; };
static void zap(struct Box *a, struct Box *b, char *p) {
  b->release = 1;
  if (a->release) free(a->data);
  *p = 1;
}
)c" + Entry + "struct Box a = {p, 0}; zap(&a, &a, p); }",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, AChangedScalarAtOneCallSiteSelectsANewContext) {
  expectContextError(R"c(
static void zap(char *a, char *b, int release) { if (release) free(a); *b = 1; }
void test(void) {
  for (int i = 0; i < 2; ++i) {
    char *p = malloc(4); if (!p) return;
    zap(p, p, i);
    if (!i) free(p);
  }
}
)c",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, RecursiveAndDepthLimitsRemainExplicit) {
  const auto result = test::analyze(R"c(
static void zap(char *a, char *b, int n) {
  if (n) zap(a, b, n - 1); else { free(a); *b = 1; }
}
)c" + Entry + "zap(p, p, 20); }");
  ASSERT_TRUE(result.ast);
  EXPECT_GT(countContextDiagnostic(result, core::diag::AnalysisIncomplete), 0U);
  EXPECT_TRUE(std::ranges::any_of(
      result.diagnostics.diagnostics(), [](const core::Diagnostic &diagnostic) {
        return diagnostic.message == "analysis is incomplete: call context "
                                     "unavailable or limit reached";
      }));
}

TEST(CompositionalCall, TooManyDistinctContextsRetainGenericEffects) {
  std::string code = R"c(
static void zap(char *a, char *b, int selector) {
  if (selector < 0) free(a); *b = 1;
}
void test(void) {
)c";
  for (unsigned i = 0; i < core::MaxMemoryContexts + 1; ++i)
    code += "{ char *p = malloc(4); if (!p) return; zap(p, p, " +
            std::to_string(i) + "); free(p); }\n";
  const auto result = test::analyze(code + "}");
  ASSERT_TRUE(result.ast);
  EXPECT_GT(countContextDiagnostic(result, core::diag::AnalysisIncomplete), 0U);
  for (const auto &[symbol, requests] :
       result.analyzer->summaries().memoryRequests) {
    (void)symbol;
    EXPECT_LE(requests.size(), core::MaxMemoryContexts);
  }
}

TEST(CompositionalCall, OversizedInputFootprintsHaveAnExplicitBoundary) {
  std::string code = "static void zap(";
  for (unsigned i = 0; i < core::MaxCallContextPaths + 1; ++i)
    code += (i ? ", char *p" : "char *p") + std::to_string(i);
  code += ") { free(p0);";
  for (unsigned i = 1; i < core::MaxCallContextPaths + 1; ++i)
    code += "*p" + std::to_string(i) + " = 1;";
  code += "}" + Entry + "zap(p";
  for (unsigned i = 1; i < core::MaxCallContextPaths + 1; ++i)
    code += ", p";
  const auto result = test::analyze(code + ");}");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(std::ranges::any_of(
      result.diagnostics.diagnostics(), [](const core::Diagnostic &diagnostic) {
        return diagnostic.message ==
               "analysis is incomplete: call context input path limit reached";
      }));
}

TEST(CompositionalCall, CallbackAndScalarPremisesShareOneBodyCase) {
  core::AnalysisStats stats;
  AnalysisOptions options;
  options.checkedFunctions.insert("client");
  options.stats = &stats;
  const auto result = test::analyze(R"c(
static void fill(char *p) { *p=7; }
int invoke(int enabled, void (*write)(char *), char *p) {
  if(!enabled) return 0;
  write(p); return *p;
}
int client(void) { char p[1]; return invoke(1,fill,p); }
)c",
                                    options);
  ASSERT_TRUE(result.ast);
  ASSERT_NE(result.summary("client"), nullptr);
  EXPECT_TRUE(result.summary("client")->checked.complete());
  EXPECT_TRUE(result.summary("client")->checked.requirements.empty());
  EXPECT_GT(stats.count("combined_callback_case_requests"), 0U);
  const auto &store = result.analyzer->summaries();
  const auto requests = store.callbackRequests.find("invoke");
  EXPECT_TRUE(requests == store.callbackRequests.end() ||
              requests->second.empty());
}

TEST(CompositionalCall, InvalidTypedContextsAreNeverCachedAsChecked) {
  auto result = test::analyze("void zap(int value) { (void)value; }");
  ASSERT_TRUE(result.ast);
  core::CallContext input;
  input.facts[core::SummaryPath::param(0)] =
      core::ValueFact::of(core::Outcome::NonNull);
  auto &store = result.analyzer->summaries();
  EXPECT_FALSE(store.specializeMemory("zap", input, {}));
  EXPECT_TRUE(store.memorySpecialized.empty());
}

TEST(CompositionalCall, OwnershipAnnotationsStillCheckKnownBodiesInContext) {
  expectContextError(R"c(
static void zap(char *OWNED a, char *b) { free(a); *b = 1; }
)c" + Entry + "zap(p, p); }",
                     core::diag::UseAfterFree);
  expectCleanContext(R"c(
static void zap(char *OWNED a, char *b) { *b = 1; free(a); }
)c" + Entry + "zap(p, p); }");
}

TEST(CompositionalCall, RewrittenSelectedCellsDoNotKeepObjectSeparation) {
  expectContextError(R"c(
static void zap(char **a, int i, int j) { free(a[i]); *a[j] = 1; }
static void middle(char **a, int i, int j) {
  free(a[j]); a[j] = a[i]; zap(a, i, j);
}
)c" + Entry + R"c(
char *q = malloc(4); if (!q) { free(p); return; }
char *items[2] = {p, q}; middle(items, 0, 1); }
)c",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, PointerReassignmentAtOneCallSiteRechecksTheContext) {
  expectContextError(R"c(
static void zap(char *a, char *b) { free(a); *b = 1; }
)c" + Entry + R"c(
char *q = malloc(4); if (!q) { free(p); return; }
char *a = p;
for (int i = 0; i < 2; ++i) { zap(a, q); a = q; }
}
)c",
                     core::diag::UseAfterFree);
}

TEST(CompositionalDatabase, ContextGlobalsAreRenumberedWithTheirSummaries) {
  UnitExports first;
  first.source = "first.c";
  (void)first.globals.idFor("unrelated");
  UnitExports second;
  second.source = "second.c";
  const auto global = core::SummaryPath::global(second.globals.idFor("shared"));
  core::CallContext input;
  ASSERT_TRUE(input.addAlias(
      {.first = core::SummaryPath::param(0), .second = global, .offset = {}}));
  core::FunctionSummary summary;
  summary.addEffect(global, {.freed = true});
  second.functions["zap"].memorySpecializations[input].assign(
      std::move(summary));
  second.memoryRequests["zap"].insert(input);
  ProgramDatabase db;
  db.add(first);
  db.add(second);
  const auto &requests = db.memoryRequestsFor("zap");
  ASSERT_EQ(requests.size(), 1U);
  const auto *found = db.findMemorySpecialization("zap", *requests.begin());
  ASSERT_NE(found, nullptr);
  const auto remapped = requests.begin()->aliases.begin()->second;
  EXPECT_NE(remapped, global);
  EXPECT_TRUE(found->effectOf(remapped).freed);
  EXPECT_FALSE(found->effectOf(global).freed);
}

TEST(CompositionalDatabase, DuplicateDefinitionsJoinContextEffects) {
  UnitExports first;
  UnitExports second;
  core::CallContext input;
  ASSERT_TRUE(input.addAlias({.first = core::SummaryPath::param(0),
                              .second = core::SummaryPath::param(1),
                              .offset = {}}));
  core::FunctionSummary releases;
  releases.addEffect(core::SummaryPath::param(0), {.freed = true});
  first.functions["zap"].memorySpecializations[input].assign(
      std::move(releases));
  core::FunctionSummary writes;
  writes.addEffect(core::SummaryPath::param(1).deref(), {.written = true});
  second.functions["zap"].memorySpecializations[input].assign(
      std::move(writes));
  ProgramDatabase db;
  db.add(first);
  db.add(second);
  const auto *found = db.findMemorySpecialization("zap", input);
  ASSERT_NE(found, nullptr);
  EXPECT_TRUE(found->frees(0));
  EXPECT_TRUE(found->effectOf(core::SummaryPath::param(1).deref()).written);
}

TEST(CompositionalDatabase, MissingGlobalRejectsTheEntireContext) {
  UnitExports unit;
  core::CallContext input;
  ASSERT_TRUE(input.addAlias({.first = core::SummaryPath::param(0),
                              .second = core::SummaryPath::global(4),
                              .offset = {}}));
  unit.memoryRequests["zap"].insert(input);
  core::FunctionSummary summary;
  summary.addEffect(core::SummaryPath::param(0), {.freed = true});
  unit.functions["zap"].memorySpecializations[input].assign(std::move(summary));
  ProgramDatabase db;
  db.add(unit);
  EXPECT_TRUE(db.memoryRequestsFor("zap").empty());
  EXPECT_EQ(db.findMemorySpecialization("zap", input), nullptr);
}

// RFC 0020: imported contracts retain full provenance and stable pointers.
TEST(CompositionalDatabase, ImportedContextsReuseCopiesButStillRecordRequests) {
  auto parsed = test::analyze("void zap(char *a, char *b);");
  ASSERT_TRUE(parsed.ast);
  ASSERT_TRUE(parsed.function("zap"));
  core::CallContext input;
  ASSERT_TRUE(input.addAlias({.first = core::SummaryPath::param(0),
                              .second = core::SummaryPath::param(1),
                              .offset = {}}));
  const core::CallbackBindings callbacks{
      {core::SummaryPath::param(0), core::CallTargets::function("target")}};
  UnitExports unit;
  auto &function = unit.functions["zap"];
  core::FunctionSummary generic;
  generic.addEffect(core::SummaryPath::param(0), {.freed = true});
  function.summary.assign(std::move(generic));
  function.memorySpecializations[input].assign(function.summary.get());
  function.specializations[callbacks].assign(function.summary.get());
  ProgramDatabase db;
  db.add(unit);
  SummaryStore store;
  core::AnalysisStats stats;
  store.stats = &stats;
  store.setContext(&parsed.ast->getASTContext());
  store.setDatabase(&db);
  const auto memory = store.specializeMemory("zap", input, {});
  const auto callback =
      store.specialize(*parsed.function("zap"), callbacks, {});
  const auto callable = store.lookupSymbol("zap");
  ASSERT_TRUE(memory);
  ASSERT_TRUE(callback);
  ASSERT_TRUE(callable);
  EXPECT_EQ(stats.count("program_import_misses"), 3U);
  store.memoryRequests.clear();
  store.callbackRequests.clear();
  SummaryStore::Dependencies dependencies;
  store.beginDependencies(dependencies);
  const auto memoryAgain = store.specializeMemory("zap", input, {});
  const auto callbackAgain =
      store.specialize(*parsed.function("zap"), callbacks, {});
  const auto callableAgain = store.lookupSymbol("zap");
  store.endDependencies();
  ASSERT_TRUE(memoryAgain);
  ASSERT_TRUE(callbackAgain);
  ASSERT_TRUE(callableAgain);
  EXPECT_EQ(memory->summary, memoryAgain->summary);
  EXPECT_EQ(callback->summary, callbackAgain->summary);
  EXPECT_EQ(callable->summary, callableAgain->summary);
  EXPECT_EQ(memoryAgain->source, SummarySource::Program);
  EXPECT_EQ(stats.count("program_import_hits"), 3U);
  EXPECT_TRUE(dependencies.contains("zap"));
  EXPECT_TRUE(store.memoryRequests.at("zap").contains(input));
  EXPECT_TRUE(store.callbackRequests.at("zap").contains(callbacks));

  // Replacing the database must preserve old pointers while importing the
  // replacement contract, even when it has identical lookup keys.
  ProgramDatabase replacement;
  core::FunctionSummary replacementSummary;
  replacementSummary.addEffect(core::SummaryPath::param(1), {.freed = true});
  function.summary.assign(std::move(replacementSummary));
  function.memorySpecializations[input].assign(function.summary.get());
  function.specializations[callbacks].assign(function.summary.get());
  replacement.add(unit);
  db = replacement;
  const auto replacedMemory = store.specializeMemory("zap", input, {});
  const auto replacedCallback =
      store.specialize(*parsed.function("zap"), callbacks, {});
  const auto replacedCallable = store.lookupSymbol("zap");
  for (const auto &resolved :
       {replacedMemory, replacedCallback, replacedCallable}) {
    ASSERT_TRUE(resolved);
    EXPECT_FALSE(resolved->summary->frees(0));
    EXPECT_TRUE(resolved->summary->frees(1));
  }
  for (const auto &resolved : {memory, callback, callable}) {
    EXPECT_TRUE(resolved->summary->frees(0));
    EXPECT_FALSE(resolved->summary->frees(1));
  }
  EXPECT_EQ(stats.count("program_import_misses"), 6U);
  db.clear();
  EXPECT_FALSE(store.specializeMemory("zap", input, {}));
  EXPECT_FALSE(store.specialize(*parsed.function("zap"), callbacks, {}));
  EXPECT_FALSE(store.lookupSymbol("zap"));
  db.add(unit);
  ASSERT_TRUE(store.lookupSymbol("zap"));
  EXPECT_EQ(stats.count("program_import_misses"), 7U);
}

TEST(CompositionalDatabase, ImportGenerationsFollowCopiesAndGlobalNumbering) {
  auto first = test::analyze("extern char *shared;");
  auto second = test::analyze("extern char *shared;");
  auto absent = test::analyze("");
  ASSERT_TRUE(first.ast);
  ASSERT_TRUE(second.ast);
  ASSERT_TRUE(absent.ast);
  UnitExports unit;
  const auto shared = core::SummaryPath::global(unit.globals.idFor("shared"));
  core::FunctionSummary generic;
  generic.addEffect(shared, {.freed = true});
  unit.functions["zap"].summary.assign(std::move(generic));
  ProgramDatabase db;
  db.add(unit);
  ProgramDatabase copy = db;
  const auto original = db.importGeneration();
  EXPECT_EQ(original, copy.importGeneration());
  UnitExports prefix;
  (void)prefix.globals.idFor("earlier");
  copy.clear();
  copy.add(prefix);
  copy.add(unit);
  EXPECT_NE(original, copy.importGeneration());
  EXPECT_EQ(original, db.importGeneration());
  SummaryStore store;
  core::AnalysisStats stats;
  store.stats = &stats;
  store.setDatabase(&db);
  store.setContext(&first.ast->getASTContext());
  const auto imported = store.lookupSymbol("zap");
  ASSERT_TRUE(imported);
  EXPECT_TRUE(imported->summary->effectOf(shared).freed);
  store.setDatabase(&copy);
  const auto renumbered = store.lookupSymbol("zap");
  ASSERT_TRUE(renumbered);
  EXPECT_TRUE(renumbered->summary->effectOf(shared).freed);
  store.setContext(&second.ast->getASTContext());
  const auto otherAST = store.lookupSymbol("zap");
  ASSERT_TRUE(otherAST);
  EXPECT_FALSE(otherAST->summary->effectOf(shared).freed);
  EXPECT_TRUE(otherAST->summary->effectOf(core::SummaryPath::global(1)).freed);
  store.setContext(&absent.ast->getASTContext());
  const auto dropped = store.lookupSymbol("zap");
  ASSERT_TRUE(dropped);
  EXPECT_TRUE(dropped->summary->effects.empty());
  EXPECT_TRUE(imported->summary->effectOf(shared).freed);
  EXPECT_EQ(stats.count("program_import_misses"), 4U);
  const auto beforeRenumber = copy.importGeneration();
  (void)copy.renumbered(prefix);
  EXPECT_NE(copy.importGeneration(), beforeRenumber);
  const auto beforeCallback = copy.importGeneration();
  copy.addCallbackInformation(unit);
  EXPECT_NE(copy.importGeneration(), beforeCallback);
}

TEST(CompositionalDatabase, InvalidTypedRequestRetainsOrdinaryBodyErrors) {
  UnitExports unit;
  core::CallContext input;
  input.facts[core::SummaryPath::param(0)] =
      core::ValueFact::of(core::Outcome::NonNull);
  unit.memoryRequests["zap"].insert(input);
  ProgramDatabase db;
  db.add(unit);
  const auto result = test::analyzeInProgram(R"c(
void zap(int value) {
  char *p = malloc(4); if (!p) return;
  free(p); *p = 1;
}
)c",
                                             &db);
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countContextDiagnostic(result, core::diag::UseAfterFree), 1U);
}

TEST(CompositionalCall, CopiedBorrowOffsetsCannotProveDifferentAddressesEqual) {
  for (const auto *storage :
       {"char data[2]; char *a = data; char *b = data + 1;",
        "struct Pair { char first; char second; } data; "
        "char *a = (char *)&data; char *b = &data.second;"}) {
    expectContextError(R"c(
static void zap(char *a, char *b, char *p) {
  if (a != b) free(p); *p = 1;
}
)c" + Entry + storage + "zap(a, b, p); }",
                       core::diag::UseAfterFree);
  }
}

static SummarySnapshot neverReturningContext() {
  core::FunctionSummary summary;
  summary.neverReturns = true;
  return std::make_shared<const core::FunctionSummary>(std::move(summary));
}

// RFC 0020: dependencies include misses and are inherited across nested hits.
TEST(ContextDependencies, CallSnapshotsSurviveReplacementAndNestedAnalysis) {
  auto parsed = test::analyze("static void zap(char *a, char *b) {}");
  ASSERT_TRUE(parsed.ast);
  ASSERT_TRUE(parsed.function("zap"));
  SummaryStore store;
  core::AnalysisStats stats;
  store.stats = &stats;
  store.setContext(&parsed.ast->getASTContext());
  core::FunctionSummary first;
  first.addEffect(core::SummaryPath::param(0), {.freed = true});
  ASSERT_TRUE(store.setInferred(*parsed.function("zap"), first));
  const auto before = store.lookup(*parsed.function("zap"));
  ASSERT_TRUE(before);
  store.beginAnalysis();
  const auto retained = store.retainSummary(*before);
  EXPECT_EQ(retained, before->summary);
  store.beginAnalysis();
  EXPECT_EQ(retained, store.retainSummary(*before));
  store.endAnalysis();
  EXPECT_EQ(retained, store.retainSummary(*before));
  core::FunctionSummary second;
  second.addEffect(core::SummaryPath::param(1), {.freed = true});
  ASSERT_TRUE(store.setInferred(*parsed.function("zap"), second));
  const auto after = store.lookup(*parsed.function("zap"));
  ASSERT_TRUE(after);
  const auto replaced = store.retainSummary(*after);
  EXPECT_NE(retained, replaced);
  EXPECT_TRUE(retained->frees(0));
  EXPECT_FALSE(retained->frees(1));
  EXPECT_FALSE(replaced->frees(0));
  EXPECT_TRUE(replaced->frees(1));
  store.endAnalysis();
  EXPECT_EQ(replaced, store.retainSummary(*after));
  EXPECT_TRUE(replaced->frees(1));
  EXPECT_EQ(stats.count("summary_shared_uses"), 5U);
  EXPECT_EQ(stats.count("summary_publications"), 2U);
}

TEST(ContextDependencies, ResolvedContractsOutliveInvalidationAndTheirStore) {
  std::optional<CallEffects> effects;
  std::optional<ResolvedSummary> builtin;
  SummarySnapshot contextual;
  {
    auto parsed = test::analyze("static void zap(char *a, char *b) {} "
                                "void caller(void) { zap(0, 0); }");
    ASSERT_TRUE(parsed.ast);
    ASSERT_TRUE(parsed.function("zap"));
    ASSERT_TRUE(parsed.function("caller"));
    const auto *body =
        clang::cast<clang::CompoundStmt>(parsed.function("caller")->getBody());
    const auto *call = clang::dyn_cast<clang::CallExpr>(*body->body_begin());
    ASSERT_TRUE(call);
    SummaryStore store;
    store.setContext(&parsed.ast->getASTContext());
    core::FunctionSummary summary;
    summary.addEffect(core::SummaryPath::param(0), {.freed = true});
    ASSERT_TRUE(store.setInferred(*parsed.function("zap"), summary));
    const auto resolved = store.lookup(*parsed.function("zap"));
    ASSERT_TRUE(resolved);
    effects = classifyCall(*call, store);
    ASSERT_TRUE(effects);
    EXPECT_EQ(effects->summary, resolved->summary);
    builtin = store.lookup(*parsed.function("malloc"));
    ASSERT_TRUE(builtin);
    const SummaryStore::MemoryContextKey key{"helper", {}};
    store.memorySpecialized[key] = neverReturningContext();
    store.memoryDependencies[key].insert("callee");
    store.beginAnalysis();
    contextual = store.memorySpecialized.at(key);
    store.invalidateDependency("callee");
    EXPECT_TRUE(store.memorySpecialized.empty());
    store.endAnalysis();
  }
  // The store, its retired nodes and the Clang AST have all been destroyed.
  EXPECT_TRUE(contextual->neverReturns);
  EXPECT_TRUE(effects->summary->frees(0));
  EXPECT_TRUE(builtin->summary->returnsFresh());
}

TEST(ContextDependencies, UnrelatedChangesKeepBothKindsOfSpecialization) {
  SummaryStore store;
  core::AnalysisStats stats;
  store.stats = &stats;
  const SummaryStore::MemoryContextKey memory{"caller", {}};
  const SummaryStore::ContextKey callback{"dispatch", {}};
  store.memorySpecialized[memory] = neverReturningContext();
  store.specialized[callback] = neverReturningContext();
  store.memoryDependencies[memory] = {"missing", "leaf"};
  store.callbackDependencies[callback] = {"leaf"};
  store.invalidateDependency("unrelated");
  EXPECT_EQ(store.memorySpecialized.size(), 1U);
  EXPECT_EQ(store.specialized.size(), 1U);
  store.invalidateDependency("missing");
  EXPECT_TRUE(store.memorySpecialized.empty());
  EXPECT_EQ(store.specialized.size(), 1U);
  store.invalidateDependency("leaf");
  EXPECT_TRUE(store.specialized.empty());
  EXPECT_EQ(stats.count("specialization_invalidations"), 2U);
}

TEST(ContextDependencies, NestedHitsAndGlobalFactsReachEveryActiveCaller) {
  SummaryStore store;
  SummaryStore::Dependencies outer;
  SummaryStore::Dependencies inner;
  store.beginDependencies(outer);
  store.noteDependency("outer");
  store.beginDependencies(inner);
  store.inheritDependencies({"leaf", "@sized", "@counts", "@callback-globals"});
  store.endDependencies();
  store.endDependencies();
  EXPECT_EQ(inner.size(), 4U);
  EXPECT_EQ(outer.size(), 5U);
  EXPECT_TRUE(outer.contains("leaf"));
  const SummaryStore::MemoryContextKey key{"f", {}};
  store.memoryDependencies[key] = inner;
  store.memorySpecialized[key] = neverReturningContext();
  const auto *active = store.memorySpecialized.at(key).get();
  store.beginAnalysis();
  store.invalidateDependency("@sized");
  EXPECT_TRUE(store.memorySpecialized.empty());
  // An applying caller owns a stable view until its outermost run ends.
  EXPECT_TRUE(active->neverReturns);
  store.endAnalysis();
}

TEST(FunctionPreparation, RepeatedContextRunsReuseCfgWithoutSharingFlowState) {
  core::AnalysisStats stats;
  auto result = test::analyze("void f(int *p) { *p = 1; }",
                              {.checked = true, .stats = &stats});
  ASSERT_TRUE(result.ast);
  EXPECT_GT(stats.count("cfg_reuses"), 0U);
  EXPECT_EQ(stats.count("cfg_builds"), 1U);
  EXPECT_EQ(stats.count("cfg_order_builds"), 1U);
  EXPECT_GT(stats.count("cfg_order_reuses"), 0U);
  EXPECT_GT(stats.count("liveness_reuses"), 0U);
}

TEST(ContextDependencies, InferredUpdatesInvalidateOnlyObservedFunctions) {
  auto result = test::analyze("void leaf(int *p){*p=1;} void caller(int "
                              "*p){leaf(p);} void unrelated(void){}");
  ASSERT_TRUE(result.ast);
  auto &store = result.analyzer->summaries();
  core::AnalysisStats stats;
  store.stats = &stats;
  const SummaryStore::MemoryContextKey key{"caller", {}};
  store.memorySpecialized[key] = neverReturningContext();
  store.memoryDependencies[key] = {"caller", "leaf"};
  auto changed = *store.inferredFor(*result.function("unrelated"));
  changed.neverReturns = true;
  EXPECT_TRUE(store.setInferred(*result.function("unrelated"), changed));
  EXPECT_TRUE(store.memorySpecialized.contains(key));
  changed = *store.inferredFor(*result.function("leaf"));
  changed.neverReturns = true;
  EXPECT_TRUE(store.setInferred(*result.function("leaf"), changed));
  EXPECT_FALSE(store.memorySpecialized.contains(key));
  EXPECT_EQ(stats.count("specialization_invalidations"), 1U);
}

TEST(ContextDependencies, ARealMemoryHitContributesItsTransitiveDependencies) {
  auto result =
      test::analyze("void leaf(int *p){*p=1;} void caller(int *p){leaf(p);}");
  ASSERT_TRUE(result.ast);
  auto &store = result.analyzer->summaries();
  core::AnalysisStats stats;
  AnalysisOptions options;
  options.stats = &stats;
  core::CallContext context;
  context.facts[core::SummaryPath::param(0)] =
      core::ValueFact::of(core::Outcome::NonNull);
  ASSERT_TRUE(store.specializeMemory("caller", context, options));
  SummaryStore::Dependencies outer;
  store.beginDependencies(outer);
  ASSERT_TRUE(store.specializeMemory("caller", context, options));
  store.endDependencies();
  EXPECT_GT(stats.count("specialization_hits"), 0U);
  EXPECT_TRUE(outer.contains("leaf"));
  EXPECT_TRUE(outer.contains("caller"));
}

TEST(ContextDependencies, ChangesDuringAnalysisCannotProduceACurrentSnapshot) {
  SummaryStore store;
  SummaryStore::Dependencies dependencies;
  store.beginDependencies(dependencies);
  store.noteDependency("missing");
  store.invalidateDependency("missing");
  store.noteDependency("missing");
  const auto snapshot = store.dependencySnapshot();
  store.endDependencies();
  EXPECT_FALSE(store.dependenciesCurrent(snapshot));
  store.beginDependencies(dependencies);
  const auto current = store.dependencySnapshot();
  store.endDependencies();
  EXPECT_TRUE(store.dependenciesCurrent(current));
  store.invalidateDependency("unrelated");
  EXPECT_TRUE(store.dependenciesCurrent(current));
}

TEST(ContextDependencies, SettledRecursiveValuesAreRecheckedBeforeReporting) {
  core::AnalysisStats stats;
  const auto result =
      test::analyze("int f(int n) { if (n <= 0) return 0; return f(n - 1); }",
                    {.stats = &stats});
  ASSERT_TRUE(result.ast);
  // RFC 0029 rechecks the body against settled conservative dependencies to
  // refine value outcomes; a pre-finalization silent result cannot supply it.
  EXPECT_EQ(stats.count("silent_function_reuses"), 0U);
  EXPECT_GT(stats.count("function_analyses"),
            stats.count("function_fixpoint_rounds"));
  EXPECT_EQ(countContextDiagnostic(result, core::diag::AnalysisIncomplete), 0U);
}

TEST(CompositionalCall, UnresolvedGlobalTargetsDoNotCreateDuplicateContexts) {
  const auto result = test::analyze(R"c(
void fill(char *p) { *p = 1; }
static void (*hook)(char *) = fill;
void set_hook(void (*fn)(char *)) { hook = fn; }
void invoke_hook(char *p) { hook(p); }
void forward_hook(char *p) { invoke_hook(p); }
int concrete(void) {
  char value;
  set_hook(fill);
  invoke_hook(&value);
  return value;
}
)c",
                                    {.checkContracts = true, .checked = true});
  ASSERT_TRUE(result.ast);
  const auto *generic = result.summary("forward_hook");
  const auto *concrete = result.summary("concrete");
  ASSERT_NE(generic, nullptr);
  ASSERT_NE(concrete, nullptr);
  EXPECT_FALSE(generic->checked.complete());
  EXPECT_FALSE(generic->callbackInputs.empty());
  EXPECT_TRUE(concrete->checked.complete());
  EXPECT_TRUE(concrete->checked.requirements.empty());
  const auto &store = result.analyzer->summaries();
  std::vector<core::CallbackBindings> requests;
  if (const auto found = store.callbackRequests.find("invoke_hook");
      found != store.callbackRequests.end())
    requests.insert(requests.end(), found->second.begin(), found->second.end());
  if (const auto found = store.memoryRequests.find("invoke_hook");
      found != store.memoryRequests.end())
    for (const auto &input : found->second)
      if (!input.callbacks.empty())
        requests.push_back(input.callbacks);
  ASSERT_FALSE(requests.empty());
  for (const auto &bindings : requests)
    for (const auto &[path, targets] : bindings) {
      EXPECT_TRUE(path.isGlobal());
      EXPECT_FALSE(targets.unknown);
      EXPECT_EQ(targets, core::CallTargets::function("fill"));
    }
}

TEST(CompositionalCall, AnUnexecutedRecursiveBranchKeepsCaseLocalLimits) {
  const std::string helper = R"c(
    struct state { unsigned depth, tag; char *data; };
    static int walk(struct state *s) {
      if (!s->tag) return 0;
      if (s->depth >= 1000) return 0;
      ++s->depth;
      if (*s->data == 1) return 100 / *s->data;
      return walk(s);
    }
  )c";
  for (const auto *entry :
       {"struct state s={0,0,0};return walk(&s);",
        "char data=1;struct state s={0,1,&data};return walk(&s);"}) {
    AnalysisOptions options;
    options.checkedFunctions.insert("client");
    const auto result =
        test::analyze(helper + "int client(void){" + entry + "}", options);
    ASSERT_NE(result.summary("client"), nullptr);
    EXPECT_TRUE(result.summary("client")->checked.complete()) << entry;
    EXPECT_TRUE(result.summary("client")->checked.requirements.empty());
    ASSERT_NE(result.summary("walk"), nullptr);
    EXPECT_FALSE(result.summary("walk")->checked.complete());
  }
}

TEST(CompositionalCall, ReachableRecursiveCasesRetainLimitsAndLocalErrors) {
  AnalysisOptions options;
  options.checkedFunctions.insert("client");
  for (const auto *entry :
       {"struct state s={0,1,0};", "char data=0;struct state s={0,1,&data};"}) {
    const auto result = test::analyze(R"c(
      struct state { unsigned depth, tag; char *data; };
      static int walk(struct state *s) {
        if (!s->tag) return 0;
        if (s->depth >= 1000) return 0;
        ++s->depth;
        if (*s->data == 1) return 100 / *s->data;
        return walk(s);
      }
      int client(void){
    )c" + std::string(entry) + "return walk(&s);}",
                                      options);
    ASSERT_NE(result.summary("client"), nullptr);
    EXPECT_FALSE(result.summary("client")->checked.complete()) << entry;
  }
}

TEST(CompositionalCall, CapturedPointeeValuesNeedExactLiveScalarStorage) {
  AnalysisOptions options;
  options.checkedFunctions.insert("client");
  for (const auto *entry : {"char data=1;struct state s={0,1,&data};data=0;",
                            "char data[2]={1,0};struct state s={0,1,data+1};",
                            "char *data=malloc(1);if(!data)return 0;*data=1;"
                            "struct state s={0,1,data};free(data);",
                            "int data=1;struct state s={0,1,(char*)&data};"}) {
    const auto result = test::analyze(R"c(
      void *malloc(__SIZE_TYPE__);
      void free(void *);
      struct state { unsigned depth, tag; char *data; };
      static int walk(struct state *s) {
        if (!s->tag) return 0;
        if (s->depth >= 1000) return 0;
        ++s->depth;
        if (*s->data == 1) return 100 / *s->data;
        return walk(s);
      }
      int client(void){
    )c" + std::string(entry) + "return walk(&s);}",
                                      options);
    ASSERT_NE(result.summary("client"), nullptr);
    EXPECT_FALSE(result.summary("client")->checked.complete()) << entry;
  }
}

TEST(CompositionalCall, MutableStateConstantsDoNotCrowdOutRecursiveInputs) {
  std::string code = R"c(
    struct node { struct node *next; unsigned tag; };
    struct state { unsigned seen, mode; };
    static int walk(struct node *p, struct state *s) {
      if (!p) return 0;
      s->seen = s->mode;
      if (p->tag == 1) return 1;
      return walk(p->next, s);
    }
  )c";
  for (unsigned i = 0; i < 36; ++i)
    code += "int generic_" + std::to_string(i) +
            "(struct node *p){struct state s={0," + std::to_string(i) +
            "};return walk(p,&s);}";
  code += R"c(
    int client(void) {
      struct node n = {0, 1};
      struct state s = {0, 100};
      return walk(&n, &s);
    }
    int bad(void) { struct node n = {0, 1}; return walk(&n, 0); }
  )c";
  AnalysisOptions options;
  options.checkedFunctions = {"client", "bad", "walk"};
  const auto result = test::analyze(code, options);
  ASSERT_NE(result.summary("client"), nullptr);
  EXPECT_TRUE(result.summary("client")->checked.complete());
  EXPECT_TRUE(result.summary("client")->checked.requirements.empty());
  ASSERT_NE(result.summary("bad"), nullptr);
  EXPECT_FALSE(result.summary("bad")->checked.complete());
  ASSERT_NE(result.summary("walk"), nullptr);
  EXPECT_FALSE(result.summary("walk")->checked.complete());
}

} // namespace weavec::analysis
