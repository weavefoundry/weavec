//===- ContainerTest.cpp - Linked proof boundaries (RFC 0023) ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"

#include <gtest/gtest.h>

#include <array>
#include <set>
#include <string>

namespace weavec::analysis {

static const std::string ContainerPrelude = R"c(
  void *malloc(__SIZE_TYPE__);
  void free(void *);
  struct node { unsigned value; struct node *next; };
  static unsigned count(const struct node *p) {
    unsigned n=0;
    while(p) { ++n; p=p->next; }
    return n;
  }
  static void destroy(struct node *p) {
    while(p) { struct node *next=p->next; free(p); p=next; }
  }
)c";

static core::CheckedContract
containerCheck(const std::string &body, const std::string &name = "client") {
  AnalysisOptions options;
  options.checkedFunctions.insert(name);
  const auto unit = test::analyze(ContainerPrelude + body, options);
  if (!unit.ast || !unit.summary(name)) {
    ADD_FAILURE() << "container fixture could not be analyzed";
    return {};
  }
  return unit.summary(name)->checked;
}

TEST(ContainerAnalysis, TraversalExportsTheChainPremise) {
  const auto contract = containerCheck("", "count");
  ASSERT_TRUE(contract.complete());
  ASSERT_EQ(contract.requirements.size(), 1U);
  const auto &requirement = *contract.requirements.begin();
  EXPECT_EQ(requirement.kind, core::CheckedRequirementKind::Container);
  EXPECT_EQ(requirement.path, core::SummaryPath::param(0));
  const auto shape = core::ContainerShape::decode(requirement.family);
  ASSERT_TRUE(shape);
  EXPECT_EQ(shape->link.name, "next");
  EXPECT_EQ(shape->access, core::ContainerAccess::Read);
  EXPECT_FALSE(shape->terminal);
}

TEST(ContainerAnalysis, BorrowedTraversalAndOwnedReleaseHaveDifferentPremises) {
  EXPECT_TRUE(containerCheck("int client(void) { struct node a={1,0}, "
                             "b={2,&a}; return count(&b); }")
                  .complete());
  EXPECT_FALSE(
      containerCheck(
          "int client(void) { struct node a={1,0}; destroy(&a); return 0; }")
          .complete());
  const auto release = containerCheck("", "destroy");
  ASSERT_TRUE(release.complete());
  ASSERT_EQ(release.requirements.size(), 1U);
  const auto shape =
      core::ContainerShape::decode(release.requirements.begin()->family);
  ASSERT_TRUE(shape);
  EXPECT_EQ(shape->access, core::ContainerAccess::Release);
  EXPECT_EQ(shape->family, "free");
}

TEST(ContainerAnalysis, UninitializedLinksAndDataDoNotEstablishAChain) {
  EXPECT_FALSE(
      containerCheck(
          "int client(void) { struct node a; a.value=1; return count(&a); }")
          .complete());
  EXPECT_FALSE(
      containerCheck(
          "int client(void) { struct node a; a.next=0; return count(&a); }")
          .complete());
  EXPECT_FALSE(containerCheck("int client(int n) { struct node a; if(n) "
                              "a.next=0; a.value=1; return count(&a); }")
                   .complete());
}

TEST(ContainerAnalysis, NodeAndFieldNamesAreNotSpecialCases) {
  const auto contract = containerCheck(R"c(
    struct packet { struct packet *following; unsigned tag; };
    static unsigned visit(const struct packet *cursor) {
      unsigned result=0;
      for(; cursor; cursor=cursor->following) result+=cursor->tag;
      return result;
    }
    int client(void) { struct packet last={0,2}, first={&last,1}; return visit(&first); }
  )c");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(contract.requirements.empty());
}

TEST(ContainerAnalysis, SavedSuccessorIsLiveButTheFreedNodeIsNot) {
  EXPECT_FALSE(containerCheck(R"c(
    void client(struct node *p) {
      while(p) { free(p); p=p->next; }
    }
  )c")
                   .complete());
  EXPECT_TRUE(containerCheck(R"c(
    int client(void) {
      struct node *p=malloc(sizeof *p);
      if(!p) return 0;
      p->value=1; p->next=0;
      destroy(p);
      return 0;
    }
  )c")
                  .complete());
}

TEST(ContainerAnalysis, AliasedCycleCannotAcquireAContainerContract) {
  EXPECT_FALSE(containerCheck(R"c(
    static void corrupt(struct node *p) { p->next=p; }
    int client(void) { struct node a={1,0}; corrupt(&a); return count(&a); }
  )c")
                   .complete());
  EXPECT_FALSE(containerCheck(R"c(
    unsigned client(struct node *p) { if(p) p->next=p; return count(p); }
  )c")
                   .complete());
}

