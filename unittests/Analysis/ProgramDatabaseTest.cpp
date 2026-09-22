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
  names.reserve(exports.functions.size());
  for (const auto &[name, function] : exports.functions)
    names.push_back(name);
  EXPECT_EQ(names,
            (std::vector<std::string>{"drop_impl", "node_free", "node_name",
                                      "node_new", "node_vp", "reset_caches"}));

  const ExportedFunction &nodeFree = exports.functions.at("node_free");
  EXPECT_TRUE(nodeFree.external);
  EXPECT_FALSE(nodeFree.addressTaken);
  EXPECT_EQ(nodeFree.typeKey, "void (struct node *)");
  EXPECT_TRUE(nodeFree.summary.get().frees(0));

  const ExportedFunction &drop = exports.functions.at("drop_impl");
  EXPECT_FALSE(drop.external);
  EXPECT_TRUE(drop.addressTaken);
  EXPECT_EQ(drop.typeKey, "void (void *)");
  EXPECT_TRUE(drop.summary.get().frees(0));

  EXPECT_TRUE(
      llvm::any_of(exports.functions.at("node_new").summary.get().returns,
                   [](const ValueSource &source) {
                     return source.isFresh() && source.family == "free";
                   }));
  EXPECT_TRUE(exports.functions.at("node_name")
                  .summary.get()
                  .returns.contains(ValueSource::copy(
                      SummaryPath::param(0).deref().field("name"))));
  EXPECT_TRUE(exports.functions.at("node_vp").summary.get().returns.contains(
      ValueSource::copyAt(SummaryPath::param(0),
                          core::PointerOffset::ofField("struct node.v"))));

  // RFC 0028: the static cell travels under its declaration identity.
  const core::FunctionSummary &reset =
      exports.functions.at("reset_caches").summary.get();
  ASSERT_EQ(exports.globals.size(), 2U);
  EXPECT_EQ(exports.globals.nameOf(0), "g_cache");
  EXPECT_TRUE(reset.effectOf(SummaryPath::global(0)).freed);
  EXPECT_EQ(reset.effects.size(), 2U);
  EXPECT_TRUE(exports.globals.nameOf(1).starts_with("@weavec-state:"));
  EXPECT_TRUE(reset.effectOf(SummaryPath::global(1)).freed);

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
  analysis::LedgerAdapter sink(unit.ast->getASTContext(),
                               analysis::LedgerAdapter::Mode::Collecting);
  analysis::TranslationUnitAnalyzer fresh(unit.ast->getASTContext(), sink);
  const UnitExports skeleton = fresh.discover();
  EXPECT_TRUE(sink.diagnostics().empty());
  EXPECT_EQ(skeleton.functions.size(), 1U);
  EXPECT_TRUE(skeleton.functions.at("f").summary.get() ==
              core::FunctionSummary{});
  EXPECT_TRUE(skeleton.imports.contains("other"));
  EXPECT_EQ(skeleton.indirectTypes, (std::set<std::string>{"void (int)"}));
}

