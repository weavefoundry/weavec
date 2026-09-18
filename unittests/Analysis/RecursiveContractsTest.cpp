//===- RecursiveContractsTest.cpp - Composed proofs (RFC 0029) -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "TestUtils.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace weavec::analysis {

static core::CheckedContract workflowContract(const std::string &source,
                                              const std::string &name) {
  AnalysisOptions options;
  options.checkedFunctions.insert(name);
  const auto unit = test::analyze(source, options);
  if (!unit.summary(name)) {
    ADD_FAILURE() << "missing workflow contract: " << name;
    return {};
  }
  return unit.summary(name)->checked;
}

TEST(RecursiveContracts, ConcreteBytesKeepCompleteInductiveWriterAvailable) {
  const auto result = test::analyze(R"c(
    struct writer { unsigned flags; unsigned char *data; size_t used, capacity; };
    int emit(const unsigned char *input,size_t n,struct writer *w) {
      if(!n)return 1;
      if(w->used>=w->capacity)return 0;
      w->data[w->used]=input[0];w->used++;
      if(!emit(input+1,n-1,w))return 0;
      return 1;
    }
    int main(void) {
      unsigned char input[]={1,2,3},output[3];
      struct writer w={7,output,0,3};
      (void)emit(input,3,&w);
      if(w.used)return w.data[w.used-1];
      return 0;
    }
  )c",
                                    {.checkedFunctions = {"emit", "main"}});
  ASSERT_TRUE(result.ast);
  ASSERT_NE(result.summary("emit"), nullptr);
  ASSERT_NE(result.summary("main"), nullptr);
  EXPECT_TRUE(result.summary("emit")->checked.complete());
  EXPECT_TRUE(result.summary("main")->checked.complete());
  EXPECT_TRUE(test::ids(result.diagnostics).empty());
}

TEST(RecursiveContracts, InPlaceExtensionAccountsForSuccessAndFailure) {
  for (const std::string variant :
       {"recursive", "lost-child", "duplicate-child", "nondecreasing",
        "failed-cleanup"}) {
    SCOPED_TRACE(variant);
    const std::string source =
        std::string(R"c(
      void *calloc(size_t, size_t); void free(void *);
      struct node { struct node *next, *child; };
      void drop(struct node *n) {
        while(n) { struct node *next=n->next; drop(n->child); free(n); n=next; }
      }
      int grow(struct node *n, unsigned depth) {
        if(!depth)return 1;
        struct node *c=calloc(1,sizeof *c);
        if(!c)return 0;
        if(!grow(c, )c") +
        (variant == "nondecreasing" ? "depth" : "depth-1") + ")) {" +
        (variant == "failed-cleanup" ? "" : "drop(c);") + "return 0;}" +
        (variant == "lost-child" ? "" : "n->child=c;") +
        (variant == "duplicate-child" ? "n->next=c;" : "") +
        R"c(
        return 1;
      }
      int client(unsigned depth) {
        struct node *n=calloc(1,sizeof *n); if(!n)return 0;
        (void)grow(n,depth); drop(n); return 0;
      }
    )c";
    const bool safe = variant == "recursive";
    const auto constructor = workflowContract(source, "grow");
    EXPECT_EQ(constructor.complete(), safe);
    EXPECT_EQ(std::ranges::any_of(
                  constructor.establishes,
                  [](const auto &post) {
                    return post.kind ==
                           core::CheckedRequirementKind::ContainerExtended;
                  }),
              safe);
    const auto client = workflowContract(source, "client");
    EXPECT_EQ(client.complete(), safe);
    EXPECT_TRUE(client.requirements.empty());
  }
}

TEST(RecursiveContracts, LocalAutomaticWritesFrameOnlyUnchangedEntryForests) {
  for (const std::string write :
       {"memset(state, 0, sizeof(state));", "memset(p, 0xa5, sizeof(*p));",
        "memset(p->left, 0xa5, sizeof(*p));",
        "struct node local = {3, 0, 0}; p->left = &local; "
        "memset(&local, 0xa5, sizeof(local));"}) {
    SCOPED_TRACE(write);
    const std::string source = R"c(
      void *memset(void *, int, size_t);
      struct node { unsigned value; struct node *left, *right; };
      struct state { unsigned depth, mode; };
      unsigned walk(const struct node *p) {
        if (!p) return 0;
        return p->value + walk(p->left) + walk(p->right);
      }
      unsigned inspect(struct node *p) {
        struct state state[1];
    )c" + write + R"c(
        state->depth = 1;
        return walk(p) + state->depth;
      }
      int client(void) {
        struct node child = {2, 0, 0}, root = {1, &child, 0};
        return inspect(&root) != 4;
      }
    )c";
    const bool safe = write == "memset(state, 0, sizeof(state));";
    EXPECT_EQ(workflowContract(source, "inspect").complete(), safe);
    const auto client = workflowContract(source, "client");
    EXPECT_EQ(client.complete(), safe);
    EXPECT_TRUE(client.requirements.empty());
  }
}

TEST(RecursiveContracts, OrdinaryAllocationsShareTheForestConservationLedger) {
  for (const auto &[action, safe] : std::vector<std::pair<std::string, bool>>{
           {"return data;", true},
           {"return data + 1;", false},
           {"return 0;", false},
           {"free(data); free(data); return 0;", false},
           {"unsigned char *out=realloc(data,2); "
            "if(!out){free(data);return 0;} return out;",
            true},
           {"unsigned char *out=realloc(data,2); if(!out)return 0; return out;",
            false},
           {"unsigned char *out=realloc(data,2); "
            "if(!out){free(data);return 0;} free(data); return out;",
            false},
           {"data=realloc(data,2); if(!data)return 0; return data;", false},
           {"free(data); data=0; unsigned char *out=realloc(data,2); "
            "if(!out)return 0; return out;",
            true}}) {
    SCOPED_TRACE(action);
    const std::string source = R"c(
      void *malloc(size_t); void *realloc(void *, size_t); void free(void *);
      struct node { unsigned value; struct node *left, *right; };
      unsigned walk(const struct node *p) {
        if (!p) return 0;
        return p->value + walk(p->left) + walk(p->right);
      }
      unsigned char *make(const struct node *p) {
        (void)walk(p);
        unsigned char *data=malloc(4);
        if(!data)return 0;
        data[0]=0;
    )c" + action + R"c(
      }
      int client(void) {
        struct node child={2,0,0}, root={1,&child,0};
        unsigned char *data=make(&root);free(data);return 0;
      }
    )c";
    EXPECT_EQ(workflowContract(source, "make").complete(), safe);
    const auto client = workflowContract(source, "client");
    EXPECT_EQ(client.complete(), safe);
    EXPECT_TRUE(client.requirements.empty());
  }
}

TEST(RecursiveContracts, MutatingFieldCursorCannotProjectOnlyItsEntryCell) {
  const std::string helper = R"c(
    struct reader { const unsigned char *data; size_t position, end; };
    unsigned consume(struct reader *r) {
      unsigned sum = 0;
      while (r->position < r->end) {
        sum += r->data[r->position];
        r->position++;
      }
      return sum;
    }
  )c";
  EXPECT_FALSE(workflowContract(helper + R"c(
    int client(void) {
      unsigned char data[] = {1, 2};
      struct reader r = {data, 0, 4};
      return (int)consume(&r);
    }
  )c",
                                "client")
                   .complete());
  EXPECT_FALSE(workflowContract(helper + R"c(
    int client(void) {
      unsigned char data[4]; data[0] = 1;
      struct reader r = {data, 0, 4};
      return (int)consume(&r);
    }
  )c",
                                "client")
                   .complete());
}

TEST(RecursiveContracts, DiscoveredBufferCanRequireItsUnchangedEntryBacking) {
  const std::string helper = R"c(
    size_t strlen(const char *);
    struct output { char *data; size_t capacity, cursor, depth; };
    int append(struct output *p, char c) {
      if (p->cursor >= p->capacity) return 0;
      p->data[p->cursor++] = c; return 1;
    }
    void update(struct output *p) {
      if (!p || !p->data) return;
      const char *old = p->data + p->cursor;
      p->cursor += strlen(old);
    }
  )c";
  EXPECT_TRUE(workflowContract(helper, "update").complete());
  const auto source = helper + R"c(
    void invalid(struct output *p) {
      char *entry = p->data; (void)entry;
      p->data = 0; p->capacity = p->cursor = 0;
      const char *q = p->data + p->cursor; (void)q;
    }
  )c";
  EXPECT_FALSE(workflowContract(source, "invalid").complete());
}

static const std::string OutputConstructor = R"c(
  void *calloc(size_t, size_t);
  struct node { unsigned char value; struct node *next; };
  static void destroy(struct node *p) { if(p){destroy(p->next);free(p);} }
  int build(const unsigned char *data, size_t n, struct node **out) {
    *out=0;
    if(!n)return 0;
    struct node *p=calloc(1,sizeof *p); if(!p)return 0;
    p->value=data[0];
    if(n>1 && !build(data+1,n-1,&p->next)){free(p);return 0;}
    *out=p; return 1;
  }
)c";

TEST(RecursiveContracts, OutputConstructionPublishesTheCompleteForest) {
  const auto contract = workflowContract(OutputConstructor, "build");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(std::ranges::any_of(contract.establishes, [](const auto &post) {
    return post.kind == core::CheckedRequirementKind::ContainerFresh &&
           post.path == core::SummaryPath::param(2).deref() &&
           post.on == core::Outcome::Positive;
  }));
  EXPECT_TRUE(workflowContract(OutputConstructor + R"c(
    int client(void) {
      unsigned char data[]={1,2,3}; struct node *p=0;
      if(build(data,3,&p))destroy(p); return 0;
    }
  )c",
                               "client")
                  .complete());
}

TEST(RecursiveContracts, OutputConstructionNeedsActualFailureAndSuccessFacts) {
  for (const auto &removed : {"*out=0;", "*out=p;", "free(p);return 0;"}) {
    auto bad = OutputConstructor;
    const auto at = bad.find(removed);
    ASSERT_NE(at, std::string::npos);
    bad.replace(at, std::string(removed).size(),
                std::string(removed).starts_with("free") ? "return 0;" : "");
    EXPECT_FALSE(workflowContract(bad, "build").complete()) << removed;
  }
}