TEST(ContainerAnalysis, ByteWritesAndUnknownCallsRetireEntryEvidence) {
  EXPECT_FALSE(containerCheck(R"c(
    void *memset(void *, int, __SIZE_TYPE__);
    unsigned client(struct node *p) { if(p) memset(p,127,sizeof *p); return count(p); }
  )c")
                   .complete());
  EXPECT_FALSE(containerCheck(R"c(
    unsigned client(struct node *p, void (*call)(struct node *)) {
      call(p); return count(p);
    }
  )c")
                   .complete());
}

TEST(ContainerAnalysis,
     ConstStorageAndIncompatibleRecoveryDoNotBecomeWritableNodes) {
  EXPECT_FALSE(containerCheck(R"c(
    static void change(struct node *p) { while(p) { p->value=2; p=p->next; } }
    int client(void) { static const struct node a={1,0}; change((struct node *)&a); return 0; }
  )c")
                   .complete());
  EXPECT_FALSE(containerCheck(R"c(
    int client(void) { char raw[3*sizeof(struct node)]={0}; return count((struct node *)(raw+1)); }
  )c")
                   .complete());
}

TEST(ContainerAnalysis, RuntimeConstructionExportsFreshOwnership) {
  const auto contract = containerCheck(R"c(
    struct node *client(unsigned n) {
      struct node *head=0;
      for(unsigned i=0;i<n;++i) {
        struct node *p=malloc(sizeof *p);
        if(!p) break;
        p->value=i; p->next=head; head=p;
      }
      return head;
    }
  )c");
  ASSERT_TRUE(contract.complete());
  EXPECT_TRUE(contract.requirements.empty());
  EXPECT_TRUE(std::ranges::any_of(contract.establishes, [](const auto &post) {
    return post.kind == core::CheckedRequirementKind::ContainerFresh &&
           post.path.isResult();
  }));
}

TEST(ContainerAnalysis, OwnershipRequirementsPropagateThroughReversalWrappers) {
  const std::string helpers = R"c(
    static struct node *reverse(struct node *p) {
      struct node *out=0;
      while(p) { struct node *next=p->next; p->next=out; out=p; p=next; }
      return out;
    }
    static void cleanup(struct node *p) { destroy(reverse(p)); }
  )c";
  const auto wrapper = containerCheck(helpers, "cleanup");
  EXPECT_TRUE(wrapper.complete());
  EXPECT_TRUE(std::ranges::any_of(wrapper.requirements, [](const auto &entry) {
    const auto descriptor = core::ContainerShape::decode(entry.family);
    return entry.kind == core::CheckedRequirementKind::Container &&
           descriptor && descriptor->access == core::ContainerAccess::Release;
  }));
  // A derived result is a subset, not a promise that every original node
  // reaches the result. Preserve the ordinary leak obligation (RFC 0023).
  EXPECT_FALSE(containerCheck(helpers + R"c(
    int client(void) {
      struct node *p=malloc(sizeof *p); if(!p) return 0;
      p->value=1; p->next=0; cleanup(p); return 0;
    }
  )c")
                   .complete());
  EXPECT_FALSE(containerCheck(helpers + R"c(
    int client(void) { struct node p={1,0}; cleanup(&p); return 0; }
  )c")
                   .complete());
}

TEST(ContainerAnalysis, DirectCleanupWrappersPreserveTheReleaseSummary) {
  EXPECT_TRUE(containerCheck(R"c(
    static void cleanup(struct node *p) { destroy(p); }
    int client(void) {
      struct node *p=malloc(sizeof *p); if(!p) return 0;
      p->value=1; p->next=0; cleanup(p); return 0;
    }
  )c")
                  .complete());
}

TEST(ContainerAnalysis, SeveralFreshOutputsCanStillAlias) {
  const std::string helpers = R"c(
    void abort(void) __attribute__((noreturn));
    static struct node *make(struct node **alias) {
      struct node *p=malloc(sizeof *p); if(!p) abort();
      p->value=1; p->next=0; *alias=p; return p;
    }
    static struct node *concat(struct node *a, struct node *b) {
      if(!a) return b;
      struct node *p=a; while(p->next) p=p->next;
      p->next=b; return a;
    }
  )c";
  const auto producer = containerCheck(helpers, "make");
  ASSERT_TRUE(producer.complete());
  EXPECT_TRUE(std::ranges::any_of(producer.establishes, [](const auto &post) {
    return post.kind == core::CheckedRequirementKind::ContainerFresh;
  }));
  EXPECT_FALSE(std::ranges::any_of(producer.establishes, [](const auto &post) {
    return post.kind == core::CheckedRequirementKind::ContainerSeparated;
  }));
  EXPECT_FALSE(containerCheck(helpers + R"c(
    int client(void) {
      struct node *b=0; struct node *a=make(&b);
      destroy(concat(a,b)); return 0;
    }
  )c")
                   .complete());
}

TEST(ContainerAnalysis, EmptyConcatInputsDoNotWeakenOwnedNodes) {
  EXPECT_TRUE(containerCheck(R"c(
    static struct node *concat(struct node *a, struct node *b) {
      if(!a) return b;
      struct node *p=a;
      while(p->next) p=p->next;
      p->next=b; return a;
    }
    int client(void) {
      struct node *p=malloc(sizeof *p); if(!p) return 0;
      p->value=1; p->next=0;
      struct node *q=concat(0,p); destroy(q); return 0;
    }
  )c")
                  .complete());
}

