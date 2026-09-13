//===- UnionCasesTest.cpp - Checked input and member cases (RFC 0025) -----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"

#include <gtest/gtest.h>

namespace weavec::analysis {

static core::CheckedContract checkCase(const std::string &source) {
  AnalysisOptions options;
  options.checkedFunctions.insert("main");
  const auto unit = test::analyze(source, options);
  if (!unit.ast || !unit.summary("main")) {
    ADD_FAILURE() << "could not analyze case";
    return {};
  }
  return unit.summary("main")->checked;
}

TEST(UnionCases, AWriteBeforeTheFirstMemberReadCannotRestoreEntryEvidence) {
  for (const auto *write :
       {"((unsigned char *)u)[0] = 0;", "set(&u->other);", "u->other = 3;"}) {
    SCOPED_TRACE(write);
    const auto contract =
        checkCase(std::string("union value { int number; int other; }; "
                              "void set(int *p) { *p = 3; } "
                              "int read(union value *u) { ") +
                  write +
                  "return u->number; } "
                  "int main(void) { union value u = {.number = 7}; "
                  "return read(&u); }");
    EXPECT_FALSE(contract.complete());
  }
}

TEST(UnionCases, ADisjointWriteDoesNotRetireAnInputMember) {
  const auto contract = checkCase(R"c(
    union value { int number; int other; };
    int read(union value *u, int *p) { *p = 3; return u->number; }
    int main(void) {
      union value u = {.number = 7};
      int n = 0;
      return read(&u, &n);
    }
  )c");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(contract.requirements.empty());
}

TEST(UnionCases, RebindingAnOutputCannotInitializeTheOriginalInput) {
  const auto contract = checkCase(R"c(
    union value { int number; int *pointer; };
    void set(union value *p) {
      union value local;
      p = &local;
      p->number = 7;
    }
    int main(void) { union value u; set(&u); return u.number; }
  )c");
  EXPECT_FALSE(contract.complete());
}

TEST(UnionCases, WholeObjectAssignmentThroughAnAliasRetiresTheOldMember) {
  const auto contract = checkCase(R"c(
    union value { int number; int *pointer; };
    int main(void) {
      int n = 7;
      union value u = {.pointer = &n};
      union value *alias = &u;
      *alias = (union value){.number = 4};
      return *u.pointer;
    }
  )c");
  EXPECT_FALSE(contract.complete());
}

TEST(UnionCases, PointerWitnessCannotRestoreAFreedAllocation) {
  for (const auto *release :
       {"free(u.pointer);", "int *p=u.pointer; free(p);"}) {
    SCOPED_TRACE(release);
    const auto contract = checkCase(
        std::string(
            "union value {int number; int *pointer;}; int main(void){") +
        "int *p=malloc(sizeof(int)); if(!p)return 0; *p=7; "
        "union value u={.pointer=p}; {" +
        release + "} return *u.pointer;}");
    EXPECT_FALSE(contract.complete());
  }
}

TEST(UnionCases, ACompleteMemberCopyDoesNotInitializeItsPointee) {
  const auto contract = checkCase(R"c(
    union value { int number; int *pointer; };
    int main(void) {
      int n;
      union value a = {.pointer = &n};
      union value b = a;
      return *b.pointer;
    }
  )c");
  EXPECT_FALSE(contract.complete());
}

TEST(UnionCases, AddressedPointerWriteEstablishesOnlyItsActualMember) {
  const auto *source = R"c(
    union value { int number; int *pointer; };
    int main(void) {
      int n = 7;
      union value u;
      int **slot = &u.pointer;
      *slot = &n;
      return *u.pointer;
    }
  )c";
  const auto contract = checkCase(source);
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(contract.requirements.empty());
  EXPECT_FALSE(contract.deferred);
  EXPECT_FALSE(contract.limited);
}

TEST(UnionCases, ReplacingTheGuardCannotSelectAStalePayload) {
  const auto contract = checkCase(R"c(
    union value { int number; int *pointer; };
    int main(int argc, char **argv) {
      (void)argv;
      union value u;
      int n = 7;
      int tag = argc > 1;
      if (tag) u.pointer = &n; else u.number = 4;
      tag = 1;
      if (tag) return *u.pointer;
      return 0;
    }
  )c");
  EXPECT_FALSE(contract.complete());
}

TEST(UnionCases, UnreachableUnsupportedOperationIsRecheckedPerCase) {
  AnalysisOptions options;
  options.checkedFunctions = {"main", "read_if"};
  const auto unit = test::analyze(R"c(
    int read_if(int enabled) {
      if (!enabled) return 7;
      __asm__("");
      return 0;
    }
    int main(void) { return read_if(0); }
  )c",
                                  options);
  ASSERT_NE(unit.summary("main"), nullptr);
  ASSERT_NE(unit.summary("read_if"), nullptr);
  EXPECT_TRUE(unit.summary("main")->checked.complete());
  EXPECT_TRUE(unit.summary("main")->checked.requirements.empty());
  EXPECT_FALSE(unit.summary("read_if")->checked.complete());
}

TEST(UnionCases, EntryFreeConstructorKeepsItsInductiveOutputs) {
  const auto contract = checkCase(R"c(
    struct node { unsigned value; struct node *next; };
    static struct node *build(unsigned n) {
      struct node *head = 0;
      for (unsigned i = 0; i < n; ++i) {
        struct node *p = malloc(sizeof *p);
        if (!p) break;
        p->value = i;
        p->next = head;
        head = p;
      }
      return head;
    }
    static void destroy(struct node *p) {
      while (p) { struct node *next = p->next; free(p); p = next; }
    }
    int main(int argc, char **argv) {
      (void)argv;
      struct node *a = build((unsigned)argc), *b = build(2);
      destroy(a);
      destroy(b);
      return 0;
    }
  )c");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(contract.requirements.empty());
}

} // namespace weavec::analysis