TEST(RecursiveContracts, OutputConstructionCannotLoseOrDuplicateAnOwnedTree) {
  for (const auto &cleanup : {"free(p);", "destroy(p); destroy(p);"}) {
    EXPECT_FALSE(workflowContract(OutputConstructor + R"c(
      int client(void) {
        unsigned char data[]={1,2,3}; struct node *p=0;
        if(build(data,3,&p)){)c" + cleanup +
                                      R"c(} return 0;
      }
    )c",
                                  "client")
                     .complete())
        << cleanup;
  }
}

TEST(RecursiveContracts, OutputConstructionDoesNotAcquireThePreviousSlot) {
  EXPECT_FALSE(workflowContract(OutputConstructor + R"c(
    int client(void) {
      unsigned char data[]={1,2,3}; struct node *p=0;
      if(!build(data,3,&p))return 0;
      if(build(data,3,&p))destroy(p); return 0;
    }
  )c",
                                "client")
                   .complete());
  EXPECT_FALSE(workflowContract(OutputConstructor + R"c(
    int client(void) {
      unsigned char data[]={1,2,3}; struct node *p=0;
      int made=build(data,3,&p); free(p);
      if(made)destroy(p); return 0;
    }
  )c",
                                "client")
                   .complete());
  auto failed = OutputConstructor;
  failed.replace(failed.find("*out=p; return 1;"),
                 std::string("*out=p; return 1;").size(), "*out=p; return 0;");
  EXPECT_FALSE(workflowContract(failed, "build").complete());
}

TEST(RecursiveContracts, OutputConstructionSupportsMutualProgress) {
  auto source = OutputConstructor;
  const auto *const declaration =
      "int forward(const unsigned char *, size_t, struct node **);\n";
  source.insert(source.find("int build("), declaration);
  source.replace(source.find("!build(data+1,n-1,&p->next)"),
                 std::string("!build(data+1,n-1,&p->next)").size(),
                 "!forward(data+1,n-1,&p->next)");
  source += R"c(
    int forward(const unsigned char *data, size_t n, struct node **out) {
      if(build(data,n,out))return 1; return 0;
    }
  )c";
  EXPECT_TRUE(workflowContract(source, "build").complete());
  EXPECT_TRUE(workflowContract(source, "forward").complete());
}

TEST(RecursiveContracts, AddressedMemberRetainsItsOwnSubobjectBounds) {
  const auto source = [](const std::string &index) {
    return "struct pair { unsigned first, second, third; }; "
           "static void set(unsigned *p) { p[" +
           index +
           "] = 7; } "
           "int client(void) { struct pair p={0}; set(&p.second); return 0; }";
  };
  EXPECT_TRUE(workflowContract(source("0"), "client").complete());
  EXPECT_FALSE(workflowContract(source("1"), "client").complete());
  EXPECT_FALSE(workflowContract(source("-1"), "client").complete());
}

static const std::string ReaderConstructor = R"c(
  void *calloc(size_t, size_t);
  struct reader { size_t depth; const unsigned char *data; size_t remaining; };
  struct node { unsigned char value; struct node *next; };
  static void destroy(struct node *p) { if(p){destroy(p->next);free(p);} }
  struct node *build(struct reader r) {
    if(!r.remaining)return 0;
    struct node *p=calloc(1,sizeof *p); if(!p)return 0;
    p->value=r.data[0];
    if(r.remaining>1){
      r.data++; r.remaining--;
      p->next=build(r);
      if(!p->next){free(p);return 0;}
    }
    return p;
  }
)c";

TEST(RecursiveContracts, CopiedReaderConstructionUsesTheCompleteInput) {
  EXPECT_TRUE(workflowContract(ReaderConstructor, "build").complete());
  for (const auto &count : {"3", "4"}) {
    const auto source = ReaderConstructor + R"c(
      int client(void) {
        unsigned char data[]={1,2,3}; struct reader r={42,data,)c" +
                        count + R"c(};
        struct node *p=build(r); destroy(p); return r.remaining!=3;
      }
    )c";
    EXPECT_EQ(workflowContract(source, "client").complete(),
              std::string_view(count) == "3");
  }
}

TEST(RecursiveContracts, CopiedReaderConstructionChecksProgressAndCleanup) {
  for (const auto &[before, after] :
       {std::pair{"r.remaining--;", ""}, std::pair{"r.data++;", "r.data+=2;"},
        std::pair{"free(p);return 0;", "return 0;"},
        std::pair{"free(p);return 0;", "free(p);free(p);return 0;"}}) {
    auto source = ReaderConstructor;
    source.replace(source.find(before), std::string(before).size(), after);
    EXPECT_FALSE(workflowContract(source, "build").complete()) << before;
  }
}

static std::string mutableReaderConstructor() {
  auto source = ReaderConstructor;
  source.replace(source.find("build(struct reader r)"),
                 std::string("build(struct reader r)").size(),
                 "build(struct reader *r)");
  for (const auto *field : {"data", "remaining"}) {
    const auto before = std::string("r.") + field;
    for (auto at = source.find(before); at != std::string::npos;
         at = source.find(before))
      source.replace(at, before.size(), std::string("r->") + field);
  }
  return source;
}

TEST(RecursiveContracts, MutableReaderConstructionAccountsForChangedInputs) {
  const auto source = mutableReaderConstructor();
  EXPECT_TRUE(workflowContract(source, "build").complete());
  EXPECT_TRUE(workflowContract(source + R"c(
    int client(void) {
      unsigned char data[]={1,2,3}; struct reader r={42,data,3};
      struct node *p=build(&r); destroy(p); return 0;
    }
  )c",
                               "client")
                  .complete());
  EXPECT_FALSE(workflowContract(source + R"c(
    int client(void) {
      unsigned char data[]={1,2,3}; struct reader r={42,data,4};
      struct node *p=build(&r); destroy(p); return 0;
    }
  )c",
                                "client")
                   .complete());
  EXPECT_FALSE(workflowContract(source + R"c(
    int client(void) {
      struct reader r={42,0,3}; r.data=(const unsigned char *)&r;
      struct node *p=build(&r); destroy(p); return 0;
    }
  )c",
                                "client")
                   .complete());
}

TEST(RecursiveContracts, MutableReaderConstructionChecksEveryRecursiveExit) {
  for (const auto &[before, after] :
       {std::pair{"r->remaining--;", ""},
        std::pair{"r->data++;", "r->data+=2;"},
        std::pair{"free(p);return 0;", "return 0;"},
        std::pair{"free(p);return 0;", "free(p);free(p);return 0;"}}) {
    auto source = mutableReaderConstructor();
    source.replace(source.find(before), std::string(before).size(), after);
    EXPECT_FALSE(workflowContract(source, "build").complete()) << before;
  }
}

TEST(RecursiveContracts, ReaderConstructionSupportsMutualForwarding) {
  for (const bool pointer : {false, true}) {
    auto source = pointer ? mutableReaderConstructor() : ReaderConstructor;
    const std::string parameter =
        pointer ? "struct reader *r" : "struct reader r";
    source.insert(source.find("struct node *build("),
                  "struct node *forward(" + parameter + ");\n");
    source.replace(source.find("p->next=build(r)"),
                   std::string("p->next=build(r)").size(),
                   "p->next=forward(r)");
    source += "struct node *forward(" + parameter + ") { return build(r); }";
    EXPECT_TRUE(workflowContract(source, "build").complete()) << pointer;
    EXPECT_TRUE(workflowContract(source, "forward").complete()) << pointer;
  }
}

TEST(RecursiveContracts, ReaderWritesCannotFrameBorrowedOrReplacedForests) {
  const auto source = mutableReaderConstructor();
  EXPECT_FALSE(workflowContract(source + R"c(
    int client(void) {
      unsigned char data[]={1,2,3}; struct reader r={42,data,3};
      struct node *p=build(&r); r.data=0;
      struct node *q=build(&r); destroy(p); destroy(q); return 0;
    }
  )c",
                                "client")
                   .complete());
  EXPECT_FALSE(workflowContract(source + R"c(
    int client(void) {
      unsigned char data[]={1,2,3}; struct reader r={42,data,3};
      struct node *p=build(&r); if(!p)return 0;
      struct reader *alias=(struct reader *)p;
      alias->data=0; destroy(p); return 0;
    }
  )c",
                                "client")
                   .complete());
}

TEST(RecursiveContracts, NumericInputModelsOnlyMemoryAndEndProvenance) {
  for (const auto &[type, name] :
       {std::pair{"double", "strtod"}, std::pair{"float", "strtof"},
        std::pair{"long double", "strtold"}}) {
    const auto declaration =
        std::string(type) + " " + name + "(const char *,char **);\n";
    EXPECT_TRUE(workflowContract(declaration + R"c(
      int client(void) {
        char data[]="12.5x"; char *end=0;
    )c" + "(void)" + name + R"c((data,&end);return *end==0;
      }
    )c",
                                 "client")
                    .complete())
        << name;
    EXPECT_FALSE(workflowContract(declaration + R"c(
      int client(void) {
        char data[]="nan"; return (int))c" +
                                      name + R"c((data,0);
      }
    )c",
                                  "client")
                     .complete())
        << name;
  }
}

TEST(RecursiveContracts, NumericInputNeedsInitializationAndWritableEndSlot) {
  const std::string declaration = "double strtod(const char *, char **);\n";
  for (const auto &source : {"int client(void){char a[3];a[0]='1';a[2]=0;"
                             "(void)strtod(a,0);return 0;}",
                             "int client(void){char a[2]={'1','2'};"
                             "(void)strtod(a,0);return 0;}",
                             "int client(void){char a[]=\"12\";char *const e=0;"
                             "(void)strtod(a,(char **)&e);return 0;}",
                             "int client(void){char a[]=\"12\";char *e=0;"
                             "(void)strtod(a,&e);return e[3];}"})
    EXPECT_FALSE(workflowContract(declaration + source, "client").complete());
  EXPECT_FALSE(workflowContract(R"c(
    double strtod(const char *text, char **end) {
      *end=(char *)text+20;return 0;
    }
    int client(void) {char a[]="12";char *end=0;
      (void)strtod(a,&end);return *end;}
  )c",
                                "client")
                   .complete());
}

