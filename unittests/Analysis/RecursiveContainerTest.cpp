//===- RecursiveContainerTest.cpp - Recursive ownership (RFC 0027) --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"
#include "weavec/Core/CheckedIO.h"

#include <gtest/gtest.h>

#include <string>
namespace weavec::analysis {
static const std::string ForestPrelude = R"c(
#define NULL ((void *)0)
typedef __SIZE_TYPE__ size_t;
void *malloc(size_t);
void free(void *);
void *memset(void *, int, size_t);
struct hooks { void *(*allocate)(size_t); void (*deallocate)(void*); };
static struct hooks global_hooks = {malloc,free};
struct node { struct node *next,*prev,*child; int flags; char *text,*key; };
static void reset(struct hooks *h){if(!h){global_hooks.allocate=malloc;global_hooks.deallocate=free;return;}global_hooks=*h;}
static struct node *new_item(const struct hooks *h){struct node *p=(struct node*)h->allocate(sizeof(struct node));if(p)memset(p,0,sizeof *p);return p;}
static struct node *create(void){struct node *p=new_item(&global_hooks);if(p)p->flags=1;return p;}
static void destroy(struct node *p){struct node *next=0;while(p){next=p->next;if(!(p->flags&256)&&p->child)destroy(p->child);if(!(p->flags&256)&&p->text){global_hooks.deallocate(p->text);p->text=0;}if(!(p->flags&512)&&p->key){global_hooks.deallocate(p->key);p->key=0;}global_hooks.deallocate(p);p=next;}}
static void suffix(struct node *last,struct node *item){last->next=item;item->prev=last;}
static int add(struct node *array,struct node *item){struct node *child=0;if(!item||!array||array==item)return 0;child=array->child;if(!child){array->child=item;item->prev=item;item->next=0;}else{if(child->prev){suffix(child->prev,item);array->child->prev=item;}}return 1;}
)c";
static core::CheckedContract forestCheck(const std::string &body,
                                         const std::string &name = "client") {
  AnalysisOptions options;
  options.checkedFunctions.insert(name);
  const auto unit = test::analyze(ForestPrelude + body, options);
  if (!unit.ast || !unit.summary(name)) {
    ADD_FAILURE() << "recursive fixture could not be analyzed";
    return {};
  }
  return unit.summary(name)->checked;
}
TEST(RecursiveContainerAnalysis, SaturatedContainerContractsRemainPortable) {
  std::string source = "struct node { unsigned value; struct node *next; }; "
                       "struct node *update(struct node *p";
  for (unsigned i = 0; i < 90; ++i)
    source += ", unsigned *a" + std::to_string(i);
  source += ") {";
  for (unsigned i = 0; i < 90; ++i)
    source += "*a" + std::to_string(i) + " += 1;";
  source += "if (p) { p->value=1; p=p->next; } return p; }";
  AnalysisOptions options;
  options.checkedFunctions.insert("update");
  const auto unit = test::analyze(source, options);
  ASSERT_NE(unit.summary("update"), nullptr);
  const auto &contract = unit.summary("update")->checked;
  EXPECT_TRUE(contract.limited);
  EXPECT_FALSE(contract.complete());
  EXPECT_EQ(
      core::parseCheckedContract(core::printCheckedContract(contract, {}), {}),
      contract);
}

TEST(RecursiveContainerAnalysis,
     ActualGlobalHooksFlowThroughConstructorHelpers) {
  const auto contract = forestCheck(R"c(
    int client(void) { reset(0); struct node *p=create();
      if (!p) return 0; destroy(p); return 0; }
  )c");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(contract.requirements.empty());
}

TEST(RecursiveContainerAnalysis,
     HeadPredicateValuesDischargeOnlyCurrentReturnGuards) {
  for (const std::string variant :
       {"null", "changed", "alias", "failed-replacement", "replacement",
        "released", "allocation"}) {
    SCOPED_TRACE(variant);
    std::string mutation;
    if (variant == "changed")
      mutation = "p->flags=2;";
    else if (variant == "alias")
      mutation = "struct node *q=p;q->flags=2;";
    else if (variant == "failed-replacement")
      mutation = "destroy(p);p=create();if(!p)return 0;p->flags=2;";
    else if (variant == "replacement")
      mutation = "destroy(p);reset(0);p=create();if(!p)return 0;"
                 "p->flags=2;reset(&h);";
    else if (variant == "released")
      mutation = "destroy(p);";
    const auto contract =
        forestCheck(R"c(
      static void *fail(size_t n) { (void)n; return 0; }
      static void *render(const struct node *p) {
        if(p->flags!=1)return malloc(1);
        char *out=global_hooks.allocate(1);
        if(!out)return 0;
        *out='a';return out;
      }
      int client(void) {
        reset(0);struct node *p=create();if(!p)return 0;
        struct hooks h={)c" +
                    std::string(variant == "allocation" ? "malloc" : "fail") +
                    R"c(,free};reset(&h);
      )c" + mutation +
                    R"c(
        char *out=render(p);if(out){out[2]=1;free(out);}
      )c" + (variant == "released" ? "" : "destroy(p);") +
                    R"c(
        return 0;
      }
    )c");
    EXPECT_EQ(contract.complete(),
              variant == "null" || variant == "failed-replacement");
    EXPECT_TRUE(contract.requirements.empty());
  }
}

