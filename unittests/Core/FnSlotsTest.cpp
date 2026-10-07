//===- FnSlotsTest.cpp - Tests for RFC 0030 function-pointer slots --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/FnSlots.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::core {

using Targets = std::set<std::string>;
using Arguments = std::vector<std::optional<SlotKey>>;

/// A direct call to `callee`: argument-to-parameter and result-to-receiver
/// copies, as `SlotCollector` records them.
static void addDirectCall(FnSlots &slots, std::string_view callee,
                          const Arguments &arguments,
                          const std::optional<SlotKey> &receiver) {
  for (std::size_t i = 0; i < arguments.size(); ++i)
    if (arguments[i])
      slots.addSubset(
          *arguments[i],
          SlotKey::param(std::string(callee), static_cast<std::uint32_t>(i)));
  if (receiver)
    slots.addSubset(SlotKey::result(std::string(callee)), *receiver);
}

/// An indirect call through `callee`: argument `n` flows into its
/// call-param slot, and its call-result slot flows to the receiver.
static void addIndirectCall(FnSlots &slots, const SlotKey &callee,
                            const Arguments &arguments,
                            const std::optional<SlotKey> &receiver) {
  for (std::size_t i = 0; i < arguments.size(); ++i)
    if (arguments[i])
      slots.addSubset(
          *arguments[i],
          SlotKey::callParam(callee, static_cast<std::uint32_t>(i)));
  if (receiver)
    slots.addSubset(SlotKey::callResult(callee), *receiver);
}

TEST(FnSlots, KeysSpellAndParse) {
  const std::vector<std::pair<SlotKey, std::string>> keys{
      {SlotKey::field("struct global_State", "frealloc"),
       "field struct global_State frealloc"},
      {SlotKey::global("g_handler"), "global g_handler"},
      {SlotKey::staticGlobal("C:/src/a.c", "hook"), "static C:/src/a.c:hook"},
      {SlotKey::param("lua_newstate", 0), "param lua_newstate 0"},
      {SlotKey::param("src/my file.c:l_alloc", 3),
       "param src/my file.c:l_alloc 3"},
      {SlotKey::result("pick"), "result pick"},
      {SlotKey::local("use", "fp"), "local use fp"},
      {SlotKey::callParam(SlotKey::field("struct ops", "reg"), 1),
       "call-param 1 field struct ops reg"},
      {SlotKey::callResult(SlotKey::callResult(SlotKey::local("f", "fp"))),
       "call-result call-result local f fp"},
      {SlotKey::callParam(SlotKey::callParam(SlotKey::global("g"), 2), 0),
       "call-param 0 call-param 2 global g"},
  };
  for (const auto &[key, spelling] : keys) {
    EXPECT_EQ(key.toString(), spelling);
    EXPECT_EQ(SlotKey::parse(spelling), key) << spelling;
  }
  for (const std::string_view bad :
       {"field struct", "param f x", "param f", "call-param x global g",
        "call-param 1", "call-result bogus", "bogus", "global ", "static a.c",
        "static :x", "local f", "result "})
    EXPECT_FALSE(SlotKey::parse(bad)) << bad;
}

TEST(FnSlots, LocalKeys) {
  const SlotKey local = SlotKey::local("f", "fp");
  EXPECT_TRUE(local.isLocal());
  EXPECT_TRUE(SlotKey::callParam(local, 0).isLocal());
  EXPECT_TRUE(SlotKey::callResult(SlotKey::callResult(local)).isLocal());
  EXPECT_FALSE(SlotKey::field("struct s", "cb").isLocal());
  EXPECT_FALSE(
      SlotKey::callParam(SlotKey::field("struct s", "cb"), 0).isLocal());
  EXPECT_EQ(SlotKey::callResult(local).callee(), local);
  EXPECT_FALSE(local.callee());
  EXPECT_EQ(SlotKey::staticGlobal("a.c", "hook").staticName(), "a.c:hook");
}