TEST(RecursiveContracts, CharacterPointerSlotsKeepMutabilityAndProvenance) {
  const std::string declaration = "double strtod(const char *, char **);\n";
  EXPECT_TRUE(workflowContract(declaration + R"c(
    int client(void) {
      unsigned char data[]="12.5x"; unsigned char *end=0;
      (void)strtod((const char *)data,(char **)&end); return *end==0;
    }
  )c",
                               "client")
                  .complete());
  for (const auto &source :
       {"int client(void){unsigned char a[]=\"12\";"
        "unsigned char *const e=0;"
        "(void)strtod((const char *)a,(char **)&e);return 0;}",
        "int client(void){unsigned char a[]=\"12\";unsigned char *e=0;"
        "(void)strtod((const char *)a,(char **)&e);return e[3];}",
        "int client(void){unsigned char *a=malloc(2);if(!a)return 0;"
        "a[0]='1';a[1]=0;unsigned char *e=0;"
        "(void)strtod((const char *)a,(char **)&e);free(a);return *e;}"})
    EXPECT_FALSE(workflowContract(declaration + source, "client").complete());
  EXPECT_FALSE(workflowContract(R"c(
    void set(float **out,float *value) { *out=value; }
    int client(void) {float a=0;int *p=0;set((float **)&p,&a);return *p;}
  )c",
                                "client")
                   .complete());
}

TEST(RecursiveContracts, NumericEndPointerDoesNotOutliveItsInput) {
  EXPECT_FALSE(workflowContract(R"c(
    double strtod(const char *, char **);
    int client(void) {
      char *data=malloc(2);if(!data)return 0;
      data[0]='1';data[1]=0;char *end=0;
      (void)strtod(data,&end);free(data);return *end;
    }
  )c",
                                "client")
                   .complete());
}

TEST(RecursiveContracts, FiniteFloatingConversionsUseTargetBounds) {
  for (const auto &source :
       {"int f(double x) { if(x >= -2147483648.0 && x <= 2147483647.0) "
        "return (int)x; return 0; }",
        "int f(double x) { if(-129.0 < x) { if(128.0 > x) return "
        "(signed char)x; } return 0; }",
        "unsigned f(double x) { if(x > -1.0 && x < 4294967296.0) "
        "return (unsigned)x; return 0; }",
        "long long f(double x) { if(x >= -9223372036854775808.0 && "
        "x < 9223372036854775808.0) return (long long)x; return 0; }",
        "int f(double x) { if(x == 12.5) return (int)x; return 0; }",
        "int f(void) { return (int)12.75; }"})
    EXPECT_TRUE(workflowContract(source, "f").complete()) << source;
}

TEST(RecursiveContracts, FloatingConversionsRejectUnorderedAndChangedValues) {
  for (const auto &source :
       {"int f(double x) { return (int)x; }",
        "int f(double x) { if(x < -2147483648.0 || x > 2147483647.0) "
        "return 0; return (int)x; }",
        "int f(double x) { if(x >= -2147483648.0 || x <= 2147483647.0) "
        "return (int)x; return 0; }",
        "long long f(double x) { if(x >= -9223372036854775808.0 && "
        "x <= 9223372036854775808.0) return (long long)x; return 0; }",
        "int f(double x) { if(x >= 0 && x <= 2147483647.0) { x += 1; "
        "return (int)x; } return 0; }",
        "int f(double x) { if(x >= 0 && x <= 2147483647.0) return "
        "(int)(float)x; return 0; }",
        "int f(double x) { double *p = &x; if(x >= 0 && x <= 127) { "
        "*p = 1000; return (signed char)x; } return 0; }",
        "#pragma STDC FENV_ACCESS ON\n"
        "int f(double x) { if(x >= 0 && x <= 127) return (int)x; "
        "return 0; }",
        "int f(void) { return (int)__builtin_inf(); }",
        "int f(void) { return (int)__builtin_nan(\"\"); }",
        "int f(double x) { goto inside; if(x >= 0 && x <= 127) { "
        "inside: return (signed char)x; } return 0; }",
        "int f(double x, int k) { switch(k) { if(x >= 0 && x <= 127) { "
        "case 1: return (signed char)x; } } return 0; }"})
    EXPECT_FALSE(workflowContract(source, "f").complete()) << source;
}

TEST(RecursiveContracts, LaterReassignmentPreservesAnEarlierEntryRelease) {
  const auto contract = workflowContract(R"c(
    void destroy(void *p) { free(p); p=0; }
    int client(void) {
      void *p=malloc(8); if(!p)return 0; destroy(p); return 0;
    }
  )c",
                                         "client");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(contract.requirements.empty());
  const auto helper =
      workflowContract("void destroy(void *p) { free(p); p=0; }", "destroy");
  EXPECT_TRUE(helper.complete());
  EXPECT_TRUE(std::ranges::any_of(helper.establishes, [](const auto &post) {
    return post.kind == core::CheckedRequirementKind::AllocationConsumed;
  }));
}

TEST(RecursiveContracts, ReplacementCannotConsumeOrValidateTheOldInput) {
  EXPECT_FALSE(workflowContract(R"c(
    void destroy(void *p) { p=malloc(8); free(p); }
    int client(void) {
      void *p=malloc(8); if(!p)return 0; destroy(p); return 0;
    }
  )c",
                                "client")
                   .complete());
  EXPECT_FALSE(workflowContract(R"c(
    void destroy(void *p) { free(p); p=0; }
    int client(void) {
      char *p=malloc(8); if(!p)return 0; destroy(p); return *p;
    }
  )c",
                                "client")
                   .complete());
}

static const std::string MutualTraversal = R"c(
  struct tree { unsigned value; struct tree *left, *right; };
  unsigned odd(const struct tree *p);
  unsigned even(const struct tree *p) {
    if (!p) return 0;
    return p->value + odd(p->left) + odd(p->right);
  }
  unsigned odd(const struct tree *p) {
    if (!p) return 0;
    return p->value + even(p->left) + even(p->right);
  }
  int client(void) {
    struct tree b = {2, 0, 0}, a = {1, &b, 0};
    return even(&a) != 3;
  }
)c";

TEST(RecursiveContracts, TraversalPreservesTheBorrowedInputFootprint) {
  for (const auto *name : {"even", "odd", "client"}) {
    const auto contract = workflowContract(MutualTraversal, name);
    EXPECT_TRUE(contract.complete()) << name;
    if (std::string_view(name) == "client") {
      EXPECT_TRUE(contract.requirements.empty());
    } else {
      EXPECT_TRUE(
          std::ranges::any_of(contract.establishes, [](const auto &post) {
            return post.kind ==
                       core::CheckedRequirementKind::ContainerPreserved &&
                   post.path == core::SummaryPath::param(0) && !post.on;
          }));
    }
  }
}

TEST(RecursiveContracts, TraversalForwardingRequiresAProgressingCycle) {
  auto source = MutualTraversal;
  source.replace(source.find("even(p->left)"),
                 std::string("even(p->left)").size(), "even(p)");
  EXPECT_TRUE(workflowContract(source, "even").complete());
  source.replace(source.find("odd(p->left)"),
                 std::string("odd(p->left)").size(), "odd(p)");
  for (const auto *name : {"even", "odd"})
    EXPECT_FALSE(workflowContract(source, name).complete());
}

TEST(RecursiveContracts, TraversalHypothesesCannotHideLocalErrors) {
  for (const auto *replacement :
       {"even(p->left + 1)", "even(p->left) + *(unsigned *)0"}) {
    auto source = MutualTraversal;
    source.replace(source.find("even(p->left)"),
                   std::string("even(p->left)").size(), replacement);
    EXPECT_FALSE(workflowContract(source, "even").complete()) << replacement;
  }
  auto source = MutualTraversal;
  source.replace(source.find("struct tree b = {2, 0, 0}"),
                 std::string("struct tree b = {2, 0, 0}").size(),
                 "struct tree b");
  EXPECT_FALSE(workflowContract(source, "client").complete());
}

TEST(RecursiveContracts, DirectTraversalUsesTheSameGroupProof) {
  const auto contract = workflowContract(R"c(
    struct tree { unsigned value; struct tree *left, *right; };
    unsigned sum(const struct tree *p) {
      if (!p) return 0;
      return p->value + sum(p->left) + sum(p->right);
    }
  )c",
                                         "sum");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(std::ranges::any_of(contract.establishes, [](const auto &post) {
    return post.kind == core::CheckedRequirementKind::ContainerPreserved;
  }));
}

TEST(RecursiveContracts, PrivateProofMembersCannotPublishNestedContexts) {
  AnalysisOptions options;
  options.checkedFunctions.insert("even");
  const auto unit = test::analyze(MutualTraversal, options);
  const auto *function = unit.function("even");
  ASSERT_NE(function, nullptr);
  auto &store = unit.analyzer->summaries();
  const auto callbacks = store.specialized.size();
  const auto memories = store.memorySpecialized.size();
  store.activeRecursiveContracts = {.members = {function->getCanonicalDecl()},
                                    .releases = false};
  EXPECT_FALSE(store.specialize(*function, {}, options, nullptr));
  EXPECT_FALSE(
      store.specializeMemory(callableSymbol(*function), {}, options, nullptr));
  EXPECT_EQ(store.specialized.size(), callbacks);
  EXPECT_EQ(store.memorySpecialized.size(), memories);
  store.activeRecursiveContracts = {};
}