TEST(RecursiveContainerAnalysis, UnknownHooksAreNotAssumedToBeLibc) {
  EXPECT_FALSE(forestCheck(R"c(
    int client(struct hooks *h) { reset(h); struct node *p=create();
      if (!p) return 0; destroy(p); return 0; }
  )c")
                   .complete());
}
TEST(RecursiveContainerAnalysis, RecursiveCleanupRetainsGlobalReleaseEffects) {
  AnalysisOptions options;
  options.checkedFunctions = {"destroy", "client"};
  const auto unit = test::analyze(R"c(
    typedef __SIZE_TYPE__ size_t;
    void *calloc(size_t, size_t);
    void *malloc(size_t);
    void free(void *);
    struct node { unsigned value; struct node *left, *right; };
    static void *extra;
    static void destroy(struct node *p) {
      if (!p) return;
      destroy(p->left); destroy(p->right); free(extra); free(p);
    }
    int client(void) {
      struct node *a=calloc(1,sizeof *a); if(!a)return 0;
      struct node *b=calloc(1,sizeof *b); if(!b){free(a);return 0;}
      extra=malloc(4); if(!extra){free(a);free(b);return 0;}
      a->left=b; destroy(a); return 0;
    }
  )c",
                                  options);
  ASSERT_NE(unit.summary("destroy"), nullptr);
  ASSERT_NE(unit.summary("client"), nullptr);
  EXPECT_FALSE(unit.summary("destroy")->checked.complete());
  EXPECT_FALSE(unit.summary("client")->checked.complete());
}
TEST(RecursiveContainerAnalysis, RecursiveNodesNeedTheirActualCallbackTargets) {
  AnalysisOptions options;
  options.checkedFunctions = {"destroy", "client"};
  const auto unit = test::analyze(R"c(
    typedef __SIZE_TYPE__ size_t;
    void *calloc(size_t, size_t);
    void free(void *);
    struct node {
      unsigned value; struct node *left, *right; void (*dispose)(void *);
    };
    static void destroy(struct node *p) {
      if (!p) return;
      destroy(p->left); destroy(p->right); p->dispose(p);
    }
    static void twice(void *p) { free(p); free(p); }
    int client(void) {
      struct node *a=calloc(1,sizeof *a); if(!a)return 0;
      struct node *b=calloc(1,sizeof *b); if(!b){free(a);return 0;}
      a->left=b; a->dispose=free; b->dispose=twice;
      destroy(a); return 0;
    }
  )c",
                                  options);
  ASSERT_NE(unit.summary("destroy"), nullptr);
  ASSERT_NE(unit.summary("client"), nullptr);
  EXPECT_FALSE(unit.summary("destroy")->checked.complete());
  EXPECT_FALSE(unit.summary("client")->checked.complete());
}
TEST(RecursiveContainerAnalysis,
     ImportedDiscoveryPreservesDependenciesAndObservesNewDatabaseFacts) {
  const std::string declarations = R"c(
    struct node { unsigned value; struct node *left, *right; };
    unsigned total(struct node *p);
  )c";
  auto client = test::analyze(declarations);
  ASSERT_TRUE(client.ast);
  const clang::RecordDecl *record = nullptr;
  const auto &context = client.ast->getASTContext();
  for (const auto *decl : context.getTranslationUnitDecl()->decls())
    if (const auto *candidate = llvm::dyn_cast<clang::RecordDecl>(decl);
        candidate != nullptr && candidate->getName() == "node")
      record = candidate;
  ASSERT_NE(record, nullptr);
  SummaryStore store;
  ProgramDatabase database;
  store.setContext(&context);
  store.setDatabase(&database);
  SummaryStore::Dependencies cold;
  store.beginDependencies(cold);
  EXPECT_TRUE(store.recursiveLinks(*record).empty());
  store.endDependencies();
  EXPECT_TRUE(cold.contains("total"));
  SummaryStore::Dependencies warm;
  store.beginDependencies(warm);
  EXPECT_TRUE(store.recursiveLinks(*record).empty());
  store.endDependencies();
  EXPECT_EQ(cold, warm);

  AnalysisOptions options;
  options.checkedFunctions.insert("total");
  const auto library = test::analyze(declarations + R"c(
    unsigned total(struct node *p) {
      if (!p) return 0;
      return p->value + total(p->left) + total(p->right);
    }
  )c",
                                     options);
  ASSERT_NE(library.summary("total"), nullptr);
  ASSERT_TRUE(library.summary("total")->checked.complete());
  database.add(library.analyzer->exports());
  const auto links = store.recursiveLinks(*record);
  ASSERT_EQ(links.size(), 2U);
  EXPECT_EQ(links[0]->getName(), "left");
  EXPECT_EQ(links[1]->getName(), "right");
}
TEST(RecursiveContainerAnalysis, ExplicitReturnExportsAttachedOwnership) {
  const auto contract = forestCheck(R"c(
    static int forward(struct node *a, struct node *b) { return add(a,b); }
    int client(void) { reset(0); struct node *a=create(); if(!a)return 0;
      struct node *b=create(); if(!b){destroy(a);return 0;}
      if(!forward(a,b)){destroy(b);destroy(a);return 0;}
      destroy(a);return 0; }
  )c");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(contract.requirements.empty());
}
TEST(RecursiveContainerAnalysis, NullSlotHistoryCannotEraseAttachedAllocation) {
  EXPECT_FALSE(forestCheck(R"c(
    int client(void) { reset(0); struct node *a=create(); if(!a)return 0;
      struct node *b=create(); if(!b){destroy(a);return 0;}
      a->child=b; a->child=0; destroy(a);return 0; }
  )c")
                   .complete());
}
TEST(RecursiveContainerAnalysis, ChangingOwnershipFlagsDoesNotConsumeAChild) {
  EXPECT_FALSE(forestCheck(R"c(
    int client(void) { reset(0); struct node *a=create(); if(!a)return 0;
      struct node *b=create(); if(!b){destroy(a);return 0;}
      a->child=b; a->flags=256; destroy(a);return 0; }
  )c")
                   .complete());
}
TEST(RecursiveContainerAnalysis, PartialDestructionCannotExportAnIntactParent) {
  EXPECT_FALSE(forestCheck(R"c(
    static struct node *broken(struct node *p) {
      if (p) destroy(p->child);
      return p;
    }
    int client(void) { reset(0); struct node *a=create(); if(!a)return 0;
      struct node *b=create(); if(!b){destroy(a);return 0;}
      a->child=b; destroy(broken(a));return 0; }
  )c")
                   .complete());
}
TEST(RecursiveContainerAnalysis,
     SavedDescendantsRemainDeadAfterRecursiveCleanup) {
  EXPECT_FALSE(forestCheck(R"c(
    int client(void) { reset(0); struct node *a=create(); if(!a)return 0;
      struct node *b=create(); if(!b){destroy(a);return 0;}
      a->child=b; destroy(a); return b->flags; }
  )c")
                   .complete());
}
TEST(RecursiveContainerAnalysis,
     FreshPayloadAcquisitionAndFailureAreAccounted) {
  const auto contract = forestCheck(R"c(
    int client(void) { reset(0); struct node *p=create(); if(!p)return 0;
      p->text=malloc(4); if(!p->text){destroy(p);return 0;}
      destroy(p);return 0; }
  )c");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(contract.requirements.empty());
  EXPECT_FALSE(forestCheck(R"c(
    int client(void) { reset(0); struct node *p=create(); if(!p)return 0;
      p->text=malloc(4); if(!p->text){destroy(p);return 0;}
      p->text=0; destroy(p);return 0; }
  )c")
                   .complete());
}

