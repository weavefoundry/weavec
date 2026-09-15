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