static const std::string RecursiveConstruction = R"c(
  void *calloc(size_t, size_t);
  struct node { unsigned char value; struct node *next; };
  static void destroy(struct node *p) {
    if (p) { destroy(p->next); free(p); }
  }
  struct node *build(const unsigned char *data, size_t n) {
    if (!n) return 0;
    struct node *p = calloc(1, sizeof *p);
    if (!p) return 0;
    p->value = data[0];
    if (n > 1) {
      p->next = build(data + 1, n - 1);
      if (!p->next) { free(p); return 0; }
    }
    return p;
  }
  int client(void) {
    const unsigned char data[] = {1, 2, 3};
    struct node *p = build(data, 3);
    destroy(p);
    return 0;
  }
)c";

TEST(RecursiveContracts, ConstructionProvesFreshOutputsAndFailureCleanup) {
  const auto contract = workflowContract(RecursiveConstruction, "build");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(std::ranges::any_of(contract.establishes, [](const auto &post) {
    return post.kind == core::CheckedRequirementKind::ContainerFresh &&
           post.path == core::SummaryPath::result();
  }));
  const auto client = workflowContract(RecursiveConstruction, "client");
  EXPECT_TRUE(client.complete());
  EXPECT_TRUE(client.requirements.empty());
}

TEST(RecursiveContracts, ConstructionNeedsTheWholeInitializedInput) {
  auto source = RecursiveConstruction;
  source.replace(source.find("build(data, 3)"),
                 std::string("build(data, 3)").size(), "build(data, 4)");
  EXPECT_FALSE(workflowContract(source, "client").complete());
  source = RecursiveConstruction;
  source.replace(source.find("const unsigned char data[] = {1, 2, 3};"),
                 std::string("const unsigned char data[] = {1, 2, 3};").size(),
                 "unsigned char data[3]; data[0] = 1;");
  EXPECT_FALSE(workflowContract(source, "client").complete());
}

TEST(RecursiveContracts, ConstructionRejectsLeaksAndInvalidReturnedForests) {
  for (const auto *replacement : {"return 0;", "free(p); free(p); return 0;"}) {
    auto source = RecursiveConstruction;
    source.replace(source.find("free(p); return 0;"),
                   std::string("free(p); return 0;").size(), replacement);
    EXPECT_FALSE(workflowContract(source, "build").complete()) << replacement;
  }
  auto source = RecursiveConstruction;
  source.replace(source.find("return p;"), std::string("return p;").size(),
                 "free(p); return (struct node *)data;");
  EXPECT_FALSE(workflowContract(source, "build").complete());
  source = RecursiveConstruction;
  source.replace(source.find("calloc(1, sizeof *p)"),
                 std::string("calloc(1, sizeof *p)").size(),
                 "malloc(sizeof *p)");
  EXPECT_FALSE(workflowContract(source, "build").complete());
  source = RecursiveConstruction;
  source.replace(source.find("if (!n) return 0;"),
                 std::string("if (!n) return 0;").size(), "(void)n;");
  EXPECT_FALSE(workflowContract(source, "build").complete());
}

TEST(RecursiveContracts, ConstructionProgressUsesTheImmutableEntryCount) {
  for (const auto *change :
       {"if (n < 16) ++n;", "size_t *alias = &n; if (*alias < 16) ++*alias;"}) {
    auto source = RecursiveConstruction;
    source.insert(source.find("if (n > 1)"), change);
    EXPECT_FALSE(workflowContract(source, "build").complete()) << change;
  }
  auto source = RecursiveConstruction;
  source.replace(source.find("build(data + 1, n - 1)"),
                 std::string("build(data + 1, n - 1)").size(),
                 "build(data, n)");
  EXPECT_FALSE(workflowContract(source, "build").complete());
}

TEST(RecursiveContracts, MutualConstructionPublishesTheWholeGroup) {
  auto source = RecursiveConstruction;
  source.insert(source.find("struct node *build("), R"c(
    struct node *build(const unsigned char *, size_t);
    struct node *forward(const unsigned char *data, size_t n) {
      return build(data, n);
    }
  )c");
  source.replace(source.find("build(data + 1, n - 1)"),
                 std::string("build(data + 1, n - 1)").size(),
                 "forward(data + 1, n - 1)");
  for (const auto *name : {"build", "forward", "client"})
    EXPECT_TRUE(workflowContract(source, name).complete()) << name;
  source.replace(source.find("forward(data + 1, n - 1)"),
                 std::string("forward(data + 1, n - 1)").size(),
                 "forward(data, n)");
  for (const auto *name : {"build", "forward", "client"})
    EXPECT_FALSE(workflowContract(source, name).complete()) << name;
}

static const std::string TreeConstruction = R"c(
  void *calloc(size_t, size_t);
  struct node { unsigned char value; struct node *left, *right; };
  static void destroy(struct node *p) {
    if (p) { destroy(p->left); destroy(p->right); free(p); }
  }
  static unsigned char first(const unsigned char *data) { return *data; }
  struct node *build(const unsigned char *data, size_t n) {
    if (!n) return 0;
    struct node *p = calloc(1, sizeof *p);
    if (!p) return 0;
    p->value = first(data);
    if (n > 1) {
      p->left = build(data + 1, n - 1);
      if (!p->left) { free(p); return 0; }
      p->right = build(data + 1, n - 1);
      if (!p->right) { destroy(p->left); free(p); return 0; }
    }
    return p;
  }
  int client(void) {
    const unsigned char data[] = {1, 2, 3};
    struct node *p = build(data, 3); destroy(p); return 0;
  }
)c";

TEST(RecursiveContracts, ConstructionComposesVerifiedPartialCleanup) {
  for (const auto *name : {"build", "client"})
    EXPECT_TRUE(workflowContract(TreeConstruction, name).complete()) << name;
  for (const auto *replacement : {"", "destroy(p->left); destroy(p->left);"}) {
    auto source = TreeConstruction;
    source.replace(source.find("destroy(p->left); free(p); return 0;"),
                   std::string("destroy(p->left);").size(), replacement);
    EXPECT_FALSE(workflowContract(source, "build").complete()) << replacement;
  }
}

TEST(RecursiveContracts, ConstructionCannotDuplicateAnAttachedSubtree) {
  auto source = TreeConstruction;
  source.replace(source.find("p->right = build(data + 1, n - 1)"),
                 std::string("p->right = build(data + 1, n - 1)").size(),
                 "p->right = p->left");
  EXPECT_FALSE(workflowContract(source, "build").complete());
  EXPECT_FALSE(workflowContract(source, "client").complete());
}

static const std::string MutualCleanup = R"c(
  void *calloc(size_t, size_t);
  struct tree { unsigned value; struct tree *left, *right; };
  static void odd(struct tree *p);
  static void even(struct tree *p) {
    if(!p)return; odd(p->left); odd(p->right); free(p);
  }
  static void odd(struct tree *p) {
    if(!p)return; even(p->left); even(p->right); free(p);
  }
  int client(void) {
    struct tree *a=calloc(1,sizeof *a); if(!a)return 0;
    struct tree *b=calloc(1,sizeof *b); if(!b){free(a);return 0;}
    a->right=b; even(a); return 0;
  }
)c";

TEST(RecursiveContracts, MutualCleanupPublishesOnlyVerifiedGroupOutputs) {
  for (const auto *name : {"even", "odd", "client"}) {
    const auto contract = workflowContract(MutualCleanup, name);
    EXPECT_TRUE(contract.complete()) << name;
    if (std::string_view(name) == "client")
      EXPECT_TRUE(contract.requirements.empty());
    else
      EXPECT_TRUE(
          std::ranges::any_of(contract.establishes, [](const auto &post) {
            return post.kind == core::CheckedRequirementKind::ContainerConsumed;
          }));
  }
}

TEST(RecursiveContracts, FailedMemberCannotAuthorizeItsPeers) {
  auto source = MutualCleanup;
  const auto position = source.find("even(p->right);");
  ASSERT_NE(position, std::string::npos);
  source.erase(position, std::string("even(p->right);").size());
  EXPECT_FALSE(workflowContract(source, "client").complete());
  source = MutualCleanup;
  source.replace(source.find("even(p->left)"),
                 std::string("even(p->left)").size(), "even(p)");
  EXPECT_FALSE(workflowContract(source, "odd").complete());
}

TEST(RecursiveContracts, RecursiveGlobalEffectsCannotDisappear) {
  auto source = "static void *extra;" + MutualCleanup;
  source.replace(source.find("even(p->right);"),
                 std::string("even(p->right);").size(),
                 "even(p->right); free(extra);");
  EXPECT_FALSE(workflowContract(source, "odd").complete());
}

TEST(RecursiveContracts, SharedAndCyclicChildrenCannotBecomeOwnedForests) {
  for (const auto *attachment :
       {"a->right=b; a->left=b;", "a->right=a; free(b);",
        "a->right=b; b->left=a;"}) {
    auto source = MutualCleanup;
    source.replace(source.find("a->right=b;"),
                   std::string("a->right=b;").size(), attachment);
    EXPECT_FALSE(workflowContract(source, "client").complete()) << attachment;
  }
}

TEST(RecursiveContracts, CallbackAllocatorDemandIsExplicitAndForwardable) {
  const std::string source = R"c(
    static void *make(void *(*allocate)(size_t), size_t n) {
      if (!n) return 0;
      unsigned char *p=allocate(n); if(p)p[n-1]=7; return p;
    }
    void *forward(void *(*allocate)(size_t), size_t n) {
      return make(allocate,n);
    }
  )c";
  for (const auto *name : {"make", "forward"}) {
    const auto contract = workflowContract(source, name);
    EXPECT_TRUE(contract.complete()) << name;
    EXPECT_TRUE(std::ranges::any_of(contract.requirements, [](const auto &pre) {
      return pre.kind == core::CheckedRequirementKind::CallbackAllocate &&
             pre.path == core::SummaryPath::param(0);
    }));
    EXPECT_FALSE(contract.obligations.trusted());
  }
}

