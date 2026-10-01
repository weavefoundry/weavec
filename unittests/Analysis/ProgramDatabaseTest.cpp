//===- ProgramDatabaseTest.cpp - Tests for cross-unit summaries -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0005: what a unit exports, how the database joins units, and how a
// unit analysed with the database sees the others' definitions.
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/ProgramDatabase.h"

#include "TestUtils.h"

#include "llvm/ADT/STLExtras.h"

#include <gtest/gtest.h>

namespace weavec::analysis {
namespace {

using core::SummaryPath;
using test::analyze;
using test::analyzeInProgram;
using test::ids;
using test::messages;

constexpr const char *NodeUnit = R"c(
struct node { int v; char *name; };
char *g_cache;
static char *s_cache;
struct node *node_new(int v) {
  struct node *n = malloc(sizeof *n);
  n->v = v;
  return n;
}
void node_free(struct node *n) { free(n); }
char *node_name(struct node *n) { return n->name; }
int *node_vp(struct node *n) { return &n->v; }
static void drop_impl(void *p) { free(p); }
void (*drop_hook)(void *) = drop_impl;
void reset_caches(void) { free(g_cache); free(s_cache); }
static void helper(void) {}
int main(void) { helper(); return 0; }
)c";

/// The node unit's exports, kept alive for the database.
struct NodeProgram {
  test::AnalysisResult unit = analyze(NodeUnit);
  ProgramDatabase db;
  NodeProgram() {
    if (!unit.ast)
      return;
    db.add(unit.exports);
    // RFC 0030 §9.3, §13.2 step 2: at link the slots are solved over every
    // unit's constraints, and the client reads the solution through the
    // program database. The program is not closed here (the client's own
    // record is not among them), so its slots stay open.
    auto facts = std::make_shared<analysis::ProgramFacts>();
    core::SlotRules rules = unit.harness->kinds->slots.rules();
    rules.scope = core::SlotScope::Link;
    facts->slots = unit.harness->kinds->slots.exported().solve(rules);
    db.programFacts = std::move(facts);
  }
};

constexpr const char *ClientHeader = R"c(
struct node { int v; char *name; };
extern char *g_cache;
struct node *node_new(int v);
void node_free(struct node *n);
char *node_name(struct node *n);
int *node_vp(struct node *n);
extern void (*drop_hook)(void *);
void reset_caches(void);
)c";

TEST(ProgramDatabase, ProgramDefinitionsAreNotUnknownCallees) {
  // RFC 0030 §5.1: in a per-unit analysis the calls into other units are
  // unknown callees; with the program's database only the callee no unit
  // defines stays one. None of them is a diagnostic.
  NodeProgram program;
  ASSERT_TRUE(program.unit.ast);
  const std::string code = std::string(ClientHeader) + R"c(
    void other(void *p);
    int ok(void) {
      struct node *n = node_new(1);
      other(n);
      node_free(n);
      return 0;
    }
  )c";
  const auto alone = analyze(code);
  ASSERT_TRUE(alone.ast);
  EXPECT_TRUE(alone.diagnostics.empty());
  EXPECT_EQ(test::unknownCalls(alone).size(), 3U);
  const auto together = analyzeInProgram(code, &program.db);
  ASSERT_TRUE(together.ast);
  EXPECT_TRUE(together.diagnostics.empty());
  // `other` may have released `n`, so the known release after it is
  // unresolved too (§3.1, *A known release after an unknown one*).
  EXPECT_EQ(test::unknownCalls(together),
            (std::vector<std::string>{"14: other(n)", "15: node_free(n)"}));
}

TEST(ProgramDatabase, IndirectCandidatesFromOtherUnits) {
  NodeProgram program;
  ASSERT_TRUE(program.unit.ast);
  const auto client = analyzeInProgram(std::string(ClientHeader) + R"c(
    int hook(void) {
      char *p = malloc(8);
      drop_hook(p);
      return p[0];
    }
  )c",
                                       &program.db);
  ASSERT_TRUE(client.ast);
  // (`p` is used without a null test: RFC 0030 §3.2's off-by-default
  // `allocation-failure`, which the engine reports and the frontend drops.)
  EXPECT_EQ(ids(client.diagnostics),
            (std::vector<std::string>{"allocation-failure", "use-after-free"}));
}