TEST(RecursiveContainerAnalysis,
     PayloadPublicationRequiresTheWholeAcquisition) {
  for (const std::string variant :
       {"direct", "helper", "duplicate", "interior", "released", "lost",
        "overwritten", "helper-leak"}) {
    SCOPED_TRACE(variant);
    const bool helper = variant == "helper" || variant == "helper-leak";
    std::string store = "n->text=p;";
    if (variant == "duplicate")
      store += "n->name=p;";
    else if (variant == "interior")
      store = "n->text=p+1;";
    else if (variant == "released")
      store = "free(p);n->text=p;";
    else if (variant == "lost")
      store.clear();
    const std::string source = R"c(
      typedef __SIZE_TYPE__ size_t;
      void *malloc(size_t); void *calloc(size_t,size_t); void free(void *);
      struct node { struct node *next,*child; char *text,*name; };
      static void drop(struct node *n) {
        while(n) { struct node *next=n->next; drop(n->child);
          free(n->text);free(n->name);free(n);n=next; }
      }
      static char *make(void) {
    )c" + std::string(variant == "helper-leak" ? "(void)malloc(2);" : "") +
                               R"c(
        char *p=malloc(4);if(p)p[0]=0;return p;
      }
      static int fill(struct node *n) { char *p=
    )c" + std::string(helper ? "make()" : "malloc(4)") +
                               ";if(!p)return 0;p[0]=0;" + store + R"c(
        return 1;
      }
      int client(void) {
        struct node *n=calloc(1,sizeof *n);if(!n)return 0;
    )c" + std::string(variant == "overwritten" ? "n->text=malloc(2);" : "") +
                               "fill(n);drop(n);return 0;}";
    AnalysisOptions options;
    options.checkedFunctions.insert("client");
    const auto unit = test::analyze(source, options);
    ASSERT_NE(unit.summary("client"), nullptr);
    const auto &contract = unit.summary("client")->checked;
    EXPECT_EQ(contract.complete(), variant == "direct" || variant == "helper");
    if (contract.complete())
      EXPECT_TRUE(contract.requirements.empty());
  }
}