TEST(FnSlots, CopiesThroughParametersAndResults) {
  // static void a(void), b(void);
  // static fn pick(int c) { return c ? a : b; }
  // static void run(fn cb) { cb(); }
  // void use(void) { fn fp = pick(1); run(fp); }
  FnSlots slots;
  slots.addMember("a", SlotKey::result("pick"));
  slots.addMember("b", SlotKey::result("pick"));
  const SlotKey fp = SlotKey::local("use", "fp");
  addDirectCall(slots, "pick", {}, fp);
  const Arguments runArguments{fp};
  addDirectCall(slots, "run", runArguments, std::nullopt);
  const SlotRules rules{.scope = SlotScope::Unit,
                        .defined = {"a", "b", "pick", "run", "use"},
                        .exported = {"use"}};
  const SlotSolution solution = slots.solve(rules);
  const SlotKey cb = SlotKey::param("run", 0);
  EXPECT_EQ(solution.targets(fp), (Targets{"a", "b"}));
  EXPECT_EQ(solution.targets(cb), (Targets{"a", "b"}));
  EXPECT_FALSE(solution.isOpen(cb));
  const CallResolution call = solution.resolveCall(cb);
  EXPECT_EQ(call.kind, IndirectCallKind::ClosedJoin);
  EXPECT_EQ(call.targets, (std::vector<std::string>{"a", "b"}));
  EXPECT_FALSE(openCallTemporalDecision(call));
  EXPECT_FALSE(solution.escapes("a"));

  // Were `run` exported, callers elsewhere could pass anything.
  SlotRules exported = rules;
  exported.exported.insert("run");
  const CallResolution open = slots.solve(exported).resolveCall(cb);
  EXPECT_EQ(open.kind, IndirectCallKind::OpenKnown);
  ASSERT_TRUE(open.open);
  EXPECT_EQ(open.open->seed, cb);
  EXPECT_EQ(open.open->detail, "values stored by 'run' parameter 0");
}

TEST(FnSlots, DynamicCallsFlowIntoTheTargets) {
  // struct ops { void (*reg)(fn); fn (*get)(void); };   (main-file struct)
  // static fn hook;
  // static void registrar(fn f) { hook = f; }
  // static fn getter(void) { return cb2; }
  // static struct ops table = { registrar, getter };
  // void setup(void) { table.reg(cb); fn h = table.get(); h(); }
  FnSlots slots;
  const SlotKey reg = SlotKey::field("a.c:struct ops", "reg");
  const SlotKey get = SlotKey::field("a.c:struct ops", "get");
  const SlotKey hook = SlotKey::staticGlobal("a.c", "hook");
  const SlotKey h = SlotKey::local("setup", "h");
  slots.addMember("registrar", reg);
  slots.addMember("getter", get);
  slots.addSubset(SlotKey::param("registrar", 0), hook);
  slots.addMember("cb2", SlotKey::result("getter"));
  slots.addMember("cb", SlotKey::callParam(reg, 0));
  addIndirectCall(slots, get, {}, h);
  SlotRules rules{.scope = SlotScope::Unit,
                  .defined = {"registrar", "getter", "cb", "cb2", "setup"},
                  .exported = {"setup"},
                  .confinedRecords = {"a.c:struct ops"}};
  const SlotSolution solution = slots.solve(rules);
  EXPECT_EQ(solution.targets(hook), (Targets{"cb"}));
  EXPECT_EQ(solution.resolveCall(hook).kind, IndirectCallKind::ClosedSingle);
  EXPECT_EQ(solution.targets(h), (Targets{"cb2"}));
  EXPECT_FALSE(solution.isOpen(h));
  EXPECT_GT(solution.steps(), 0U);

  // The same struct declared in a header: its fields are open, so the
  // registrar may be called from elsewhere and `hook` may hold anything.
  rules.confinedRecords.clear();
  const SlotSolution open = slots.solve(rules);
  EXPECT_TRUE(open.escapes("registrar"));
  const CallResolution call = open.resolveCall(hook);
  EXPECT_EQ(call.kind, IndirectCallKind::OpenKnown);
  EXPECT_EQ(call.targets, (std::vector<std::string>{"cb"}));
  ASSERT_TRUE(call.open);
  EXPECT_EQ(call.open->detail,
            "'registrar' may be called by code outside this unit");
}

TEST(FnSlots, OpenSourcesPropagate) {
  // static fn hook;  void set_hook(fn h) { hook = h; }
  // void init(void) { set_hook(my_cb); }  void fire(void) { hook(); }
  FnSlots slots;
  const SlotKey hook = SlotKey::staticGlobal("u.c", "hook");
  slots.addSubset(SlotKey::param("set_hook", 0), hook);
  slots.addMember("my_cb", SlotKey::param("set_hook", 0));
  SlotRules rules{.scope = SlotScope::Unit,
                  .defined = {"set_hook", "init", "fire", "my_cb"},
                  .exported = {"set_hook", "init", "fire"}};
  const CallResolution perUnit = slots.solve(rules).resolveCall(hook);
  EXPECT_EQ(perUnit.kind, IndirectCallKind::OpenKnown);
  ASSERT_TRUE(perUnit.open);
  EXPECT_EQ(perUnit.open->seed, SlotKey::param("set_hook", 0));
  const auto decision = openCallTemporalDecision(perUnit);
  ASSERT_TRUE(decision);
  EXPECT_EQ(*decision, FacetDecision::trustedFor(
                           TrustReason::ExternContract,
                           "values stored by 'set_hook' parameter 0"));

  // In a closed-world program the exported parameter is closed.
  rules.scope = SlotScope::Program;
  EXPECT_EQ(slots.solve(rules).resolveCall(hook).kind,
            IndirectCallKind::ClosedSingle);
}

