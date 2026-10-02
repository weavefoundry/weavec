//===- SharedOwnershipTest.cpp - Reference counts, per-outcome stores -----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0010: shared ownership. Each test names the RFC section it pins.
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"
#include "weavec/Analysis/ProgramDatabase.h"

#include <gtest/gtest.h>

namespace weavec::analysis {
namespace {

using core::SummaryPath;
using test::analyze;
using test::analyzeInProgram;
using test::ids;
using test::messages;
using test::notes;

using Strings = std::vector<std::string>;

// -- Per-outcome stores (RFC 0010, *Per-outcome stores*) ----------------------

TEST(SharedOwnership, ARetractedStoreLeavesTheResourceWithTheCaller) {
  const auto result = analyze(R"c(
    int put(char **slot, char *s, int room) {
      if (!room) return -1;
      *slot = s;
      return 0;
    }
    void retracted(char **slot, int room) {
      char *s = malloc(8);
      if (put(slot, s, room) < 0) {
        return;
      }
    }
    void kept(char **slot, int room) {
      char *s = malloc(8);
      if (put(slot, s, room) == 0)
        return;
      use(s);
    }
  )c");
  ASSERT_TRUE(result.ast);
  // `retracted`: on the failure edge the store did not happen, so `s` is
  // still this function's and is lost at the return. `kept`: on the failure
  // edge it is lost at the end.
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"10: 's' is leaked", "17: 's' is leaked"}));
}

// -- Annotations (RFC 0010, *Annotations*) -----------------------------------

TEST(SharedOwnership, AnnotatedRetainAndRelease) {
  const auto result = analyze(R"c(
    struct obj;
    struct obj *g_new(void) OWNED;
    struct obj *g_ref(struct obj *RETAINS o);
    void g_unref(struct obj *RELEASES o);
    int balanced(void) {
      struct obj *a = g_new();
      if (!a) return -1;
      struct obj *b = g_ref(a);
      g_unref(b);
      g_unref(a);
      return 0;
    }
    int twice(void) {
      struct obj *a = g_new();
      if (!a) return -1;
      g_unref(a);
      g_unref(a);
      return 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"18: 'a' is released twice"}))
      << "the result of a returning ref is a copy of its argument";
}

TEST(SharedOwnership, OwnedByNamesTheFamily) {
  const auto result = analyze(R"c(
    struct obj;
    void obj_destroy(struct obj *OWNED o);
    struct obj *obj_make(void) OWNED OWNED_BY(obj_destroy);
    int right(void) {
      struct obj *o = obj_make();
      if (!o) return -1;
      obj_destroy(o);
      return 0;
    }
    int wrong(void) {
      struct obj *o = obj_make();
      if (!o) return -1;
      free(o);
      return 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), (Strings{"mismatched-release"}));
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"14: 'o' is released with 'free' but must be released "
                     "with 'obj_destroy'"}));
}

TEST(SharedOwnership, InvalidShareAnnotations) {
  const auto result = analyze(R"c(
    struct obj;
    void both(struct obj *RETAINS RELEASES o) { use(o); }
    struct obj *family_alone(void) OWNED_BY(obj_destroy) { return NULL; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"invalid-annotation", "invalid-annotation"}));
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"3: 'o' is declared both WEAVEC_RETAINS and WEAVEC_RELEASES",
               "4: 'family_alone' is declared WEAVEC_OWNED_BY(obj_destroy) "
               "without WEAVEC_OWNED"}));
}

// -- Whole program (RFC 0010, *Across translation units*) ---------------------

} // namespace
} // namespace weavec::analysis