TEST(RecursiveContainerAnalysis, PayloadFramesRequireActualInputSeparation) {
  for (const std::string variant :
       {"direct", "early", "helper", "alias", "aliased-helper", "duplicate"}) {
    SCOPED_TRACE(variant);
    const bool helper = variant == "helper" || variant == "aliased-helper";
    const bool alias = variant == "alias" || variant == "aliased-helper";
    const std::string write = helper ? "step(cursor);" : "*cursor=256;";
    std::string body = R"c(
      static void step(int *p) { *p=256; }
      static int fill(struct node *n,int *cursor) {
    )c" + std::string(variant == "early" ? write : "") +
                       R"c(
        char *p=malloc(4);if(!p){
    )c" + write + R"c(
          return 0;
        }
        p[0]=0;n->text=p;
    )c";
    if (variant == "duplicate")
      body += "n->key=p;";
    body += write;
    body += R"c(
        return 1;
      }
      int client(void) {
        reset(0);struct node *n=create();if(!n)return 0;int cursor=0;
        fill(n,
    )c" + std::string(alias ? "&n->flags" : "&cursor") +
            ");destroy(n);return 0;}";
    const auto contract = forestCheck(body);
    EXPECT_EQ(contract.complete(), !alias && variant != "duplicate");
    if (contract.complete())
      EXPECT_TRUE(contract.requirements.empty());
  }
}

TEST(RecursiveContainerAnalysis, PayloadRelocationConservesActualOwnership) {
  for (const std::string variant :
       {"helper", "local", "alias", "duplicate", "interior", "overwritten",
        "released", "intervening"}) {
    SCOPED_TRACE(variant);
    std::string source = R"c(
      typedef __SIZE_TYPE__ size_t;
      void *malloc(size_t);void *calloc(size_t,size_t);void free(void *);
      struct node {struct node *next,*child;char *text,*name;};
      static void drop(struct node *n) {while(n){struct node *next=n->next;
        drop(n->child);free(n->text);free(n->name);free(n);n=next;}}
      static int fill(struct node *n) {char *p=malloc(4);if(!p)return 0;
        p[0]=0;n->text=p;return 1;}
      int client(void) {struct node *n=calloc(1,sizeof *n);if(!n)return 0;
    )c";
    source += variant == "local" ? "n->text=malloc(4);" : "fill(n);";
    if (variant == "overwritten")
      source += "n->name=malloc(4);";
    if (variant == "released")
      source += "free(n->text);";
    if (variant == "alias") {
      source += "struct node *p=n;p->name=p->text;p->text=0;";
    } else if (variant == "interior") {
      source += "if(n->text){n->name=n->text+1;n->text=0;}";
    } else {
      source += "n->name=n->text;";
      if (variant == "intervening")
        source += "free(n->text);";
      if (variant != "duplicate")
        source += "n->text=0;";
    }
    source += "drop(n);return 0;}";
    AnalysisOptions options;
    options.checkedFunctions.insert("client");
    const auto unit = test::analyze(source, options);
    ASSERT_NE(unit.summary("client"), nullptr);
    const auto &contract = unit.summary("client")->checked;
    EXPECT_EQ(contract.complete(),
              variant == "helper" || variant == "local" || variant == "alias");
    if (contract.complete())
      EXPECT_TRUE(contract.requirements.empty());
  }
}