TEST(FnSlots, OutsideValuesMakeCallsUnknown) {
  // void f(uintptr_t v) { fn fp = (fn)v; g_cb = fp; r = fp(cb); keep = r; }
  FnSlots slots;
  const SlotKey fp = SlotKey::local("f", "fp");
  const SlotKey r = SlotKey::local("f", "r");
  const SlotKey keep = SlotKey::staticGlobal("f.c", "keep");
  slots.addOpen(fp, "integer conversion");
  slots.addSubset(fp, SlotKey::global("g_cb"));
  slots.addMember("cb", SlotKey::callParam(fp, 0));
  addIndirectCall(slots, fp, {}, r);
  slots.addSubset(r, keep);
  // `cb` stores its parameter somewhere, so its parameter slot exists.
  slots.addSubset(SlotKey::param("cb", 0), SlotKey::local("cb", "x"));
  const SlotRules rules{
      .scope = SlotScope::Program, .defined = {"f", "cb"}, .exported = {"f"}};
  const SlotSolution solution = slots.solve(rules);
  const CallResolution call = solution.resolveCall(fp);
  EXPECT_EQ(call.kind, IndirectCallKind::OpenUnknown);
  EXPECT_EQ(openCallTemporalDecision(call),
            FacetDecision::unresolvedFor(UnresolvedReason::Callback,
                                         "integer conversion"));
  EXPECT_TRUE(solution.isOpen(SlotKey::global("g_cb")));
  EXPECT_EQ(solution.openSource(SlotKey::global("g_cb"))->detail,
            "integer conversion");
  EXPECT_EQ(solution.openSource(SlotKey::callResult(fp))->detail,
            "the result of a call through 'local f fp'");
  EXPECT_TRUE(solution.isOpen(keep));
  // `cb` is passed to an unknown function, which may call it with anything.
  EXPECT_TRUE(solution.escapes("cb"));
  EXPECT_TRUE(solution.isOpen(SlotKey::param("cb", 0)));
  EXPECT_TRUE(solution.isOpen(SlotKey::param("cb", 5)));
  EXPECT_EQ(solution.openSource(SlotKey::param("cb", 0))->detail,
            "'cb' may be called by code outside the analysed program");
}

TEST(FnSlots, RulesDecideSlotsNoConstraintMentions) {
  const SlotRules unit{.scope = SlotScope::Unit,
                       .defined = {"f"},
                       .exported = {"f"},
                       .confinedRecords = {"a.c:struct s"},
                       .escapedStatics = {"a.c:leaked"}};
  const SlotSolution solution = FnSlots{}.solve(unit);
  EXPECT_TRUE(solution.isOpen(SlotKey::field("struct hdr", "cb")));
  EXPECT_FALSE(solution.isOpen(SlotKey::field("a.c:struct s", "cb")));
  EXPECT_TRUE(solution.isOpen(SlotKey::global("g")));
  EXPECT_FALSE(solution.isOpen(SlotKey::staticGlobal("a.c", "kept")));
  EXPECT_TRUE(solution.isOpen(SlotKey::staticGlobal("a.c", "leaked")));
  EXPECT_TRUE(solution.isOpen(SlotKey::param("f", 0)));
  EXPECT_FALSE(solution.isOpen(SlotKey::param("g", 0)));
  EXPECT_TRUE(solution.isOpen(SlotKey::result("g")));
  EXPECT_FALSE(solution.isOpen(SlotKey::result("f")));
  EXPECT_FALSE(solution.isOpen(SlotKey::local("f", "x")));
  EXPECT_TRUE(solution.isOpen(SlotKey::callResult(SlotKey::global("g"))));
  EXPECT_EQ(solution.resolveCall(SlotKey::local("f", "x")).kind,
            IndirectCallKind::ClosedEmpty);
  EXPECT_EQ(solution.resolveCall(SlotKey::global("g")).kind,
            IndirectCallKind::OpenUnknown);

  SlotRules link = unit;
  link.scope = SlotScope::Program;
  EXPECT_TRUE(link.closedWorld());
  const SlotSolution closed = FnSlots{}.solve(link);
  EXPECT_FALSE(closed.isOpen(SlotKey::field("struct hdr", "cb")));
  EXPECT_FALSE(closed.isOpen(SlotKey::global("g")));
  EXPECT_FALSE(closed.isOpen(SlotKey::param("f", 0)));
  EXPECT_TRUE(closed.isOpen(SlotKey::staticGlobal("a.c", "leaked")));
  EXPECT_TRUE(closed.isOpen(SlotKey::result("g")));
  SlotRules shared = link;
  shared.executable = false;
  EXPECT_FALSE(shared.closedWorld());
  EXPECT_FALSE(unit.closedWorld());
}

