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

using core::PlaceEffect;
using core::SummaryPath;
using core::ValueSource;
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

TEST(UnitExports, ExportsExternalAndAddressTakenDefinitions) {
  const auto unit = analyze(NodeUnit);
  ASSERT_TRUE(unit.ast);
  const UnitExports exports = unit.analyzer->exports();

  EXPECT_EQ(exports.source, "input.c");
  std::vector<std::string> names;
  for (const auto &[name, function] : exports.functions)
    names.push_back(name);
  EXPECT_EQ(names,
            (std::vector<std::string>{"drop_impl", "node_free", "node_name",
                                      "node_new", "node_vp", "reset_caches"}));

  const ExportedFunction &nodeFree = exports.functions.at("node_free");
  EXPECT_TRUE(nodeFree.external);
  EXPECT_FALSE(nodeFree.addressTaken);
  EXPECT_EQ(nodeFree.typeKey, "void (struct node *)");
  EXPECT_TRUE(nodeFree.summary.frees(0));

  const ExportedFunction &drop = exports.functions.at("drop_impl");
  EXPECT_FALSE(drop.external);
  EXPECT_TRUE(drop.addressTaken);
  EXPECT_EQ(drop.typeKey, "void (void *)");
  EXPECT_TRUE(drop.summary.frees(0));

  EXPECT_TRUE(exports.functions.at("node_new")
                  .summary.returns.contains(ValueSource::fresh()));
  EXPECT_TRUE(exports.functions.at("node_name")
                  .summary.returns.contains(ValueSource::copy(
                      SummaryPath::param(0).deref().field("name"))));
  EXPECT_TRUE(exports.functions.at("node_vp").summary.returns.contains(
      ValueSource::borrow(SummaryPath::param(0).deref().field("v"))));

  // Globals travel by name; the static one is dropped.
  const core::FunctionSummary &reset =
      exports.functions.at("reset_caches").summary;
  ASSERT_EQ(exports.globals.size(), 1U);
  EXPECT_EQ(exports.globals.nameOf(0), "g_cache");
  EXPECT_TRUE(reset.effectOf(SummaryPath::global(0)).freed);
  EXPECT_EQ(reset.effects.size(), 1U);

  // libc callees are imports too; they just never resolve to a unit.
  EXPECT_TRUE(exports.imports.contains("malloc"));
  EXPECT_TRUE(exports.imports.contains("free"));
  EXPECT_FALSE(exports.imports.contains("helper"));
  EXPECT_TRUE(exports.indirectTypes.empty());
  EXPECT_TRUE(exports.unknownCallees.empty());
}

TEST(UnitExports, DiscoverySkipsAnalysis) {
  const auto unit = analyze(R"c(
    void other(void *p);
    void (*hook)(int);
    void f(void *p) { other(p); hook(1); }
  )c");
  ASSERT_TRUE(unit.ast);
  core::DiagnosticCollector sink;
  analysis::TranslationUnitAnalyzer fresh(unit.ast->getASTContext(), sink);
  const UnitExports skeleton = fresh.discover();
  EXPECT_TRUE(sink.empty());
  EXPECT_EQ(skeleton.functions.size(), 1U);
  EXPECT_TRUE(skeleton.functions.at("f").summary == core::FunctionSummary{});
  EXPECT_TRUE(skeleton.imports.contains("other"));
  EXPECT_EQ(skeleton.indirectTypes, (std::set<std::string>{"void (int)"}));
}

TEST(UnitExports, RecordsBoundariesAndDefersTheirWarnings) {
  AnalysisOptions options;
  options.deferBoundary = true;
  const auto unit = analyze(R"c(
    void other(void *p);
    void (*hook)(void *);
    void f(void *p) { other(p); hook(p); }
  )c",
                            options);
  ASSERT_TRUE(unit.ast);
  EXPECT_TRUE(unit.diagnostics.empty());
  const UnitExports exports = unit.analyzer->exports();
  EXPECT_EQ(exports.unknownCallees, (std::set<std::string>{"other"}));
  EXPECT_EQ(exports.unknownIndirectTypes,
            (std::set<std::string>{"void (void *)"}));
  EXPECT_FALSE(exports.sameSummariesAs(UnitExports{}));
  EXPECT_TRUE(exports.sameSummariesAs(exports));
}