TEST(RecursiveContainerAnalysis,
     ComparisonFramesRetainOrdinaryReadObligations) {
  for (const std::string variant : {"bounded", "bytes", "string", "released",
                                    "uninitialized", "extent", "unknown"}) {
    SCOPED_TRACE(variant);
    std::string source = R"c(
      typedef __SIZE_TYPE__ size_t;
      void *calloc(size_t,size_t);void free(void *);
      int strcmp(const char *,const char *);
      int strncmp(const char *,const char *,size_t);
      int memcmp(const void *,const void *,size_t);
      struct node {struct node *next,*child;unsigned value;};
      void unknown(struct node *);
      static unsigned read_tree(const struct node *n) {
        return n?n->value+read_tree(n->child)+read_tree(n->next):0;
      }
      static void drop(struct node *n) {while(n){struct node *next=n->next;
        drop(n->child);free(n);n=next;}}
      static unsigned inspect(struct node *n,const char *s) {
    )c";
    if (variant == "released")
      source += "free(n);";
    if (variant == "unknown")
      source += "unknown(n);";
    std::string comparison = "strncmp(s,\"ok\",2)";
    if (variant == "string")
      comparison = "strcmp(s,\"ok\")";
    else if (variant == "bytes")
      comparison = "memcmp(s,\"ok\",2)";
    else if (variant == "extent")
      comparison = "memcmp(s,\"okay\",4)";
    source +=
        "if(" + comparison + "==0)return read_tree(n);return read_tree(n);}";
    source += R"c(
      int client(void) {struct node *n=calloc(1,sizeof *n);if(!n)return 0;
        n->value=3;
    )c";
    source += variant == "uninitialized" ? "char s[3];s[0]='o';"
                                         : "const char s[]=\"ok\";";
    source += "unsigned r=inspect(n,s);";
    if (variant != "released")
      source += "drop(n);";
    source += "return r==3?0:1;}";
    AnalysisOptions options;
    options.checkedFunctions.insert("client");
    const auto unit = test::analyze(source, options);
    ASSERT_NE(unit.summary("client"), nullptr);
    const auto &contract = unit.summary("client")->checked;
    EXPECT_EQ(contract.complete(), variant == "bounded" || variant == "bytes" ||
                                       variant == "string");
    if (contract.complete())
      EXPECT_TRUE(contract.requirements.empty());
  }
}

TEST(RecursiveContainerAnalysis, PayloadExtensionSurvivesReaderForwarding) {
  for (const std::string variant : {"goto", "early", "leak", "duplicate",
                                    "released-head", "released-payload"}) {
    SCOPED_TRACE(variant);
    std::string source = R"c(
      typedef __SIZE_TYPE__ size_t;
      void *malloc(size_t);void *calloc(size_t,size_t);void free(void *);
      struct node {struct node *next,*child;unsigned flags;char *text,*name;};
      struct reader {const unsigned char *content;size_t length,offset,depth;};
      static void drop(struct node *n) {
        while(n){struct node *next=n->next;drop(n->child);
          if(!(n->flags&1))free(n->text);free(n->name);free(n);n=next;}
      }
      static int role(struct reader *r) {
        if(r->offset<r->length){r->offset++;return 1;}return 0;
      }
      static int fill(struct node *n,struct reader *r) {char *p=malloc(4);
    )c";
    source += variant == "early" ? "if(!p){r->offset=1;return 0;}"
                                 : "if(!p)goto fail;";
    source += "p[0]=0;";
    if (variant == "released-payload")
      source += "free(p);";
    if (variant != "leak")
      source += "n->text=p;";
    if (variant == "duplicate")
      source += "n->name=p;";
    source += "r->offset++;return 1;";
    if (variant != "early") {
      source += "fail:";
      if (variant == "released-head")
        source += "free(n);";
      source += "r->offset=1;return 0;";
    }
    source += R"c(
      }
      static int wrap(struct node *n,struct reader *r) {
        if(!fill(n,r))return 0;r->offset++;return 1;
      }
      int client(void) {
        struct node *n=calloc(1,sizeof *n);if(!n)return 0;
        static const unsigned char bytes[]="abc";
        struct reader r={bytes,sizeof bytes,0,0};wrap(n,&r);drop(n);return 0;
      }
    )c";
    AnalysisOptions options;
    options.checkedFunctions.insert("client");
    const auto unit = test::analyze(source, options);
    ASSERT_NE(unit.summary("client"), nullptr);
    const auto &contract = unit.summary("client")->checked;
    EXPECT_EQ(contract.complete(), variant == "goto" || variant == "early");
    if (contract.complete())
      EXPECT_TRUE(contract.requirements.empty());
  }
}