/// Constraints with chains, a cycle, calls through locals and an outside
/// value in a local.
static FnSlots withLocals() {
  FnSlots slots;
  const SlotKey p = SlotKey::local("f", "p");
  const SlotKey q = SlotKey::local("f", "q");
  const SlotKey r = SlotKey::local("f", "r");
  const SlotKey z = SlotKey::local("f", "z");
  const SlotKey cb = SlotKey::field("struct s", "cb");
  slots.addMember("a", p);
  slots.addSubset(p, q);
  slots.addSubset(q, p);
  slots.addSubset(cb, q);
  slots.addSubset(q, SlotKey::global("g1"));
  const Arguments arguments{p};
  addIndirectCall(slots, q, arguments, r);
  slots.addSubset(r, SlotKey::staticGlobal("x.c", "keep"));
  slots.addOpen(z, "load through an unknown pointer");
  slots.addSubset(z, cb);
  slots.addMember("b", SlotKey::callParam(z, 0));
  addIndirectCall(slots, z, {}, SlotKey::staticGlobal("x.c", "out"));
  slots.addMember("c", cb);
  slots.addMember("d", SlotKey::result("a"));
  slots.addMember("e", SlotKey::result("c"));
  // `b` keeps its parameter, so its parameter slot exists.
  slots.addSubset(SlotKey::param("b", 0), SlotKey::staticGlobal("x.c", "b0"));
  return slots;
}

TEST(FnSlots, LocalEliminationKeepsTheSolution) {
  const FnSlots original = withLocals();
  const FnSlots exported = original.withoutLocals();
  for (const SlotConstraint &constraint : exported.constraints()) {
    EXPECT_FALSE(constraint.slot.isLocal()) << constraint.slot.toString();
    EXPECT_FALSE(constraint.kind == SlotConstraint::Kind::Subset &&
                 constraint.from.isLocal())
        << constraint.from.toString();
  }
  const SlotRules rules{.scope = SlotScope::Unit,
                        .defined = {"a", "b", "c", "d", "e", "f"},
                        .exported = {"f"}};
  const SlotSolution before = original.solve(rules);
  const SlotSolution after = exported.solve(rules);
  std::size_t compared = 0;
  for (const SlotKey &slot : before.slots()) {
    if (slot.isLocal())
      continue;
    ++compared;
    EXPECT_EQ(before.targets(slot), after.targets(slot)) << slot.toString();
    EXPECT_EQ(before.isOpen(slot), after.isOpen(slot)) << slot.toString();
  }
  EXPECT_GE(compared, 8U);
  for (const std::string function : {"a", "b", "c", "d", "e", "f"})
    EXPECT_EQ(before.escapes(function), after.escapes(function)) << function;
  EXPECT_EQ(after.targets(SlotKey::staticGlobal("x.c", "keep")),
            (Targets{"d", "e"}));
  EXPECT_EQ(after.targets(SlotKey::param("c", 0)), (Targets{"a", "c"}));
  EXPECT_TRUE(after.isOpen(SlotKey::staticGlobal("x.c", "out")));
  EXPECT_TRUE(after.escapes("b"));
  EXPECT_TRUE(after.isOpen(SlotKey::staticGlobal("x.c", "b0")));
  // A call through a local that holds an outside value is a call of the
  // unknown function.
  EXPECT_EQ(after.targets(SlotKey::param(std::string(UnknownFunction), 0)),
            (Targets{"b"}));
}