TEST(ProgramDatabase, JoinsDuplicateDefinitions) {
  const auto a = analyze("void f(void *p) { free(p); }");
  const auto b = analyze("void f(void *p) { *(char *)p = 1; }");
  ASSERT_TRUE(a.ast && b.ast);
  ProgramDatabase db;
  db.add(a.analyzer->exports());
  db.add(b.analyzer->exports());
  ASSERT_TRUE(db.defines("f"));
  const core::FunctionSummary *joined = db.find("f");
  ASSERT_NE(joined, nullptr);
  EXPECT_TRUE(joined->frees(0));
  EXPECT_TRUE(joined->effectOf(SummaryPath::param(0).deref()).written);
  EXPECT_FALSE(db.defines("g"));
  EXPECT_EQ(db.find("g"), nullptr);
}

/// The node unit's exports, kept alive for the database.
struct NodeProgram {
  test::AnalysisResult unit = analyze(NodeUnit);
  ProgramDatabase db;
  NodeProgram() {
    if (unit.ast)
      db.add(unit.analyzer->exports());
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

TEST(ProgramDatabase, CalleeDefinedInAnotherUnitIsChecked) {
  NodeProgram program;
  ASSERT_TRUE(program.unit.ast);
  const auto client = analyzeInProgram(std::string(ClientHeader) + R"c(
    int uaf(void) {
      struct node *n = node_new(1);
      node_free(n);
      return n->v;
    }
    int twice(void) {
      struct node *n = node_new(1);
      node_free(n);
      free(n);
      return 0;
    }
    int dangling(void) {
      struct node *n = node_new(1);
      int *p = node_vp(n);
      node_free(n);
      return *p;
    }
    char *copied_out(void) {
      struct node *n = node_new(1);
      char *s = node_name(n);
      node_free(n);
      return s;
    }
  )c",
                                       &program.db);
  ASSERT_TRUE(client.ast);
  EXPECT_EQ(ids(client.diagnostics),
            (std::vector<std::string>{"use-after-free", "double-free",
                                      "conflicting-borrow"}));
  EXPECT_EQ(messages(client.diagnostics)[0],
            "14: use of 'n' after it was freed");
  EXPECT_EQ(messages(client.diagnostics)[2],
            "25: cannot free 'n' while it is borrowed")
      << "`s` in copied_out was copied out before the free: no report";

  const auto resolved =
      client.analyzer->summaries().lookup(*client.function("node_free"));
  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->source, SummarySource::Program);
  EXPECT_TRUE(resolved->summary->frees(0));
}

TEST(ProgramDatabase, NoBoundaryWarningForProgramDefinitions) {
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
  // Alone: node_new, other and node_free are all boundaries.
  EXPECT_EQ(alone.diagnostics.size(), 3U);
  const auto together = analyzeInProgram(code, &program.db);
  ASSERT_TRUE(together.ast);
  ASSERT_EQ(ids(together.diagnostics),
            (std::vector<std::string>{"annotation-required"}));
  EXPECT_EQ(messages(together.diagnostics)[0],
            "14: call to 'other' is not checked: it has no definition or "
            "ownership annotations here");
  EXPECT_EQ(test::notes(together.diagnostics)[1],
            "annotate its pointer parameters with WEAVEC_OWNED, "
            "WEAVEC_BORROWED, WEAVEC_MUT or WEAVEC_RAW, or define it in this "
            "program");
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
  EXPECT_EQ(ids(client.diagnostics),
            (std::vector<std::string>{"use-after-free"}));
}