TEST(RecursiveContainerAnalysis, NumericParserFramesRequireSeparateEndCells) {
  for (const std::string variant :
       {"null-end", "local-end", "record-end", "released", "uninitialized",
        "owned-end", "unknown"}) {
    SCOPED_TRACE(variant);
    std::string source = R"c(
      typedef __SIZE_TYPE__ size_t;
      void *calloc(size_t,size_t);void free(void *);
      double strtod(const char *,char **);
      struct node {struct node *next,*child;char *text;unsigned value;};
      void unknown(struct node *);
      static unsigned read_tree(const struct node *n) {
        return n?n->value+read_tree(n->child)+read_tree(n->next):0;
      }
      static void drop(struct node *n) {while(n){struct node *next=n->next;
        drop(n->child);free(n->text);free(n);n=next;}}
      static unsigned inspect(struct node *n,const char *s) {
        char *end=0;struct {char *end;} local={0};
    )c";
    if (variant == "released")
      source += "free(n);";
    std::string output = "&end";
    if (variant == "null-end")
      output = "0";
    else if (variant == "record-end")
      output = "&local.end";
    else if (variant == "owned-end")
      output = "&n->text";
    source += "(void)strtod(s," + output + ");";
    if (variant == "unknown")
      source += "unknown(n);";
    source += R"c(
        return read_tree(n);
      }
      int client(void) {struct node *n=calloc(1,sizeof *n);if(!n)return 0;
        n->value=3;
    )c";
    source += variant == "uninitialized" ? "char s[3];s[0]='1';"
                                         : "const char s[]=\"12\";";
    source += "unsigned r=inspect(n,s);";
    if (variant != "released")
      source += "drop(n);";
    source += "return r==3?0:1;}";
    AnalysisOptions options;
    options.checkedFunctions.insert("client");
    const auto unit = test::analyze(source, options);
    ASSERT_NE(unit.summary("client"), nullptr);
    const auto &contract = unit.summary("client")->checked;
    EXPECT_EQ(contract.complete(), variant == "null-end" ||
                                       variant == "local-end" ||
                                       variant == "record-end");
    if (contract.complete())
      EXPECT_TRUE(contract.requirements.empty());
  }
}

TEST(RecursiveContainerAnalysis, TemporaryReleaseFramesKeepActualEntryForests) {
  for (const std::string variant : {"guarded", "nullable", "helper", "interior",
                                    "twice", "lost", "attached"}) {
    SCOPED_TRACE(variant);
    std::string source = R"c(
      typedef __SIZE_TYPE__ size_t;
      void *malloc(size_t);void *calloc(size_t,size_t);void free(void *);
      struct node {struct node *next,*child;char *text;unsigned value;};
      static unsigned read_tree(const struct node *n) {
        return n?n->value+read_tree(n->child)+read_tree(n->next):0;
      }
      static void drop(struct node *n) {while(n){struct node *next=n->next;
        drop(n->child);free(n->text);free(n);n=next;}}
      static char *make(void) {return malloc(4);}
      static unsigned inspect(struct node *n) {char *p=
    )c";
    source += variant == "helper" ? "make();" : "malloc(4);";
    if (variant != "nullable")
      source += "if(!p)return read_tree(n);";
    if (variant == "attached")
      source += "n->text=p;";
    if (variant != "lost")
      source += variant == "interior" ? "free(p+1);" : "free(p);";
    if (variant == "twice")
      source += "free(p);";
    source += R"c(
        return read_tree(n);
      }
      int client(void) {struct node *n=calloc(1,sizeof *n);if(!n)return 0;
        n->value=3;unsigned r=inspect(n);drop(n);return r==3?0:1;}
    )c";
    AnalysisOptions options;
    options.checkedFunctions.insert("client");
    const auto unit = test::analyze(source, options);
    ASSERT_NE(unit.summary("client"), nullptr);
    const auto &contract = unit.summary("client")->checked;
    EXPECT_EQ(contract.complete(), variant == "guarded" ||
                                       variant == "nullable" ||
                                       variant == "helper");
    if (contract.complete())
      EXPECT_TRUE(contract.requirements.empty());
  }
}