TEST(ContainerAnalysis, ACallbackThatMayDestroyTheTailCannotPreserveIt) {
  EXPECT_FALSE(containerCheck(R"c(
    static void release_one(struct node *p) { free(p); }
    int client(int n) {
      struct node *b=malloc(sizeof *b); if(!b) return 0;
      b->value=1; b->next=0;
      struct node *a=malloc(sizeof *a); if(!a) { destroy(b); return 0; }
      a->value=2; a->next=b;
      void (*release)(struct node *)=n?destroy:release_one;
      release(a); unsigned value=count(b); destroy(b); return value;
    }
  )c")
                   .complete());
}

TEST(ContainerAnalysis, AReadOnlySuccessorHelperPreservesTheSavedTail) {
  EXPECT_TRUE(containerCheck(R"c(
    static struct node *successor(struct node *p) { return p->next; }
    int client(void) {
      struct node *b=malloc(sizeof *b); if(!b) return 0;
      b->value=2; b->next=0;
      struct node *a=malloc(sizeof *a); if(!a) { destroy(b); return 0; }
      a->value=1; a->next=b;
      struct node *next=successor(a); free(a); destroy(next); return 0;
    }
  )c")
                  .complete());
}

TEST(ContainerAnalysis, PointerArithmeticCannotRetainAChainPredicate) {
  EXPECT_FALSE(containerCheck(R"c(
    unsigned client(struct node *p) { p++; return count(p); }
  )c")
                   .complete());
  EXPECT_FALSE(containerCheck(R"c(
    unsigned client(struct node *p) { if(p) p->next++; return count(p); }
  )c")
                   .complete());
  EXPECT_FALSE(containerCheck(R"c(
    unsigned client(struct node *p) { if(p) p->next+=1; return count(p); }
  )c")
                   .complete());
}

// Independent concrete graph oracle. Expected acceptance follows integer
// successor edges; it does not read inferred shapes or call Core's prover.
TEST(ContainerAnalysis, EveryThreeNodeTopologyMatchesTheConcreteChainOracle) {
  constexpr std::array Names{"a", "b", "c"};
  for (unsigned encoding = 0; encoding < 64; ++encoding) {
    auto remaining = encoding;
    std::array<unsigned, 3> links{};
    std::string source =
        "int client(void) { struct node a={0,0}, b={0,0}, c={0,0};";
    for (std::size_t i = 0; i < links.size(); ++i) {
      links.at(i) = remaining % 4;
      remaining /= 4;
      source +=
          std::string(Names.at(i)) + ".next=" +
          (links.at(i) ? "&" + std::string(Names.at(links.at(i) - 1)) : "0") +
          ";";
    }
    source += "return count(&a);}";
    std::set<unsigned> visited;
    unsigned cursor = 1;
    bool valid = true;
    while (cursor) {
      if (!visited.insert(cursor).second) {
        valid = false;
        break;
      }
      cursor = links.at(cursor - 1);
    }
    const auto contract = containerCheck(source);
    EXPECT_EQ(contract.complete(), valid) << encoding << '\n' << source;
    if (contract.complete())
      EXPECT_TRUE(contract.requirements.empty()) << encoding;
  }
}

TEST(ContainerAnalysis, EveryThreeNodeRelinkRetiresStaleTraversalEvidence) {
  constexpr std::array Names{"a", "b", "c"};
  for (unsigned changed = 0; changed < 3; ++changed) {
    for (unsigned successor = 0; successor < 4; ++successor) {
      std::array<unsigned, 3> links{2, 3, 0};
      links.at(changed) = successor;
      std::set<unsigned> visited;
      unsigned cursor = 1;
      bool valid = true;
      while (cursor) {
        if (!visited.insert(cursor).second) {
          valid = false;
          break;
        }
        cursor = links.at(cursor - 1);
      }
      for (const bool helper : {false, true}) {
        std::string source = helper ? "static void relink(struct node **out, "
                                      "struct node *p) { *out=p; }"
                                    : "";
        source += "int client(void) { struct node c={3,0}, b={2,&c}, a={1,&b}; "
                  "count(&a);";
        const auto value =
            successor ? "&" + std::string(Names.at(successor - 1)) : "0";
        source += helper
                      ? "relink(&" + std::string(Names.at(changed)) + ".next," +
                            value + ");"
                      : std::string(Names.at(changed)) + ".next=" + value + ";";
        source += "return count(&a);}";
        const auto contract = containerCheck(source);
        // Arbitrary pointer-output helpers lose the concrete link relation.
        // The direct transfer is exact; both forms must reject every cycle.
        if (!helper || !valid)
          EXPECT_EQ(contract.complete(), valid) << source;
        if (contract.complete())
          EXPECT_TRUE(contract.requirements.empty()) << source;
      }
    }
  }
}

} // namespace weavec::analysis