TEST(ProgramDatabase, GlobalsAreMatchedByNameOrDropped) {
  NodeProgram program;
  ASSERT_TRUE(program.unit.ast);
  const auto declared = analyzeInProgram(std::string(ClientHeader) + R"c(
    int f(void) {
      g_cache = malloc(4);
      reset_caches();
      return g_cache[0];
    }
  )c",
                                         &program.db);
  ASSERT_TRUE(declared.ast);
  EXPECT_EQ(ids(declared.diagnostics),
            (std::vector<std::string>{"use-after-free"}));

  const auto freeOnly = analyzeInProgram(R"c(
    extern char *g_cache;
    void reset_caches(void);
    void bump(void);
    int f(void) {
      char *keep = g_cache;
      reset_caches();
      return keep[0];
    }
  )c",
                                         &program.db);
  ASSERT_TRUE(freeOnly.ast);
  EXPECT_EQ(ids(freeOnly.diagnostics),
            (std::vector<std::string>{"use-after-free"}));

  // A unit that never names the global cannot observe the effect.
  const auto undeclared = analyzeInProgram(R"c(
    void reset_caches(void);
    int f(void) { reset_caches(); return 0; }
  )c",
                                           &program.db);
  ASSERT_TRUE(undeclared.ast);
  EXPECT_TRUE(undeclared.diagnostics.empty());
  const auto resolved = undeclared.analyzer->summaries().lookup(
      *undeclared.function("reset_caches"));
  ASSERT_TRUE(resolved);
  EXPECT_TRUE(resolved->summary->effects.empty());
  EXPECT_TRUE(resolved->summary->stores.empty());
}

TEST(ProgramDatabase, ProgramDefinitionOutranksTheLibraryTable) {
  const auto lib = analyze(R"c(
    char *strdup(const char *s) { return (char *)s; }
  )c");
  ASSERT_TRUE(lib.ast);
  ProgramDatabase db;
  db.add(lib.analyzer->exports());

  const std::string code = R"c(
    char *strdup(const char *s);
    int f(void) {
      char *s = malloc(4);
      char *c = strdup(s);
      free(s);
      return c[0];
    }
  )c";
  const auto alone = analyze(code);
  ASSERT_TRUE(alone.ast);
  EXPECT_TRUE(alone.diagnostics.empty());
  const auto together = analyzeInProgram(code, &db);
  ASSERT_TRUE(together.ast);
  EXPECT_EQ(ids(together.diagnostics),
            (std::vector<std::string>{"use-after-free"}));
  const auto resolved =
      together.analyzer->summaries().lookup(*together.function("strdup"));
  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->source, SummarySource::Program);
}

TEST(ProgramDatabase, LocalDefinitionAndAnnotationsOutrankTheProgram) {
  NodeProgram program;
  ASSERT_TRUE(program.unit.ast);
  // A static `node_free` here shadows the program's.
  const auto local = analyzeInProgram(R"c(
    struct node { int v; char *name; };
    struct node *node_new(int v);
    static void node_free(struct node *n) { (void)n; }
    int f(void) {
      struct node *n = node_new(1);
      node_free(n);
      return n->v;
    }
  )c",
                                      &program.db);
  ASSERT_TRUE(local.ast);
  EXPECT_TRUE(local.diagnostics.empty());
  EXPECT_EQ(
      local.analyzer->summaries().lookup(*local.function("node_free"))->source,
      SummarySource::Inferred);

  // An annotation on the declaration is authoritative over the body
  // elsewhere (RFC 0003); the mismatch is reported where the body is.
  const auto annotated = analyzeInProgram(R"c(
    struct node { int v; char *name; };
    struct node *node_new(int v);
    void node_free(struct node *BORROWED n);
    int f(void) {
      struct node *n = node_new(1);
      node_free(n);
      return n->v;
    }
  )c",
                                          &program.db);
  ASSERT_TRUE(annotated.ast);
  EXPECT_TRUE(annotated.diagnostics.empty());
  EXPECT_EQ(annotated.analyzer->summaries()
                .lookup(*annotated.function("node_free"))
                ->source,
            SummarySource::Annotation);
}

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
  clang::ASTContext &ctx = unit.ast->getASTContext();
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
  EXPECT_NE(text.find("  function 'node_free': param 0: freed; stores{} "
                      "returns{}\n"),
            std::string::npos);
  EXPECT_NE(text.find("  function 'reset_caches': global g_cache: freed; "
                      "stores{} returns{}\n"),
            std::string::npos);
  EXPECT_NE(text.find("  function 'node_vp': stores{} "
                      "returns{borrow param 0 *.v}\n"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("  candidate 'void (void *)': param 0: freed; stores{} "
                      "returns{}\n"),
            std::string::npos);
}

} // namespace
} // namespace weavec::analysis