TEST(UnitExports, RecordsBoundaries) {
  // RFC 0030 §5.1: the calls into code the unit cannot see are recorded for
  // the link step, and report nothing (they are ledger rows).
  const auto unit = analyze(R"c(
    void other(void *p);
    void (*hook)(void *);
    void f(void *p) { other(p); hook(p); }
  )c");
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

TEST(ProgramDatabase, SharedPublicationsDetachOnDefinitionAndCandidateJoins) {
  // RFC 0020: a new database generation cannot mutate a retained contract,
  // another lookup index, or a copy of the previous database.
  UnitExports first;
  auto &original = first.functions["f"];
  original.typeKey = "void (void *)";
  original.addressTaken = true;
  core::FunctionSummary originalSummary;
  originalSummary.effects[SummaryPath::param(0)].freed = true;
  originalSummary.neverReturns = true;
  original.summary.assign(std::move(originalSummary));
  ProgramDatabase db;
  db.add(first);
  ProgramDatabase retained = db;
  const auto *old = retained.findCallable("f");
  ASSERT_NE(old, nullptr);
  const auto expected = *old;

  UnitExports second;
  auto &replacement = second.functions["f"];
  replacement.typeKey = original.typeKey;
  replacement.addressTaken = true;
  core::FunctionSummary replacementSummary;
  replacementSummary.addEffect(SummaryPath::param(0).deref(),
                               {.written = true});
  replacement.summary.assign(std::move(replacementSummary));
  db.add(second);
  ASSERT_NE(db.find("f"), nullptr);
  EXPECT_TRUE(db.find("f")->frees(0));
  EXPECT_TRUE(db.find("f")->effectOf(SummaryPath::param(0).deref()).written);
  EXPECT_FALSE(db.find("f")->neverReturns);
  ASSERT_NE(db.findCallable("f"), nullptr);
  EXPECT_TRUE(*db.findCallable("f") == replacement.summary.get());
  ASSERT_NE(db.candidates(original.typeKey), nullptr);
  EXPECT_TRUE(*db.candidates(original.typeKey) == *db.find("f"));
  EXPECT_TRUE(*old == expected);
  EXPECT_TRUE(*retained.find("f") == expected);
  EXPECT_TRUE(*retained.candidates(original.typeKey) == expected);

  UnitExports third;
  auto &candidate = third.functions["g"];
  candidate.typeKey = original.typeKey;
  candidate.addressTaken = true;
  core::FunctionSummary candidateSummary;
  candidateSummary.addEffect(SummaryPath::param(1), {.freed = true});
  candidate.summary.assign(std::move(candidateSummary));
  db.add(third);
  EXPECT_TRUE(db.candidates(original.typeKey)->frees(1));
  EXPECT_FALSE(db.find("f")->frees(1));
  EXPECT_FALSE(db.findCallable("f")->frees(1));
  db.clear();
  EXPECT_TRUE(*old == expected);
  EXPECT_TRUE(*retained.find("f") == expected);
}

TEST(ExportedSummary,
     CopiesKeepTheirValuesAfterReplacementAndOwnerDestruction) {
  ExportedSummary retained;
  {
    core::FunctionSummary value;
    value.addEffect(SummaryPath::param(0), {.freed = true});
    ExportedSummary original(std::move(value));
    retained = original;
    EXPECT_EQ(retained.share(), original.share());
    const ExportedSummary equal(original.get());
    EXPECT_EQ(equal, original);
    EXPECT_NE(equal.share(), original.share());
    auto changed = original.get();
    changed.addEffect(SummaryPath::param(1), {.written = true});
    original.assign(std::move(changed));
    EXPECT_NE(retained, original);
    EXPECT_FALSE(retained.get().effectOf(SummaryPath::param(1)).written);
    EXPECT_TRUE(original.get().effectOf(SummaryPath::param(1)).written);
  }
  EXPECT_TRUE(retained.get().frees(0));
}

TEST(ProgramDatabase, ExportIndexesReuseTheImmutablePublication) {
  UnitExports exports;
  core::FunctionSummary value;
  value.addEffect(SummaryPath::param(0), {.freed = true});
  auto &function = exports.functions["release"];
  function.summary.assign(std::move(value));
  function.typeKey = "void (void *)";
  function.addressTaken = true;
  const UnitExports retained = exports;
  const auto owner = function.summary.share();
  ProgramDatabase database;
  database.add(exports);
  EXPECT_EQ(database.find("release"), owner.get());
  EXPECT_EQ(database.findCallable("release"), owner.get());
  EXPECT_EQ(database.candidates(function.typeKey), owner.get());
  function.summary.assign({});
  exports = {};
  EXPECT_TRUE(database.find("release")->frees(0));
  EXPECT_TRUE(retained.functions.at("release").summary.get().frees(0));
}

TEST(ProgramDatabase, UnchangedRemappedValuesRetainPublicationsUnderNewKeys) {
  UnitExports prefix;
  (void)prefix.globals.idFor("other");
  (void)prefix.globals.idFor("shared");
  UnitExports unit;
  (void)unit.globals.idFor("shared");
  (void)unit.globals.idFor("other");
  core::FunctionSummary value;
  value.addEffect(SummaryPath::param(0), {.freed = true});
  ExportedSummary publication(std::move(value));
  core::CallContext input;
  input.facts[SummaryPath::global(0)] = core::ValueFact::ofConstant(7);
  const core::CallbackBindings callbacks{
      {SummaryPath::global(0), core::CallTargets::function("drop")}};
  auto &function = unit.functions["release"];
  function.summary = publication;
  function.memorySpecializations[input] = publication;
  function.specializations[callbacks] = publication;
  ProgramDatabase database;
  database.add(prefix);
  const auto numbered = database.renumbered(unit);
  const auto &mapped = numbered.functions.at("release");
  EXPECT_EQ(mapped.summary.share(), publication.share());
  ASSERT_EQ(mapped.memorySpecializations.size(), 1U);
  const auto &[mappedInput, memory] = *mapped.memorySpecializations.begin();
  EXPECT_TRUE(mappedInput.facts.contains(SummaryPath::global(1)));
  EXPECT_EQ(memory.share(), publication.share());
  ASSERT_EQ(mapped.specializations.size(), 1U);
  EXPECT_TRUE(
      mapped.specializations.begin()->first.contains(SummaryPath::global(1)));
  EXPECT_EQ(mapped.specializations.begin()->second.share(),
            publication.share());
  EXPECT_TRUE(
      unit.functions.at("release").memorySpecializations.contains(input));
}

TEST(ProgramDatabase, CandidateGroupJoinsPreserveIndividualPublications) {
  // RFC 0020: joining several targets in one add preserves every target's
  // effects and leaves both individual contracts and older groups unchanged.
  UnitExports first;
  for (unsigned index = 0; index < 3; ++index) {
    auto &function = first.functions["f" + std::to_string(index)];
    function.typeKey = "void (void *, void *, void *)";
    function.addressTaken = true;
    core::FunctionSummary summary;
    summary.addEffect(SummaryPath::param(index), {.freed = true});
    summary.neverReturns = index != 1;
    function.summary.assign(std::move(summary));
  }
  ProgramDatabase db;
  db.add(first);
  const auto type = first.functions.begin()->second.typeKey;
  ProgramDatabase retained = db;
  UnitExports second;
  for (unsigned index = 0; index < 3; ++index) {
    auto &function = second.functions["g" + std::to_string(index)];
    function.typeKey = type;
    function.addressTaken = true;
    core::FunctionSummary summary;
    summary.addEffect(SummaryPath::param(index).deref(), {.written = true});
    summary.neverReturns = true;
    function.summary.assign(std::move(summary));
  }
  db.add(second);
  ASSERT_NE(db.candidates(type), nullptr);
  EXPECT_FALSE(db.candidates(type)->neverReturns);
  for (unsigned index = 0; index < 3; ++index) {
    EXPECT_TRUE(db.candidates(type)->frees(index));
    EXPECT_TRUE(db.candidates(type)
                    ->effectOf(SummaryPath::param(index).deref())
                    .written);
    EXPECT_FALSE(retained.candidates(type)
                     ->effectOf(SummaryPath::param(index).deref())
                     .written);
    const auto name = "f" + std::to_string(index);
    ASSERT_NE(db.find(name), nullptr);
    EXPECT_TRUE(*db.find(name) == first.functions.at(name).summary.get());
    EXPECT_TRUE(*db.findCallable(name) ==
                first.functions.at(name).summary.get());
  }
}

TEST(ProgramDatabase, SharedIndexesRetainRenumberedGlobalEffects) {
  // RFC 0005/0020: sharing the callable publication must use its database
  // numbering, including internal address-taken candidates and checkpoints.
  UnitExports first;
  (void)first.globals.idFor("first");
  (void)first.globals.idFor("second");
  ProgramDatabase db;
  db.add(first);
  UnitExports reordered;
  reordered.source = "other.c";
  const auto second = reordered.globals.idFor("second");
  (void)reordered.globals.idFor("first");
  auto &external = reordered.functions["f"];
  external.typeKey = "void (void)";
  external.addressTaken = true;
  core::FunctionSummary externalSummary;
  externalSummary.addEffect(SummaryPath::global(second), {.freed = true});
  external.summary.assign(std::move(externalSummary));
  auto &internal = reordered.functions["g"];
  internal = external;
  internal.external = false;
  db.add(reordered);
  const auto expected = SummaryPath::global(*db.globals().find("second"));
  ASSERT_NE(db.find("f"), nullptr);
  ASSERT_NE(db.findCallable("other.c#g"), nullptr);
  ASSERT_NE(db.candidates(external.typeKey), nullptr);
  for (const auto *summary :
       {db.find("f"), db.findCallable("f"), db.findCallable("other.c#g"),
        db.candidates(external.typeKey)}) {
    EXPECT_EQ(summary->effects.size(), 1U);
    EXPECT_TRUE(summary->effectOf(expected).freed);
  }
}

/// The node unit's exports, kept alive for the database.
struct NodeProgram {
  test::AnalysisResult unit = analyze(NodeUnit);
  ProgramDatabase db;
  NodeProgram() {
    if (!unit.ast)
      return;
    db.add(unit.analyzer->exports());
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
  // `node_vp` returns a copy of `n` at the field `v` (RFC 0011): freeing `n`
  // frees what `p` points to, reported at the use.
  EXPECT_EQ(ids(client.diagnostics),
            (std::vector<std::string>{"use-after-free", "double-free",
                                      "use-after-free"}));
  EXPECT_EQ(messages(client.diagnostics)[0],
            "14: use of 'n' after it was freed");
  EXPECT_EQ(messages(client.diagnostics)[2],
            "26: use of 'p' after it was freed")
      << "`s` in copied_out was copied out before the free: no report";

  const auto resolved =
      client.analyzer->summaries().lookup(*client.function("node_free"));
  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->source, SummarySource::Program);
  EXPECT_TRUE(resolved->summary->frees(0));
}

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

  // Undeclared public storage is dropped; private storage has an adapter.
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
  ASSERT_EQ(resolved->summary->effects.size(), 1U);
  const auto &[path, effect] = *resolved->summary->effects.begin();
  ASSERT_TRUE(path.isGlobal());
  EXPECT_TRUE(effect.freed);
  EXPECT_TRUE(undeclared.analyzer->summaries()
                  .globals()
                  .portableName(path.index)
                  ->starts_with("@weavec-state:"));
  EXPECT_TRUE(resolved->summary->stores.empty());
}