TEST(RecursiveContracts, CallbackReleaseDemandConsumesTheIncomingAllocation) {
  const std::string source = R"c(
    static void destroy(void (*release)(void *), void *p) { release(p); }
    static void forward(void (*release)(void *), void *p) { destroy(release,p); }
    int client(void) { void *p=malloc(8); forward(free,p); return 0; }
  )c";
  for (const auto *name : {"destroy", "forward", "client"}) {
    const auto contract = workflowContract(source, name);
    EXPECT_TRUE(contract.complete()) << name;
    if (std::string_view(name) == "client")
      EXPECT_TRUE(contract.requirements.empty());
    else
      EXPECT_TRUE(
          std::ranges::any_of(contract.requirements, [](const auto &pre) {
            return pre.kind == core::CheckedRequirementKind::CallbackRelease;
          }));
  }
}

TEST(RecursiveContracts, CallbackBadBindingsRemainUnproved) {
  const std::string helper = R"c(
    static void *make(void *(*allocate)(size_t), size_t n) {
      if (!n) return 0;
      unsigned char *p=allocate(n); if(p)p[n-1]=7; return p;
    }
    static void *short_one(size_t n) { return malloc(1); }
  )c";
  for (const auto *body :
       {"void *p=make(short_one,8); free(p);", "void *p=make(0,8); free(p);",
        "void *p=make(flag?malloc:short_one,8); free(p);",
        "void *p=make(flag?malloc:0,8); free(p);"}) {
    const auto source = helper + "int client(int flag){" + body + "return 0;}";
    EXPECT_FALSE(workflowContract(source, "client").complete()) << body;
  }
}

TEST(RecursiveContracts, CallbackCastAndReplacementCannotRecoverEntryBehavior) {
  EXPECT_FALSE(workflowContract(R"c(
    void *client(void *(*f)(unsigned), size_t n) {
      unsigned char *p=((void *(*)(size_t))f)(n);
      if(n&&p)p[n-1]=7; return p;
    }
  )c",
                                "client")
                   .complete());
  EXPECT_FALSE(workflowContract(R"c(
    static void *bad(size_t n) { return malloc(1); }
    void *client(void *(*f)(size_t), size_t n) {
      f=bad; unsigned char *p=f(n); if(n&&p)p[n-1]=7; return p;
    }
  )c",
                                "client")
                   .complete());
}

TEST(RecursiveContracts, ParameterSlotIsNotInitializedCallerMemory) {
  const auto contract = workflowContract("void reset(char *p){p=0;}", "reset");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(contract.establishes.empty());
}

TEST(RecursiveContracts, RecursiveForwardingRequiresTheExactNode) {
  for (const auto *argument : {"p+1", "p-1", "(struct tree *)((char *)p+1)"}) {
    const std::string source = R"c(
      void *calloc(size_t, size_t);
      struct tree { struct tree *left, *right; };
      static void odd(struct tree *p);
      static void even(struct tree *p) {
        if(!p)return;
        odd(p->left); odd(p->right); free(p);
      }
      static void odd(struct tree *p) { even()c" +
                               std::string(argument) + R"c(); }
      int client(void) {
        struct tree *p=calloc(1,sizeof *p); if(!p)return 0;
        odd(p); return 0;
      }
    )c";
    EXPECT_FALSE(workflowContract(source, "client").complete()) << argument;
  }
}

TEST(RecursiveContracts, ForwardingNeedsProgressOnEveryCycle) {
  const std::string source = R"c(
    struct tree { struct tree *left, *right; };
    static void walk(struct tree *p);
    static void forward(struct tree *p) { walk(p); }
    static void walk(struct tree *p) {
      if(!p)return; forward(p->left); forward(p->right); free(p);
    }
  )c";
  EXPECT_TRUE(workflowContract(source, "forward").complete());
  EXPECT_TRUE(workflowContract(source, "walk").complete());
  auto bad = source;
  bad.replace(bad.find("forward(p->left)"),
              std::string("forward(p->left)").size(), "forward(p)");
  EXPECT_FALSE(workflowContract(bad, "forward").complete());
  EXPECT_FALSE(workflowContract(bad, "walk").complete());
}

TEST(RecursiveContracts, ExcludedNullCallbackDoesNotRequireBehavior) {
  const auto contract = workflowContract(R"c(
    static void *make(void *(*allocate)(size_t), size_t n) {
      if(!n)return 0;
      unsigned char *p=allocate(n); if(p)p[n-1]=7; return p;
    }
    int client(void) { void *p=make(0,0); free(p); return 0; }
  )c",
                                         "client");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(contract.requirements.empty());
}

TEST(RecursiveContracts, VerifiedAllocationWrapperCarriesItsActualTrust) {
  const auto contract = workflowContract(R"c(
    static void *allocate(size_t n) { return malloc(n); }
    static void *make(void *(*callback)(size_t), size_t n) {
      if(!n)return 0;
      unsigned char *p=callback(n); if(p)p[n-1]=7; return p;
    }
    int client(void) { void *p=make(allocate,9); free(p); return 0; }
  )c",
                                         "client");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(contract.requirements.empty());
  EXPECT_TRUE(contract.obligations.trusted());
}

TEST(RecursiveContracts,
     HiddenCallbackEffectsRemainVisibleDuringSpecialization) {
  const auto contract = workflowContract(R"c(
    static void *saved;
    static void *allocate(size_t n) { saved=malloc(n); return saved; }
    static void *make(void *(*callback)(size_t), size_t n) {
      if(!n)return 0;
      unsigned char *p=callback(n); if(p)p[n-1]=7; return p;
    }
    int client(void) { void *p=make(allocate,9); free(p); return p ? *(unsigned char *)saved : 0; }
  )c",
                                         "client");
  EXPECT_FALSE(contract.complete());
}

TEST(RecursiveContracts, StateRolesIgnoreUnrelatedPointerAndCounterFields) {
  const auto contract = workflowContract(R"c(
    struct output {
      int *unrelated; size_t generation, capacity; unsigned char *bytes;
      size_t length, depth;
    };
    static int append(struct output *p, unsigned char value) {
      if(p->length==p->capacity) {
        if(p->capacity>10000)return 0;
        size_t capacity=p->capacity+16;
        unsigned char *bytes=realloc(p->bytes,capacity);
        if(!bytes)return 0;
        p->bytes=bytes; p->capacity=capacity;
      }
      p->bytes[p->length++]=value; return 1;
    }
    int client(size_t n) {
      if(n>1000)return 0;
      struct output p={0};
      for(size_t i=0;i<n;++i)if(!append(&p,7))break;
      free(p.bytes); return 0;
    }
  )c",
                                         "client");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(contract.requirements.empty());
}

static const std::string ByteWriter = R"c(
  struct writer { unsigned flags; unsigned char *data; size_t used, capacity; };
  int emit(const unsigned char *input, size_t n, struct writer *w) {
    if(!n)return 1;
    if(w->used>=w->capacity)return 0;
    w->data[w->used]=input[0]; w->used++;
    if(!emit(input+1,n-1,w))return 0;
    return 1;
  }
)c";

TEST(RecursiveContracts, ByteWriterEstablishesItsActualInitializedPrefix) {
  const auto contract = workflowContract(ByteWriter, "emit");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(std::ranges::any_of(contract.establishes, [](const auto &post) {
    return post.kind == core::CheckedRequirementKind::Buffer &&
           post.path == core::SummaryPath::param(2).deref() && !post.on;
  }));
  for (const auto *capacity : {"2", "3"})
    EXPECT_TRUE(workflowContract(ByteWriter + R"c(
      int client(void) {
        unsigned char input[]={1,2,3}, output[3];
        struct writer w={7,output,0,)c" +
                                     capacity + R"c(};
        (void)emit(input,3,&w);
        return w.used ? w.data[w.used-1] : 0;
      }
    )c",
                                 "client")
                    .complete())
        << capacity;
}

TEST(RecursiveContracts, ByteWriterDoesNotInventSuccessLengthOrTermination) {
  auto early = ByteWriter;
  early.replace(early.find("if(w->used>=w->capacity)return 0;"),
                std::string("if(w->used>=w->capacity)return 0;").size(),
                "if(w->used>=w->capacity)return 1;");
  EXPECT_TRUE(workflowContract(early, "emit").complete());
  EXPECT_FALSE(workflowContract(early + R"c(
    int client(void) {
      unsigned char input[]={1,2,3}, output[3];
      struct writer w={7,output,0,2};
      return emit(input,3,&w) ? output[2] : 0;
    }
  )c",
                                "client")
                   .complete());
  EXPECT_FALSE(workflowContract(ByteWriter + R"c(
    size_t strlen(const char*);
    int client(void) {
      unsigned char input[]={1,2,3}, output[3];
      struct writer w={7,output,0,3};
      (void)emit(input,3,&w); return (int)strlen((char*)w.data);
    }
  )c",
                                "client")
                   .complete());
}

TEST(RecursiveContracts, ByteWriterChecksEveryEffectAndFailurePrefix) {
  for (const auto *mutation : {"w->used++;", "w->flags++;", "w->data=0;"}) {
    auto bad = ByteWriter;
    const std::string original = "if(w->used>=w->capacity)return 0;";
    bad.replace(bad.find(original), original.size(),
                "if(w->used>=w->capacity){" + std::string(mutation) +
                    "return 0;}");
    EXPECT_FALSE(workflowContract(bad, "emit").complete()) << mutation;
  }
}

TEST(RecursiveContracts, InitializedSpansRequireActualCallerEvidence) {
  const std::string helper = R"c(
    int pair(const unsigned char *first, const unsigned char *last) {
      const unsigned char *copy = first;
      if (last-copy < 2) return 0;
      return copy[0] + copy[1];
    }
  )c";
  const auto generic = workflowContract(helper, "pair");
  EXPECT_TRUE(generic.complete());
  EXPECT_TRUE(std::ranges::any_of(generic.requirements, [](const auto &pre) {
    return pre.kind == core::CheckedRequirementKind::InitializedSpan;
  }));
  for (const auto &[body, safe] : std::vector<std::pair<std::string, bool>>{
           {"const unsigned char a[3]={1,2,3};return pair(a+1,a+3);", true},
           {"const unsigned char a[1]={1};return pair(a,a);", true},
           {"const unsigned char a[2]={1,2},b[2]={3,4};return pair(a,b+2);",
            false},
           {"unsigned char a[2];a[0]=1;return pair(a,a+2);", false},
           {"const unsigned char a[2]={1,2};return pair(a,a+3);", false}}) {
    SCOPED_TRACE(body);
    auto source = helper;
    source += "int client(void){";
    source += body;
    source += '}';
    const auto client = workflowContract(source, "client");
    EXPECT_EQ(client.complete(), safe);
    EXPECT_TRUE(client.requirements.empty());
  }
}