TEST(FnSlots, LuaAllocatorIsClosedAtLinkAndOpenPerUnit) {
  // lstate.c:  LUA_API lua_State *lua_newstate(lua_Alloc f, void *ud) {
  //              ... g->frealloc = f; ... }
  // lmem.c:    luaM_realloc_ calls (*g->frealloc)(ud, block, os, ns): a call
  //            through the field with no function-pointer argument or
  //            receiver, so it adds no constraint.
  // lauxlib.c: static void *l_alloc(void *ud, void *p, size_t os, size_t ns);
  //            lua_State *luaL_newstate(void) {
  //              lua_State *L = lua_newstate(l_alloc, NULL); ... }
  const SlotKey frealloc = SlotKey::field("struct global_State", "frealloc");
  const SlotKey allocParam = SlotKey::param("lua_newstate", 0);
  const std::string lAlloc = "lauxlib.c:l_alloc";
  FnSlots lstate;
  lstate.addSubset(allocParam, frealloc);
  FnSlots lauxlib;
  lauxlib.addMember(lAlloc, allocParam);

  // Per TU, lstate.c: the slot is open and empty, so the calls are
  // unresolved(callback), not silent.
  const SlotSolution perUnit = lstate.solve({.scope = SlotScope::Unit,
                                             .defined = {"lua_newstate"},
                                             .exported = {"lua_newstate"}});
  const CallResolution unitCall = perUnit.resolveCall(frealloc);
  EXPECT_EQ(unitCall.kind, IndirectCallKind::OpenUnknown);
  const auto unitDecision = openCallTemporalDecision(unitCall);
  ASSERT_TRUE(unitDecision);
  EXPECT_EQ(unitDecision->unresolved, UnresolvedReason::Callback);
  EXPECT_FALSE(unitDecision->detail.empty());

  // Per TU, lauxlib.c: `lua_newstate` has no body here, so `l_alloc` escapes.
  const SlotSolution auxUnit =
      lauxlib.solve({.scope = SlotScope::Unit,
                     .defined = {lAlloc, "luaL_newstate"},
                     .exported = {"luaL_newstate"}});
  EXPECT_TRUE(auxUnit.escapes(lAlloc));
  EXPECT_EQ(auxUnit.targets(allocParam), (Targets{lAlloc}));

  // Each unit's constraints without locals, as rows, read back and solved
  // together.
  FnSlots program;
  for (const FnSlots *unit : {&lstate, &lauxlib})
    program.merge(FnSlots::fromRows(unit->withoutLocals().rows()));
  SlotRules link{
      .scope = SlotScope::Program,
      .defined = {"lua_newstate", "luaL_newstate", lAlloc, "luaM_realloc_"},
      .exported = {"lua_newstate", "luaL_newstate", "luaM_realloc_"}};
  const SlotSolution whole = program.solve(link);
  const CallResolution linked = whole.resolveCall(frealloc);
  EXPECT_EQ(linked.kind, IndirectCallKind::ClosedSingle);
  EXPECT_EQ(linked.targets, std::vector<std::string>{lAlloc});
  EXPECT_FALSE(openCallTemporalDecision(linked));
  EXPECT_FALSE(whole.escapes(lAlloc));

  // A program that is not an executable keeps the slot open, with its
  // known target.
  SlotRules openLink = link;
  openLink.executable = false;
  const CallResolution call = program.solve(openLink).resolveCall(frealloc);
  EXPECT_EQ(call.kind, IndirectCallKind::OpenKnown);
  EXPECT_EQ(call.targets, std::vector<std::string>{lAlloc});
  const auto decision = openCallTemporalDecision(call);
  ASSERT_TRUE(decision);
  EXPECT_EQ(decision->trusted, TrustReason::ExternContract);
}

TEST(FnSlots, RowsRoundTrip) {
  const FnSlots slots = withLocals().withoutLocals();
  const std::vector<SlotRow> rows = slots.rows();
  EXPECT_EQ(FnSlots::fromRows(rows), slots);
  // Rows are sorted by slot and list each slot once.
  for (std::size_t i = 1; i < rows.size(); ++i)
    EXPECT_LT(rows[i - 1].slot, rows[i].slot);
  const auto out =
      std::ranges::find(rows, SlotKey::field("struct s", "cb"), &SlotRow::slot);
  ASSERT_NE(out, rows.end());
  EXPECT_EQ(out->targets, std::vector<std::string>{"c"});
  EXPECT_EQ(out->open, "load through an unknown pointer");
}

TEST(FnSlots, MergeIsAUnion) {
  FnSlots a;
  a.addMember("f", SlotKey::global("g"));
  FnSlots b;
  b.addMember("f", SlotKey::global("g"));
  b.addOpen(SlotKey::global("h"), "x");
  a.merge(b);
  EXPECT_EQ(a.constraints().size(), 2U);
  EXPECT_FALSE(a.empty());
  EXPECT_EQ(toString(IndirectCallKind::OpenKnown), "open-known");
}

} // namespace weavec::core