TEST(GlobalNames, ExtendToAcceptsPrefixesOnly) {
  GlobalNames a;
  (void)a.idFor("x");
  (void)a.idFor("y");
  GlobalNames b = a;
  (void)b.idFor("z");
  GlobalNames c;
  (void)c.idFor("y");

  EXPECT_TRUE(a.extendTo(b));
  EXPECT_EQ(a, b);
  EXPECT_TRUE(b.extendTo(a));
  EXPECT_EQ(b.size(), 3U);
  EXPECT_FALSE(b.extendTo(c));
  EXPECT_EQ(b.size(), 3U);
  EXPECT_EQ(b.nameOf(1), "y");
  GlobalNames empty;
  EXPECT_TRUE(empty.extendTo(c));
  EXPECT_EQ(empty, c);
}

TEST(ProgramDatabase, RenumberedExportsMeanTheSameVerbatim) {
  // The two units number the shared globals in opposite orders.
  const auto a = analyze(R"c(
    char *g_a; char *g_b;
    void fa(void) { free(g_a); free(g_b); }
  )c");
  const auto b = analyze(R"c(
    char *g_b; char *g_a;
    void fb(void) { free(g_b); free(g_a); }
  )c");
  ASSERT_TRUE(a.ast && b.ast);
  const UnitExports ea = a.analyzer->exports();
  const UnitExports eb = b.analyzer->exports();
  ASSERT_EQ(ea.globals.nameOf(0), "g_a");
  ASSERT_EQ(eb.globals.nameOf(0), "g_b");

  ProgramDatabase direct;
  direct.add(ea);
  direct.add(eb);

  ProgramDatabase rebuilt;
  rebuilt.add(ea);
  const UnitExports renumbered = rebuilt.renumbered(eb);
  EXPECT_EQ(renumbered.globals, rebuilt.globals());
  EXPECT_FALSE(renumbered.sameSummariesAs(eb));
  rebuilt.add(renumbered);

  EXPECT_EQ(direct.globals(), rebuilt.globals());
  ASSERT_TRUE(direct.find("fb") && rebuilt.find("fb"));
  EXPECT_TRUE(*direct.find("fb") == *rebuilt.find("fb"));
  for (const ProgramDatabase *db : {&direct, &rebuilt}) {
    const core::FunctionSummary &fb = *db->find("fb");
    ASSERT_EQ(fb.effects.size(), 2U);
    for (const auto &[path, effect] : fb.effects) {
      EXPECT_TRUE(path.isGlobal());
      EXPECT_TRUE(effect.freed);
    }
    EXPECT_TRUE(
        fb.effectOf(SummaryPath::global(*db->globals().find("g_a"))).freed);
    EXPECT_TRUE(
        fb.effectOf(SummaryPath::global(*db->globals().find("g_b"))).freed);
  }

  // Exports the database already numbers are taken as they are.
  const UnitExports again = rebuilt.renumbered(renumbered);
  EXPECT_TRUE(again.sameSummariesAs(renumbered));
}