TEST(RecursiveContracts, InitializedSpansDoNotSurviveUnknownMutation) {
  EXPECT_FALSE(workflowContract(R"c(
    void mutate(unsigned char *);
    int f(unsigned char *first, unsigned char *last) {
      if(last-first<2)return 0;
      mutate(first);
      return first[1];
    }
  )c",
                                "f")
                   .complete());
  EXPECT_FALSE(workflowContract(R"c(
    int f(const unsigned char *first, const unsigned char *last) {
      const unsigned char other[2]={1,2};
      const unsigned char *copy=first;
      copy=other;
      if(last-copy<2)return 0;
      return copy[1];
    }
  )c",
                                "f")
                   .complete());
}
TEST(RecursiveContracts,
     ReadOnlySpanEndpointsRemainRelatedWithSeparateOutputs) {
  const auto contract = workflowContract(R"c(
    int decode(const unsigned char *first, const unsigned char *last,
               unsigned char **out) {
      if(last-first<2)return 0;
      **out=first[1]; ++*out; return 1;
    }
  )c",
                                         "decode");
  EXPECT_TRUE(contract.complete());
  for (const auto &pre : contract.requirements)
    if (pre.kind == core::CheckedRequirementKind::Separated)
      EXPECT_FALSE(pre.path == core::SummaryPath::param(0) &&
                   pre.other == core::SummaryPath::param(1));
}

TEST(RecursiveContracts, NarrowReverseCountersProjectActualWriteBounds) {
  const std::string helper = R"c(
    void encode(unsigned char **out, unsigned code) {
      unsigned char length;
      if(code<128)length=1;
      else if(code<2048)length=2;
      else if(code<65536)length=3;
      else length=4;
      unsigned char i;
      for(i=(unsigned char)(length-1);i>0;i--) (*out)[i]=42;
      (*out)[0]=1;
      *out+=length;
    }
  )c";
  const auto generic = workflowContract(helper, "encode");
  EXPECT_TRUE(generic.complete());
  EXPECT_TRUE(std::ranges::any_of(generic.requirements, [](const auto &pre) {
    return pre.kind == core::CheckedRequirementKind::Writable &&
           pre.path == core::SummaryPath::param(0).deref() &&
           pre.begin == core::PathAffine::ofConstant(0) &&
           pre.end == core::PathAffine::ofConstant(4);
  }));
  for (const unsigned capacity : {3U, 4U}) {
    const auto client = workflowContract(
        helper + "int client(void){unsigned char out[" +
            std::to_string(capacity) +
            "];unsigned char *p=out;encode(&p,0x10000);return 0;}",
        "client");
    EXPECT_EQ(client.complete(), capacity == 4);
    EXPECT_TRUE(client.requirements.empty());
  }
}

TEST(RecursiveContracts, ReturnedCountMustFitTheActualInputSpan) {
  for (const unsigned produced : {4U, 5U}) {
    const std::string source = R"c(
      unsigned consume(const unsigned char *first,const unsigned char *last,int mode) {
        if(last-first<2)return 0;
        if(mode){if(last-first<4)return 0;return )c" +
                               std::to_string(produced) + R"c(;}
        return 2;
      }
      int probe(const unsigned char *first,const unsigned char *last,int mode) {
        if(last-first<1)return 0;
        unsigned n=consume(first,last,mode);
        if(!n)return 0;
        return first[n-1];
      }
    )c";
    const auto helper = workflowContract(source, "consume");
    EXPECT_TRUE(helper.complete());
    EXPECT_EQ(std::ranges::any_of(
                  helper.establishes,
                  [](const auto &post) {
                    return post.kind ==
                           core::CheckedRequirementKind::CountWithinSpan;
                  }),
              produced == 4);
    EXPECT_EQ(workflowContract(source, "probe").complete(), produced == 4);
  }
}

TEST(RecursiveContracts, JoinedCountsRetainEveryBranchBound) {
  for (const unsigned produced : {4U, 5U}) {
    const std::string source = R"c(
      unsigned char consume(const unsigned char *first,const unsigned char *last,int mode) {
        unsigned char count=0;
        if(last-first<2)return 0;
        if(mode){count=)c" + std::to_string(produced) +
                               R"c(;
          if(last-(first+2)<2)return 0;}
        else count=2;
        return count;
      }
      int probe(const unsigned char *first,const unsigned char *last,int mode) {
        if(last-first<1)return 0;
        unsigned char n=consume(first,last,mode);
        if(!n)return 0;
        return first[n-1];
      }
    )c";
    const auto helper = workflowContract(source, "consume");
    EXPECT_TRUE(helper.complete());
    EXPECT_EQ(std::ranges::any_of(
                  helper.establishes,
                  [](const auto &post) {
                    return post.kind ==
                           core::CheckedRequirementKind::CountWithinSpan;
                  }),
              produced == 4);
    EXPECT_EQ(workflowContract(source, "probe").complete(), produced == 4);
  }
}

TEST(RecursiveContracts, ConfinedCalleeCopiesRetainSourceOwnership) {
  const std::string source = R"c(
    void copy(char *p,char **out){*out=p;}
    int client(void){
      char *p=malloc(3);if(!p)return 0;p[0]=1;
      char *q=0;copy(p,&q);int n=p[0];free(p);return n;
    }
  )c";
  const auto result = workflowContract(source, "client");
  EXPECT_TRUE(result.complete());
  EXPECT_TRUE(result.requirements.empty());
  for (const auto &tail : {"free(p);return q[0];", "return q[0];"}) {
    const auto bad = workflowContract(
        "void copy(char *p,char **out){*out=p;}"
        "int client(void){char *p=malloc(3);if(!p)return 0;p[0]=1;"
        "char *q=0;copy(p,&q);" +
            std::string(tail) + "}",
        "client");
    EXPECT_FALSE(bad.complete());
  }
}

TEST(RecursiveContracts, NumericEndPointerPreservesOwnedInput) {
  const auto result = workflowContract(R"c(
    double strtod(const char *, char **);
    int client(void){
      char *p=malloc(3);if(!p)return 0;p[0]='1';p[1]='2';p[2]=0;
      char *end=0;(void)strtod(p,&end);
      long n=end-p;free(p);return (int)n;
    }
  )c",
                                       "client");
  EXPECT_TRUE(result.complete());
  EXPECT_TRUE(result.requirements.empty());
}

TEST(RecursiveContracts, ReverseWritesInitializeOnlyVisitedBytes) {
  for (const bool writeZero : {false, true}) {
    const auto result = workflowContract(
        "int fill(unsigned char *out,unsigned char length) {"
        "if(length<1||length>4)return 0; unsigned char i;"
        "for(i=(unsigned char)(length-1);i>0;i--)out[i]=42;" +
            std::string(writeZero ? "out[0]=1;" : "") +
            "return out[0];}"
            "int client(void){unsigned char bytes[4];return fill(bytes,4);}",
        "client");
    EXPECT_EQ(result.complete(), writeZero);
  }
}

TEST(RecursiveContracts, ConditionalArgumentsKeepConvertedRanges) {
  for (const unsigned count : {4U, 5U}) {
    const auto result = workflowContract(
        "void touch(unsigned char *p,unsigned n){if(n)p[n-1]=1;}"
        "int client(int mode){unsigned char out[4];touch(out,mode?" +
            std::to_string(count) + ":1);return 0;}",
        "client");
    EXPECT_EQ(result.complete(), count == 4);
  }
}

TEST(RecursiveContracts, InitializedAdvanceEndsAtActualOutputPointer) {
  for (const bool skip : {false, true}) {
    const std::string source =
        "void encode(unsigned char **out,unsigned code){"
        "unsigned char n=code>255?4:1,i;"
        "for(i=(unsigned char)(n-1);i>0;i--)" +
        std::string(skip ? "if(i!=2)" : "") +
        "(*out)[i]=42;(*out)[0]=1;*out+=n;}"
        "int client(int mode){unsigned char out[4],*p=out;"
        "encode(&p,mode?65536:1);if(p>out)return p[-1];return 0;}";
    const auto helper = workflowContract(source, "encode");
    EXPECT_TRUE(helper.complete());
    EXPECT_EQ(std::ranges::any_of(
                  helper.establishes,
                  [](const auto &post) {
                    return post.kind ==
                           core::CheckedRequirementKind::InitializedAdvance;
                  }),
              !skip);
    EXPECT_EQ(workflowContract(source, "client").complete(), !skip);
  }
}

TEST(RecursiveContracts, AdvanceInitializationIncludesEarlyReturns) {
  for (const bool badFailure : {false, true}) {
    const std::string source =
        "unsigned encode(unsigned char **out,unsigned code){"
        "if(!code){" +
        std::string(badFailure ? "*out+=1;" : "") +
        "return 0;}unsigned char n=code>255?4:1,i;"
        "for(i=(unsigned char)(n-1);i>0;i--)(*out)[i]=42;"
        "(*out)[0]=1;*out+=n;return n;}"
        "int client(int mode){unsigned char out[4],*p=out;"
        "encode(&p,mode?65536:0);if(p>out)return p[-1];return 0;}";
    const auto helper = workflowContract(source, "encode");
    EXPECT_TRUE(helper.complete());
    EXPECT_EQ(
        std::ranges::any_of(
            helper.establishes,
            [](const auto &post) {
              return post.kind ==
                         core::CheckedRequirementKind::InitializedAdvance &&
                     !post.on;
            }),
        !badFailure);
    EXPECT_EQ(workflowContract(source, "client").complete(), !badFailure);
  }
}