TEST(RecursiveContainerAnalysis, OutcomeJoinsRetainCommonOwnershipStructure) {
  for (const std::string variant :
       {"direct", "helper", "branch", "released", "lost", "disowned"}) {
    SCOPED_TRACE(variant);
    std::string source = R"c(
      typedef __SIZE_TYPE__ size_t;
      void *malloc(size_t);void *calloc(size_t,size_t);void free(void *);
      struct node {struct node *next,*child;char *text;unsigned flags;};
      static void drop(struct node *n) {while(n){struct node *next=n->next;
        drop(n->child);if(!(n->flags&1))free(n->text);free(n);n=next;}}
      static int inspect(struct node *n) {
        void *p=malloc(4);if(!p)return 0;free(p);
    )c";
    if (variant == "released")
      source += "free(n);";
    else if (variant == "lost")
      source += "n->text=0;";
    else if (variant == "disowned")
      source += "n->flags=1;";
    else
      source += "n->flags=2;";
    source += R"c(
        return 1;
      }
      static int forward(struct node *n) {return inspect(n);}
      int client(void) {struct node *n=calloc(1,sizeof *n);if(!n)return 0;
        n->text=malloc(4);if(!n->text){drop(n);return 0;}
    )c";
    if (variant == "branch")
      source += "if(inspect(n))drop(n);else drop(n);";
    else if (variant == "helper")
      source += "(void)forward(n);drop(n);";
    else
      source += "(void)inspect(n);drop(n);";
    source += "return 0;}";
    AnalysisOptions options;
    options.checkedFunctions.insert("client");
    const auto unit = test::analyze(source, options);
    ASSERT_NE(unit.summary("client"), nullptr);
    const auto &contract = unit.summary("client")->checked;
    EXPECT_EQ(contract.complete(), variant == "direct" || variant == "helper" ||
                                       variant == "branch");
    if (contract.complete())
      EXPECT_TRUE(contract.requirements.empty());
  }
}

TEST(RecursiveContainerAnalysis, DetachmentPreservesBothAllocationPartitions) {
  const std::string helpers = R"c(
static struct node *get_array_item(const struct node *array, size_t index){ struct node *child=NULL;if(array==NULL)return NULL;child=array->child;while(child!=NULL&&index>0){index--;child=child->next;}return child;}
static struct node * via(struct node *parent, struct node * const item)
{
    if ((parent == NULL) || (item == NULL) || (item != parent->child && item->prev == NULL))
    {
        return NULL;
    }

    if (item != parent->child)
    {
        /* not the first element */
        item->prev->next = item->next;
    }
    if (item->next != NULL)
    {
        /* not the last element */
        item->next->prev = item->prev;
    }

    if (item == parent->child)
    {
        /* first element */
        parent->child = item->next;
    }
    else if (item->next == NULL)
    {
        /* last element */
        parent->child->prev = item->prev;
    }

    /* make sure the detached item doesn't point anywhere anymore */
    item->prev = NULL;
    item->next = NULL;

    return item;
}

static struct node * detach_at(struct node *array, int which)
{
    if (which < 0)
    {
        return NULL;
    }

    return via(array, get_array_item(array, (size_t)which));
}

)c";
  const auto contract = forestCheck(helpers + R"c(
int client(void){reset(0);struct node*a=create();if(!a)return 0;struct node*b=create();if(!b){destroy(a);return 0;}if(!add(a,b)){destroy(b);destroy(a);return 0;}struct node*child=detach_at(a,0);destroy(a);destroy(child);return 0;}
)c");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(contract.requirements.empty());
  EXPECT_FALSE(forestCheck(helpers + R"c(
int client(void){reset(0);struct node*a=create();if(!a)return 0;struct node*b=create();if(!b){destroy(a);return 0;}if(!add(a,b)){destroy(b);destroy(a);return 0;}struct node*child=detach_at(a,0);destroy(a);(void)child;return 0;}
)c")
                   .complete());
}