// RFC 0020: rebuilding already-numbered contexts preserves all their facts.
TEST(ProgramDatabase, ContextRebuildAgreesWithGlobalRenumbering) {
  UnitExports prefix;
  (void)prefix.globals.idFor("a");
  (void)prefix.globals.idFor("b");
  UnitExports unit;
  (void)unit.globals.idFor("b");
  (void)unit.globals.idFor("a");
  auto &function = unit.functions["helper"];
  core::FunctionSummary summary;
  summary.addEffect(SummaryPath::global(0), PlaceEffect{.freed = true});
  function.summary.assign(std::move(summary));
  core::CallContext input;
  input.facts[SummaryPath::global(0)] =
      core::ValueFact::of(core::Outcome::NonNull);
  unit.memoryRequests["helper"].insert(input);
  function.memorySpecializations[input].assign(function.summary.get());
  core::CallbackBindings callbacks;
  callbacks[SummaryPath::param(0)] = core::CallTargets::function("release");
  callbacks[SummaryPath::global(0)] = core::CallTargets::function("allocate");
  unit.callbackRequests["helper"].insert(callbacks);
  function.specializations[callbacks].assign(function.summary.get());
  ProgramDatabase rebuilt;
  rebuilt.add(prefix);
  const auto numbered = rebuilt.renumbered(unit);
  rebuilt.add(numbered);
  ASSERT_EQ(rebuilt.requestsFor("helper").size(), 1U);
  const auto &mappedCallbacks = *rebuilt.requestsFor("helper").begin();
  EXPECT_TRUE(mappedCallbacks.contains(SummaryPath::global(1)));
  ASSERT_NE(rebuilt.findSpecialization("helper", mappedCallbacks), nullptr);
  const auto &mapped = *rebuilt.memoryRequestsFor("helper").begin();
  EXPECT_TRUE(mapped.facts.contains(SummaryPath::global(1)));
  ASSERT_NE(rebuilt.findMemorySpecialization("helper", mapped), nullptr);
  EXPECT_TRUE(rebuilt.findMemorySpecialization("helper", mapped)
                  ->effectOf(SummaryPath::global(1))
                  .freed);

  // Consume both a differently numbered export and an already-numbered one.
  // The move fast path must retain context keys, requests and global effects.
  ProgramDatabase consuming;
  consuming.add(prefix);
  auto moved = consuming.renumbered(UnitExports(unit));
  EXPECT_TRUE(moved.sameSummariesAs(numbered));
  EXPECT_EQ(moved.globals, numbered.globals);
  auto unchanged = consuming.renumbered(std::move(moved));
  EXPECT_TRUE(unchanged.sameSummariesAs(numbered));
  EXPECT_EQ(unchanged.globals, numbered.globals);
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
      char *s = malloc(4); if (!s) return 0;
      char *c = strdup(s);
      free(s);
      use(c);
      return 0;
    }
  )c";
  const auto alone = analyze(code);
  ASSERT_TRUE(alone.ast);
  EXPECT_EQ(ids(alone.diagnostics), (std::vector<std::string>{"leak"}))
      << "the library's strdup returns a fresh copy that is never released";
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
  // No use-after-free: the local `node_free` releases nothing, so the node
  // `node_new` handed out is leaked instead (RFC 0007).
  EXPECT_EQ(ids(local.diagnostics), (std::vector<std::string>{"leak"}));
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
  EXPECT_EQ(ids(annotated.diagnostics), (std::vector<std::string>{"leak"}))
      << "a borrowing node_free leaves the node unreleased";
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
  EXPECT_NE(text.find("  function 'node_free': param 0: freed(free); stores{} "
                      "returns{}\n"),
            std::string::npos);
  EXPECT_NE(text.find("  function 'reset_caches': global g_cache: freed(free); "
                      "global @weavec-state:"),
            std::string::npos);
  EXPECT_NE(
      text.find("  function 'node_vp': stores{} "
                "returns{copy param 0 @+struct~node.v} requires{param 0}\n"),
      std::string::npos)
      << text;
  EXPECT_NE(
      text.find("  candidate 'void (void *)': param 0: freed(free); stores{} "
                "returns{}\n"),
      std::string::npos);
}

} // namespace
} // namespace weavec::analysis