TEST(RecursiveContracts, CounterResetPreservesOnlyUnchangedRanges) {
  for (const bool changeCount : {false, true}) {
    const std::string source = R"c(
      void *memcpy(void *,const void *,size_t);
      double strtod(const char *,char **);
      struct reader {const unsigned char *data;size_t position,capacity,extra;};
      long scan(struct reader *r) {
        size_t i=0,count=0;int decimal=0;
        if(!r||!r->data)return 0;
        for(i=0;r->position+i<r->capacity;i++){
          switch((r->data+r->position)[i]){
          case '0':case '1':++count;break;
          case '.':++count;decimal=1;break;
          default:goto done;}}
      done:;
        char *out=malloc(count+1);if(!out)return 0;
        memcpy(out,r->data+r->position,count);out[count]=0;
    )c" + std::string(changeCount ? "count+=16;" : "") +
                               R"c(
        if(decimal)for(i=0;i<count;i++)if(out[i]=='.')out[i]='.';
        char *end=0;(void)strtod(out,&end);
        long result=end-out;free(out);return result;
      }
      int client(void){const unsigned char input[]="1.0";
        struct reader r={input,0,sizeof input,0};return (int)scan(&r);}
    )c";
    EXPECT_EQ(workflowContract(source, "client").complete(), !changeCount);
  }
}

TEST(RecursiveContracts, DisjointSymbolicStoresRetainOnlyUntouchedZeros) {
  for (const bool overwriteZero : {false, true}) {
    const std::string source = R"c(
      void *memcpy(void *,const void *,size_t);
      double strtod(const char *,char **);
      struct reader {const unsigned char *data;size_t position,capacity,extra;};
      void rewrite(struct reader *r) {
        size_t i=0,count=0;
        if(!r||!r->data)return;
        for(i=0;r->position+i<r->capacity;i++) {
          switch((r->data+r->position)[i]) {
          case '0':case '1':case '.':++count;break;
          default:goto done;
          }
        }
      done:;
        char *out=malloc(count+1);if(!out)return;
        memcpy(out,r->data+r->position,count);out[count]=0;
        for(i=0;i<count
    )c" + std::string(overwriteZero ? "+1" : "") +
                               R"c(;i++)out[i]='.';
        char *end=0;(void)strtod(out,&end);free(out);
      }
      int client(void){const unsigned char input[]="1.0";
        struct reader r={input,0,sizeof input,0};rewrite(&r);return 0;}
    )c";
    EXPECT_EQ(workflowContract(source, "client").complete(), !overwriteZero);
  }
}

TEST(RecursiveContracts, GuardedAdvancesNeedTheActualUnchangedResult) {
  for (const bool advanceFailure : {false, true})
    for (const std::string mutation : {"", "ok=1;", "p-=4;"}) {
      const std::string source =
          "unsigned encode(unsigned char **out,unsigned code){"
          "if(!code){" +
          std::string(advanceFailure ? "*out+=1;" : "") +
          "return 0;}unsigned char n=code>255?4:1,i;"
          "for(i=(unsigned char)(n-1);i>0;i--)(*out)[i]=42;"
          "(*out)[0]=1;*out+=n;return n;}"
          "int client(int mode){unsigned char out[4],*p=out;"
          "unsigned ok=encode(&p,mode?65536:0);" +
          mutation + "if(!ok)return 0;return p[-1];}";
      EXPECT_EQ(workflowContract(source, "client").complete(),
                mutation.empty());
    }
}

TEST(RecursiveContracts, ReturnedCountsBoundEachCursorAdvance) {
  for (const unsigned extra : {0U, 1U}) {
    const std::string source = R"c(
      unsigned char consume(const unsigned char *first,const unsigned char *last,int mode){
        unsigned char n=0;if(last-first<2)return 0;
        if(mode){n=4;if(last-(first+2)<2)return 0;}else n=2;
        return n;
      }
      int client(int mode){const unsigned char input[8]={0};
        const unsigned char *p=input,*end=input+8;
        while(p<end){unsigned char n=consume(p,end,mode);if(!n)return 0;
          p+=n+)c" + std::to_string(extra) +
                               R"c(;}
        return p==end?0:1;
      }
    )c";
    EXPECT_EQ(workflowContract(source, "client").complete(), extra == 0);
  }
}

TEST(RecursiveContracts, SequentialCursorLoopsKeepProvedBounds) {
  for (const unsigned step : {1U, 3U}) {
    const std::string source =
        "int client(void){const unsigned char data[6]={0};"
        "const unsigned char *first=data+1,*last=data+1;"
        "while(last<data+5)last++;"
        "while(first<last)first+=" +
        std::to_string(step) + ";return first==last?0:1;}";
    EXPECT_EQ(workflowContract(source, "client").complete(), step == 1);
  }
}

TEST(RecursiveContracts, NumericContentsSupportOnlyUnchangedFiniteClamps) {
  for (const auto &[initial, safe] : std::vector<std::pair<std::string, bool>>{
           {"char text[]=\"12.5\";", true},
           {"char text[]=\"nan\";", false},
           {"char input[]=\"12\",text[3];memcpy(text,input,3);", true},
           {"char text[]=\"123\";char *p=text;p[0]='n';p[1]='a';p[2]='n';",
            false},
           {"char "
            "text[]=\"123\";if(mode){text[0]='n';text[1]='a';text[2]='n';}",
            false}}) {
    const std::string source =
        "double strtod(const char*,char**);void *memcpy(void*,const "
        "void*,size_t);"
        "int client(int mode){" +
        initial +
        "double x=strtod(text,0);if(x>=2147483647.0)return 0;"
        "else if(x<=-2147483648.0)return 0;else return (int)x;}";
    EXPECT_EQ(workflowContract(source, "client").complete(), safe) << initial;
  }
  EXPECT_FALSE(
      workflowContract(
          "double strtod(const char*,char**);int client(void){char "
          "text[]=\"12\";"
          "double x=strtod(text,0);if(x>=2147483647.0)return 0;"
          "else if(x<=-2147483648.0)return 0;else {x=1e99;return (int)x;}}",
          "client")
          .complete());
}

TEST(RecursiveContracts, RefutedOutputStoresDoNotEscapeTheirSource) {
  for (const bool conversion : {false, true}) {
    const std::string source =
        "double strtod(const char*,char**);"
        "void keep(char *s,char **out){if(out)*out=s;}"
        "int client(void){char *p=malloc(2);if(!p)return 0;"
        "p[0]='1';p[1]=0;" +
        std::string(conversion ? "(void)strtod(p,0);" : "keep(p,0);") +
        "free(p);return 0;}";
    EXPECT_TRUE(workflowContract(source, "client").complete());
  }
}

TEST(RecursiveContracts, PrivateScalarWritesPreserveOtherNumericContents) {
  for (const bool overwrite : {false, true}) {
    const std::string source =
        "double strtod(const char*,char**);int client(void){"
        "char text[]=\"123\";char *out;unsigned count=0;"
        "count=3;out=text;" +
        std::string(overwrite ? "out[0]='n';out[1]='a';out[2]='n';" : "") +
        "double x=strtod(out,0);if(x>=2147483647.0)return 0;"
        "else if(x<=-2147483648.0)return 0;else return (int)x;}";
    EXPECT_EQ(workflowContract(source, "client").complete(), !overwrite);
  }
}

TEST(RecursiveContracts, AdjustedPointersDoNotCopyPointeeValues) {
  for (const std::string displacement : {"0", "1", "n"}) {
    const auto source =
        "int f(const unsigned char *p,unsigned n){if(*p!=0)return 0;"
        "const unsigned char *q=p+" +
        displacement +
        ";int a[1]={0};if(*q!=0)a[3]=1;return a[0];}"
        "int client(void){const unsigned char a[2]={0,1};return f(a,1);}";
    EXPECT_EQ(workflowContract(source, "client").complete(),
              displacement == "0");
  }
}

TEST(RecursiveContracts, PointerCountsAndBranchJoinsRetainActualBounds) {
  for (const unsigned step : {1U, 8U}) {
    const std::string source =
        "int scan(const unsigned char *data,size_t size){"
        "const unsigned char *p=data+1,*end=data+1;"
        "while((size_t)(end-data)<size&&*end!=34){"
        "if(*end==92){if((size_t)(end+1-data)>=size)return 0;end++;}end++;}"
        "if((size_t)(end-data)>=size||*end!=34)return 0;"
        "while(p<end){if(*p==92)p++;p+=" +
        std::to_string(step) +
        ";}return p==end;}"
        "int client(void){const unsigned char text[]={34,97,92,110,98,34,0};"
        "return scan(text,sizeof text);}";
    EXPECT_EQ(workflowContract(source, "client").complete(), step == 1);
  }
}

TEST(RecursiveContracts, SettledBodiesRetainVerifiedNullOutcomes) {
  for (const auto &forward : {"return c(s,n);", "c(s,n);return 1;",
                              "int ok=b(s,n);s->p=0;return ok;"}) {
    SCOPED_TRACE(forward);
    const std::string code = R"c(
      void *malloc(size_t); void free(void *);
      struct B {char *p;unsigned tag;};
      static int a(struct B *s,unsigned n);
      static int b(struct B *s,unsigned n) {
        if(n && !a(s,n-1))return 0;
        free(s->p);s->p=malloc(1);if(!s->p)return 0;*s->p=0;return 1;
      }
      static int c(struct B *s,unsigned n) {return b(s,n);}
      static int a(struct B *s,unsigned n) {
        s->tag=1;
        if(n&1)return b(s,n);
    )c" + std::string(forward) +
                             "}";
    const auto result =
        test::analyze(code, {.checkContracts = true, .checked = true});
    ASSERT_TRUE(result.ast);
    const auto *summary = result.summary("a");
    ASSERT_NE(summary, nullptr);
    const auto facts = summary->nonNullOn.find(core::Outcome::Positive);
    const bool nonNull =
        facts != summary->nonNullOn.end() &&
        facts->second.contains(core::SummaryPath::param(0).deref().field("p"));
    EXPECT_EQ(nonNull, std::string_view(forward) == "return c(s,n);");
  }
}

} // namespace weavec::analysis