// RFC 0020: rebuilding already-numbered contexts preserves all their facts.
TEST(ProgramDatabase, TypeKeysIgnoreTypedefsAndRejectAnonymousRecords) {
  const auto unit = analyze(R"c(
    typedef struct node node_t;
    typedef void (*cb_t)(node_t *, const char *);
    static void a(node_t *n, const char *s) { (void)n; (void)s; }
    cb_t table[] = { a };
    struct { int x; } anon;
    static void b(int *p) { (void)p; }
    void (*bp)(int *) = b;
  )c");
  ASSERT_TRUE(unit.ast);
  const clang::ASTContext &ctx = unit.ast->getASTContext();
  EXPECT_EQ(functionTypeKey(unit.function("a")->getType(), ctx),
            "void (struct node *, const char *)");
  EXPECT_EQ(functionTypeKey(unit.function("b")->getType(), ctx),
            "void (int *)");
  const auto anon = analyze(R"c(
    static void c(struct { int x; } *p) { (void)p; }
    void (*cp)(void *) = (void (*)(void *))c;
  )c");
  ASSERT_TRUE(anon.ast);
  EXPECT_EQ(
      functionTypeKey(anon.function("c")->getType(), anon.ast->getASTContext()),
      "");
}

TEST(ProgramDatabase, DumpListsFunctionsAndCandidates) {
  NodeProgram program;
  ASSERT_TRUE(program.unit.ast);
  std::string text;
  llvm::raw_string_ostream os(text);
  program.db.dump(os);
  EXPECT_NE(text.find("program:\n"), std::string::npos);
  EXPECT_NE(text.find("  function 'node_free':\n"), std::string::npos) << text;
  EXPECT_NE(text.find("release *param0 free when always"), std::string::npos)
      << text;
  EXPECT_NE(text.find("  candidate 'void (void *)':\n"), std::string::npos)
      << text;
  EXPECT_NE(text.find("'g_cache'"), std::string::npos) << text;
}

TEST(ProgramDatabase, SeveralDefinitionsOfANameJoin) {
  // RFC 0005, *Accepted false positives*: a name two units define gets the
  // join of both definitions; what only one does becomes possible.
  const auto first = analyze("void f(char *p) { free(p); }");
  const auto second = analyze("void f(char *p) { (void)p; }");
  ASSERT_TRUE(first.ast);
  ASSERT_TRUE(second.ast);
  ProgramDatabase db;
  db.add(first.exports);
  db.add(second.exports);
  const core::FunctionEffects *joined = db.findEffects("f");
  ASSERT_NE(joined, nullptr);
  ASSERT_EQ(joined->effects.size(), 1U);
  EXPECT_EQ(joined->effects[0].kind, core::PathEffect::Kind::Release);
  EXPECT_TRUE(joined->effects[0].may);
}

TEST(ProgramDatabase, GlobalsAreNumberedByName) {
  // Each unit numbers its globals; the database renumbers them by name, so
  // the same global has one id whichever unit mentions it first.
  const auto first = analyze(R"c(
    char *a, *b;
    void f(void) { free(b); }
  )c");
  const auto second = analyze(R"c(
    char *b;
    void g(void) { free(b); }
  )c");
  ASSERT_TRUE(first.ast);
  ASSERT_TRUE(second.ast);
  ProgramDatabase db;
  db.add(first.exports);
  db.add(second.exports);
  const core::FunctionEffects *f = db.findEffects("f");
  const core::FunctionEffects *g = db.findEffects("g");
  ASSERT_NE(f, nullptr);
  ASSERT_NE(g, nullptr);
  ASSERT_FALSE(f->effects.empty());
  ASSERT_FALSE(g->effects.empty());
  EXPECT_EQ(f->effects[0].path, g->effects[0].path);
  EXPECT_EQ(db.globals().nameOf(f->effects[0].path.index), "b");
}

} // namespace
} // namespace weavec::analysis