TEST(RecursiveContainerAnalysis, AttachedPayloadsTransferOnTheTestedOutcome) {
  // RFC 0029: a complete attach publishes the union of both owned inputs and
  // its own fresh payload on success, and preserves them separately on
  // failure. A forwarding wrapper carries that outcome-specific guarantee.
  for (const std::string variant : {"good", "leak", "twice", "ignored"}) {
    const std::string body =
        "static int attach(struct node *object,struct node *item){char *key=0;"
        "if(!object||!item||object==item)return 0;key=(char*)malloc(4);"
        "if(!key)return 0;key[0]=0;item->key=key;return add(object,item);}"
        "static int attach_wrapper(struct node *o,struct node *i){"
        "return attach(o,i);}"
        "int client(void){reset(0);struct node *o=create();if(!o)return 0;"
        "struct node *i=create();if(!i){destroy(o);return 0;}"
        "if(!attach_wrapper(o,i)){" +
        std::string(variant == "good"    ? "destroy(i);destroy(o);return 0;"
                    : variant == "leak"  ? "destroy(o);return 0;"
                    : variant == "twice" ? "destroy(i);destroy(o);return 0;"
                                         : "") +
        "}destroy(o);" + std::string(variant == "twice" ? "destroy(i);" : "") +
        "return 0;}";
    EXPECT_EQ(forestCheck(body).complete(), variant == "good") << variant;
  }
}

TEST(RecursiveContainerAnalysis,
     DirectByteResultsAndHelperReleasesAreLedgered) {
  // RFC 0029: a complete callee's fresh byte result returned without a local
  // holder transfers one allocation; a complete release helper settles it.
  for (const std::string variant :
       {"good", "unguarded", "leak", "twice", "interior"}) {
    const std::string body =
        "static char *render(const struct node *p){"
        "char *out=(char*)global_hooks.allocate(4);if(!out)return 0;"
        "out[0]=p->flags?'x':'y';out[1]=0;return out;}"
        "static char *publish(const struct node *p){return (char*)render(p)" +
        std::string(variant == "interior" ? "+1" : "") +
        ";}static void release(void *p){global_hooks.deallocate(p);}"
        "int client(void){reset(0);struct node *v=create();if(!v)return 0;"
        "char *t=publish(v);destroy(v);" +
        std::string(variant == "good" || variant == "interior"
                        ? "if(t)release(t);"
                    : variant == "unguarded" ? "release(t);"
                    : variant == "twice"     ? "if(t){release(t);release(t);}"
                                             : "") +
        "return 0;}";
    EXPECT_EQ(forestCheck(body).complete(),
              variant == "good" || variant == "unguarded")
        << variant;
  }
}

TEST(RecursiveContainerAnalysis, EveryCallbackTargetMustConsumeTheWholeInput) {
  const std::string helpers = R"c(
    static void first(struct node *p) { destroy(p); }
    static void second(struct node *p) { destroy(p); }
    static void partial(struct node *p) {
      if (p) { struct node *child=p->child; p->child=0; destroy(p); (void)child; }
    }
  )c";
  const auto contract = forestCheck(helpers + R"c(
    int client(int choose) { reset(0); struct node *p=create(); if(!p)return 0;
      void (*cleanup)(struct node *)=choose?first:second; cleanup(p);return 0; }
  )c");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(contract.requirements.empty());
  EXPECT_FALSE(forestCheck(helpers + R"c(
    int client(int choose) { reset(0); struct node *p=create(); if(!p)return 0;
      struct node *child=create(); if(!child){destroy(p);return 0;}
      p->child=child;
      void (*cleanup)(struct node *)=choose?first:partial; cleanup(p);return 0; }
  )c")
                   .complete());
}

TEST(RecursiveContainerAnalysis, CallbackTargetsPreserveOnlyCommonOutputs) {
  const std::string helpers = R"c(
    static struct node *first(struct node *p) { return p; }
    static struct node *second(struct node *p) { return p; }
    static struct node *drop(struct node *p) { (void)p; return 0; }
  )c";
  const auto contract = forestCheck(helpers + R"c(
    int client(int choose) { reset(0); struct node *p=create(); if(!p)return 0;
      struct node *(*forward)(struct node *)=choose?first:second;
      destroy(forward(p));return 0; }
  )c");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(contract.requirements.empty());
  EXPECT_FALSE(forestCheck(helpers + R"c(
    int client(int choose) { reset(0); struct node *p=create(); if(!p)return 0;
      struct node *(*forward)(struct node *)=choose?first:drop;
      destroy(forward(p));return 0; }
  )c")
                   .complete());
}

} // namespace weavec::analysis