namespace weavec::analysis {
TEST(RecursiveContracts, StringLengthCopiesRequireTheActualInitializedPrefix) {
  for (const std::string variant : {"direct", "callback", "forward", "overread",
                                    "stale", "missing", "tail"}) {
    const std::string helper =
        "char *copy(const char *s,void *(*allocate)(size_t)){if(!s)return 0;"
        "size_t n=strlen(s)+" +
        std::string(variant == "overread" ? "2" : "1") + ";char *p=" +
        std::string(variant == "callback" ? "allocate(n)" : "malloc(n)") +
        ";if(!p)return 0;" +
        std::string(variant == "stale" ? "free((void*)s);" : "") +
        "memcpy(p,s,n);return p;}";
    const std::string input =
        variant == "missing" ? "const char input[]={104,105};"
        : variant == "tail"  ? "char input[3];input[0]=104;input[2]=0;"
                             : "const char input[]=\"hi\";";
    const auto source =
        "size_t strlen(const char*);void *memcpy(void*,const void*,size_t);" +
        helper + "char *forward(const char *s){return copy(s,malloc);}" +
        "int client(void){" + input + "char *p=" +
        std::string(variant == "forward" ? "forward(input)"
                                         : "copy(input,malloc)") +
        ";if(p)free(p);return 0;}";
    EXPECT_EQ(workflowContract(source, "client").complete(),
              variant == "direct" || variant == "callback" ||
                  variant == "forward")
        << variant;
  }
}
} // namespace weavec::analysis

namespace weavec::analysis {
TEST(RecursiveContracts, TransparentCharacterCastsRetainLiteralWitnesses) {
  for (const std::string input :
       {"\"hi\"", "(const char*)\"hi\"", "(const unsigned char*)\"hi\"",
        "(const void*)\"hi\"", "(const char*)(\"hi\"+3)"}) {
    const std::string source =
        "size_t strlen(const char*);void *memcpy(void*,const void*,size_t);"
        "char *copy(const char *s){size_t n=strlen(s)+1;char *p=malloc(n);"
        "if(p)memcpy(p,s,n);return p;}int client(void){char *p=copy(" +
        input + ");free(p);return 0;}";
    EXPECT_EQ(workflowContract(source, "client").complete(),
              input.find("+3") == std::string::npos)
        << input;
  }
}
TEST(RecursiveContracts, ContainerOutputsReachOnlyCurrentDefiniteHeadAliases) {
  for (const std::string variant :
       {"direct", "forwarded", "payload", "released", "interior", "disowned",
        "lost-payload"}) {
    const std::string source =
        "void *calloc(size_t,size_t);"
        "struct node{struct node *next;char *payload;unsigned flags;};"
        "void drop(struct node *p){while(p){struct node *n=p->next;"
        "if(!(p->flags&256))free(p->payload);free(p);p=n;}}"
        "void update(struct node *p){p->flags=" +
        std::string(variant == "disowned" ? "256" : "2") +
        ";}void forward(struct node *p){update(p);}"
        "int client(void){struct node *p=calloc(1,sizeof *p);if(!p)return 0;" +
        std::string(
            variant == "payload" || variant == "disowned" ||
                    variant == "lost-payload"
                ? "p->payload=malloc(1);if(!p->payload){drop(p);return 0;}"
                : "") +
        "struct node *a=p;" +
        std::string(variant == "released"   ? "drop(a);"
                    : variant == "interior" ? "a=(struct node*)((char*)a+1);"
                    : variant == "lost-payload" ? "a->payload=0;"
                                                : "") +
        std::string(variant == "forwarded" ? "forward(a);" : "update(a);") +
        "drop(p);return 0;}";
    EXPECT_EQ(workflowContract(source, "client").complete(),
              variant == "direct" || variant == "forwarded" ||
                  variant == "payload")
        << variant;
  }
}
} // namespace weavec::analysis

namespace weavec::analysis {
TEST(RecursiveContracts, FloatingCallPremisesRequireProvedNonNanValues) {
  for (const std::string variant :
       {"finite", "forwarded", "infinite", "nan", "changed", "mixed"}) {
    const std::string source =
        "int clamp(double x){" +
        std::string(variant == "changed" ? "x=__builtin_nan(\"\");" : "") +
        "if(x>=2147483647.0)return 2147483647;if(x<=-2147483648.0)return "
        "(-2147483647-1);return (int)x;}"
        "int forward(double x){return clamp(x);}int client(int k){return " +
        std::string(variant == "forwarded" ? "forward" : "clamp") + "(" +
        std::string(variant == "infinite" ? "__builtin_inf()"
                    : variant == "nan"    ? "__builtin_nan(\"\")"
                    : variant == "mixed"  ? "k?1.0:__builtin_nan(\"\")"
                                          : "1.0") +
        ");}";
    EXPECT_FALSE(workflowContract(source, "clamp").complete());
    EXPECT_EQ(workflowContract(source, "client").complete(),
              variant == "finite" || variant == "forwarded" ||
                  variant == "infinite")
        << variant;
  }
}
} // namespace weavec::analysis

namespace weavec::analysis {
TEST(RecursiveContracts,
     FloatingEarlyReturnsRequireDominanceAndUnchangedValues) {
  for (const std::string variant :
       {"plain", "block", "jump", "write", "address"}) {
    const std::string source =
        "void change(double *x){*x=1e100;}int clamp(double x){" +
        std::string(variant == "jump" ? "goto cast;" : "") +
        "if(x>=2147483647.0)" +
        std::string(variant == "block" ? "{return 2147483647;}"
                                       : "return 2147483647;") +
        "if(x<=-2147483648.0)return (-2147483647-1);" +
        std::string(variant == "jump"      ? "cast:"
                    : variant == "write"   ? "x=1e100;"
                    : variant == "address" ? "change(&x);"
                                           : "") +
        "return (int)x;}int client(void){return clamp(1.0);}";
    EXPECT_EQ(workflowContract(source, "client").complete(),
              variant == "plain" || variant == "block")
        << variant;
  }
}
} // namespace weavec::analysis

namespace weavec::analysis {
TEST(RecursiveContracts, AllocationTransfersSurviveEveryReturningAlternative) {
  for (const std::string variant : {"kept", "disowned", "lost", "released"}) {
    const std::string source =
        "struct node{struct node *next,*child;unsigned flags;};"
        "struct node *make(void){struct node *p=malloc(sizeof *p);"
        "if(p){p->next=0;p->child=0;p->flags=0;}return p;}"
        "void drop(struct node *p){while(p){struct node *n=p->next;"
        "if(!(p->flags&1))drop(p->child);free(p);p=n;}}"
        "void build(struct node *p){struct node *q=make();if(!q)return;"
        "p->child=q;" +
        std::string(variant == "disowned"   ? "p->flags=1;"
                    : variant == "lost"     ? "p->child=0;"
                    : variant == "released" ? "drop(q);"
                                            : "") +
        "}int client(void){struct node *p=make();if(!p)return 0;"
        "build(p);drop(p);return 0;}";
    EXPECT_EQ(workflowContract(source, "client").complete(), variant == "kept")
        << variant;
  }
}
} // namespace weavec::analysis

namespace weavec::analysis {
TEST(RecursiveContracts, LocalContainerFoldsRetainOnlyLiveHeadAliases) {
  for (const std::string variant :
       {"direct", "selector", "lost", "duplicate", "released", "interior"}) {
    const std::string source =
        "void *calloc(size_t,size_t);"
        "struct node{struct node *next,*child;unsigned flags;};"
        "void drop(struct node *p){while(p){struct node *n=p->next;"
        "if(!(p->flags&256))drop(p->child);free(p);p=n;}}"
        "int client(void){struct node *p=calloc(1,sizeof *p);if(!p)return 0;"
        "struct node *q=calloc(1,sizeof *q);if(!q){drop(p);return 0;}"
        "struct node *a=p;" +
        std::string(variant == "released"   ? "free(p);"
                    : variant == "interior" ? "a=(struct node*)((char*)p+1);"
                                            : "") +
        "a->child=q;" +
        std::string(variant == "selector"    ? "p->flags=2;"
                    : variant == "lost"      ? "a->child=0;"
                    : variant == "duplicate" ? "a->next=q;"
                                             : "") +
        "drop(p);return 0;}";
    EXPECT_EQ(workflowContract(source, "client").complete(),
              variant == "direct" || variant == "selector")
        << variant;
  }
}
} // namespace weavec::analysis

namespace weavec::analysis {
TEST(RecursiveContracts, HelperContainerFramesRequireConfinedEffects) {
  for (const std::string variant :
       {"direct", "forwarded", "reader", "lost", "released", "disowned"}) {
    const std::string source =
        "struct node{struct node *next,*child;unsigned flags;};"
        "struct reader{unsigned count;};"
        "struct node *make(void){struct node *p=malloc(sizeof *p);"
        "if(p){p->next=0;p->child=0;p->flags=0;}return p;}"
        "void drop(struct node *p){while(p){struct node *n=p->next;"
        "if(!(p->flags&256))drop(p->child);free(p);p=n;}}"
        "void mark(struct node *p){p->flags=2;}"
        "void forward(struct node *p){mark(p);}"
        "void readmark(struct node *p,struct reader *r){p->flags=2;r->count=1;}"
        "void change(struct node *p,struct node *root){p->flags=2;" +
        std::string(variant == "lost"       ? "root->child=0;"
                    : variant == "released" ? "drop(root);"
                                            : "root->flags=256;") +
        "}int client(void){struct node *p=make();if(!p)return 0;"
        "struct node *q=make();if(!q){drop(p);return 0;}"
        "struct node *r=make();if(!r){drop(p);drop(q);return 0;}"
        "p->child=q;q->next=r;" +
        std::string(variant == "direct"      ? "mark(r);"
                    : variant == "forwarded" ? "forward(r);"
                    : variant == "reader"
                        ? "struct reader in={0};readmark(r,&in);"
                        : "change(r,p);") +
        "drop(p);return 0;}";
    EXPECT_EQ(workflowContract(source, "client").complete(),
              variant == "direct" || variant == "forwarded" ||
                  variant == "reader")
        << variant;
  }
}
} // namespace weavec::analysis
