//===- SlotCollectorTest.cpp - Tests for SlotCollector (RFC 0030) ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §9.3: the constraints of a unit's function-pointer values, the
// per-TU closedness rules, the export form, and the Lua allocator pattern.
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/SlotCollector.h"

#include "weavec/Core/FnSlots.h"
#include "weavec/Core/LibrarySpec.h"

#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace weavec::analysis {
namespace {

using core::SlotKey;
using Targets = std::set<std::string>;

struct CollectedSlots {
  std::unique_ptr<clang::ASTUnit> ast;
  SlotCollection slots;
  core::SlotSolution solution;

  /// Every constraint, spelled as `FnSlots::print` spells it.
  [[nodiscard]] std::string text() const { return slots.constraints().print(); }
  [[nodiscard]] bool has(const std::string &line) const {
    return text().find(line + "\n") != std::string::npos;
  }
};

} // namespace

/// Parses `code` as the unit `unit` (with `header` as `lib.h` on the include
/// path) and collects its slots.
static std::unique_ptr<CollectedSlots>
collect(const std::string &code, const std::string &header = {},
        const std::string &unit = "a.c") {
  auto out = std::make_unique<CollectedSlots>();
  clang::tooling::FileContentMappings files;
  if (!header.empty())
    files.emplace_back("/virtual/lib.h", header);
  out->ast = clang::tooling::buildASTFromCodeWithArgs(
      code, {"-std=gnu17", "-x", "c", "-w", "-I", "/virtual"}, unit,
      "clang-tool", std::make_shared<clang::PCHContainerOperations>(),
      clang::tooling::getClangStripDependencyFileAdjuster(), files);
  EXPECT_NE(out->ast, nullptr);
  if (out->ast == nullptr)
    return out;
  EXPECT_FALSE(out->ast->getDiagnostics().hasErrorOccurred());
  out->slots =
      SlotCollector(out->ast->getASTContext(), core::LibrarySpec::shipped(),
                    SlotCollectorOptions{.unit = unit})
          .collect();
  out->solution = out->slots.solve();
  return out;
}

namespace {

TEST(SlotCollector, MembersCopiesAndCalls) {
  const auto unit = collect(R"c(
typedef void (*fn)(void);
static void a(void) {}
static void b(void) {}
void c(void) {}
static fn table[] = {a, b};
fn global = c;
static fn pick(int i) { return i ? a : table[i]; }
static void run(fn f) { f(); }
void drive(int i) {
  fn local = pick(i);
  run(local);
  run((fn)&c);
  global = local;
}
)c");
  for (const char *line :
       {"member\ta.c:a\tstatic a.c:table", "member\ta.c:b\tstatic a.c:table",
        "member\tc\tglobal global", "member\ta.c:a\tresult a.c:pick",
        "subset\tstatic a.c:table\tresult a.c:pick",
        "subset\tresult a.c:pick\tlocal drive local",
        "subset\tlocal drive local\tparam a.c:run 0",
        "member\tc\tparam a.c:run 0",
        "subset\tlocal drive local\tglobal global"})
    EXPECT_TRUE(unit->has(line)) << line << "\n" << unit->text();
  EXPECT_EQ(unit->solution.targets(SlotKey::param("a.c:run", 0)),
            (Targets{"a.c:a", "a.c:b", "c"}));
  // The call through `run`'s parameter: closed (`run` is static and its
  // address never escapes), with three targets.
  ASSERT_EQ(unit->slots.indirectCalls().size(), 1U);
  const auto &[call, slot] = unit->slots.indirectCalls().front();
  EXPECT_EQ(unit->slots.calleeSlot(*call), slot);
  EXPECT_EQ(slot, SlotKey::param("a.c:run", 0));
  const core::CallResolution resolution = unit->solution.resolveCall(slot);
  EXPECT_EQ(resolution.kind, core::IndirectCallKind::ClosedJoin);
  EXPECT_EQ(resolution.targets,
            (std::vector<std::string>{"a.c:a", "a.c:b", "c"}));
  // `global` is externally visible: open per TU.
  EXPECT_TRUE(unit->solution.isOpen(SlotKey::global("global")));
  EXPECT_NE(unit->slots.function("a.c:a"), nullptr);
  EXPECT_NE(unit->slots.function("c"), nullptr);
  EXPECT_EQ(unit->slots.function("missing"), nullptr);
  EXPECT_TRUE(unit->slots.rules().defined.contains("a.c:pick"));
  EXPECT_TRUE(unit->slots.rules().exported.contains("drive"));
  EXPECT_FALSE(unit->slots.rules().exported.contains("a.c:pick"));
}

TEST(SlotCollector, IndirectCallsThroughSlots) {
  const auto unit = collect(R"c(
typedef int (*op)(int);
typedef op (*maker)(void);
static int inc(int x) { return x + 1; }
static op make(void) { return inc; }
struct ops { maker make; op apply; };
static struct ops all = {make, inc};
int apply(int x, int c) {
  op chosen = c ? all.apply : all.make();
  return chosen(x) + (c ? inc : all.apply)(x);
}
)c");
  // `all.make()` is a call through a field; its result flows to `chosen`.
  EXPECT_TRUE(unit->has(
      "subset\tcall-result field a.c:struct ops make\tlocal apply chosen"))
      << unit->text();
  std::vector<std::string> kinds;
  for (const auto &[call, slot] : unit->slots.indirectCalls())
    kinds.push_back(
        slot.toString() + " " +
        std::string(core::toString(unit->solution.resolveCall(slot).kind)));
  // A callee expression with no single slot gets its own local slot.
  EXPECT_EQ(kinds, (std::vector<std::string>{
                       "field a.c:struct ops make closed-single",
                       "local apply chosen closed-single",
                       "local apply <callee 10:22> closed-single"}));
  EXPECT_EQ(unit->solution.targets(SlotKey::local("apply", "chosen")),
            Targets{"a.c:inc"});
  EXPECT_TRUE(unit->slots.rules().confinedRecords.contains("a.c:struct ops"));
}

TEST(SlotCollector, OpenSources) {
  const auto unit = collect(R"c(
typedef void (*fn)(void);
typedef __builtin_va_list va_list;
struct box { fn f; };
union either { fn f; long n; };
struct packed { fn f; };
fn from_int(unsigned long bits) { fn f = (fn)bits; return f; }
fn from_data(void *p) { fn f = (fn)p; return f; }
fn through(fn *pp) { return *pp; }
fn varargs(int n, ...) {
  va_list ap;
  __builtin_va_start(ap, n);
  fn f = __builtin_va_arg(ap, fn);
  __builtin_va_end(ap);
  return f;
}
void *memcpy(void *, const void *, unsigned long);
void writes(union either *u, struct packed *p, const char *raw) {
  u->n = 1;
  memcpy(p, raw, sizeof *p);
}
)c");
  const std::string punned = std::string("open\tfield a.c:union either f\t") +
                             "a store to 'n', another member of the union";
  for (const std::string &line :
       {std::string("open\tlocal from_int f\ta conversion from an integer"),
        std::string(
            "open\tlocal from_data f\ta conversion from a data pointer"),
        std::string("open\tresult through\ta load through a pointer"),
        std::string("open\tlocal varargs f\ta value from va_arg"), punned,
        std::string(
            "open\tfield a.c:struct packed f\ta byte-wise write by 'memcpy'")})
    EXPECT_TRUE(unit->has(line)) << line << "\n" << unit->text();
}

TEST(SlotCollector, EscapesAndClosedness) {
  const auto unit = collect(R"c(
typedef void (*fn)(void);
static void a(void) {}
static void b(void) {}
static void d(void) {}
struct local_ops { fn f; };
struct shared_ops { fn f; };
static struct local_ops mine = {a};
static fn hook = b;
static fn exposed = b;
void register_ops(struct shared_ops *ops);
void *keep(void *p);
void setup(void) {
  static struct shared_ops theirs = {d};
  register_ops(&theirs);
  mine.f();
  hook();
  fn *where = &exposed;
  (void)where;
  (void)keep((void *)d);
}
)c");
  const core::SlotRules &rules = unit->slots.rules();
  EXPECT_TRUE(rules.confinedRecords.contains("a.c:struct local_ops"));
  // Objects of `shared_ops` reach a callee without a body.
  EXPECT_FALSE(rules.confinedRecords.contains("a.c:struct shared_ops"));
  EXPECT_TRUE(rules.escapedStatics.contains("a.c:exposed"));
  EXPECT_FALSE(rules.escapedStatics.contains("a.c:hook"));
  EXPECT_TRUE(
      unit->solution.isClosed(SlotKey::field("a.c:struct local_ops", "f")));
  EXPECT_TRUE(
      unit->solution.isOpen(SlotKey::field("a.c:struct shared_ops", "f")));
  EXPECT_TRUE(unit->solution.isClosed(SlotKey::staticGlobal("a.c", "hook")));
  EXPECT_TRUE(unit->solution.isOpen(SlotKey::staticGlobal("a.c", "exposed")));
  // A function converted to a data pointer escapes: code outside may call it.
  EXPECT_TRUE(unit->has("member\ta.c:d\tparam <unknown> 0")) << unit->text();
  EXPECT_TRUE(unit->solution.escapes("a.c:d"));
  EXPECT_FALSE(unit->solution.escapes("a.c:a"));
}

// §9.3 and §13.1: the export form drops local slots and keeps the solution
// of every other slot.
TEST(SlotCollector, ExportFormKeepsTheSolution) {
  const auto unit = collect(R"c(
typedef void (*fn)(void);
static void a(void) {}
fn out;
void relay(fn in) { fn tmp = in; fn other = a; out = tmp; other(); tmp(); }
)c");
  const core::FnSlots exported = unit->slots.exported();
  for (const core::SlotConstraint &constraint : exported.constraints())
    EXPECT_FALSE(constraint.slot.isLocal()) << constraint.slot.toString();
  const core::SlotSolution again = exported.solve(unit->slots.rules());
  for (const SlotKey &slot : unit->solution.slots()) {
    if (slot.isLocal())
      continue;
    EXPECT_EQ(unit->solution.targets(slot), again.targets(slot))
        << slot.toString();
    EXPECT_EQ(unit->solution.isOpen(slot), again.isOpen(slot))
        << slot.toString();
  }
  EXPECT_TRUE(again.isOpen(SlotKey::global("out")));
}

// §9.3, the Lua pattern: `g->frealloc` is stored from `lua_newstate`'s
// parameter, and `luaL_newstate` passes `l_alloc`. Per TU the slot is open
// and empty, so the calls are `unresolved(callback)`, not silent; at link
// the slot is `{l_alloc}`.
TEST(SlotCollector, LuaAllocatorPattern) {
  const std::string header = R"c(
typedef void *(*lua_Alloc)(void *ud, void *ptr, unsigned long osize,
                           unsigned long nsize);
struct global_State { lua_Alloc frealloc; void *ud; };
struct global_State *lua_newstate(lua_Alloc f, void *ud);
)c";
  const auto lstate = collect(R"c(
#include "lib.h"
struct global_State *make_state(void);
struct global_State *lua_newstate(lua_Alloc f, void *ud) {
  struct global_State *g = make_state();
  g->frealloc = f;
  g->ud = ud;
  return g;
}
void *luaM_realloc_(struct global_State *g, void *block, unsigned long n) {
  return (*g->frealloc)(g->ud, block, 0, n);
}
)c",
                              header, "lstate.c");
  const SlotKey frealloc = SlotKey::field("struct global_State", "frealloc");
  EXPECT_TRUE(lstate->has("subset\tparam lua_newstate 0\tfield struct "
                          "global_State frealloc"))
      << lstate->text();
  ASSERT_EQ(lstate->slots.indirectCalls().size(), 1U);
  EXPECT_EQ(lstate->slots.indirectCalls().front().second, frealloc);
  const core::CallResolution perUnit = lstate->solution.resolveCall(frealloc);
  EXPECT_EQ(perUnit.kind, core::IndirectCallKind::OpenUnknown);
  const auto decision = core::openCallTemporalDecision(perUnit);
  ASSERT_TRUE(decision);
  EXPECT_EQ(decision->unresolved, core::UnresolvedReason::Callback);

  const auto lauxlib = collect(R"c(
#include "lib.h"
static void *l_alloc(void *ud, void *ptr, unsigned long osize,
                     unsigned long nsize) {
  (void)ud; (void)osize; (void)nsize; return ptr;
}
struct global_State *luaL_newstate(void) { return lua_newstate(l_alloc, 0); }
)c",
                               header, "lauxlib.c");
  EXPECT_TRUE(lauxlib->has("member\tlauxlib.c:l_alloc\tparam lua_newstate 0"))
      << lauxlib->text();
  EXPECT_TRUE(lauxlib->solution.escapes("lauxlib.c:l_alloc"));

  // The unit records carry the export form; the link solves them together.
  core::FnSlots program = lstate->slots.exported();
  program.merge(lauxlib->slots.exported());
  core::SlotRules link;
  link.scope = core::SlotScope::Link;
  for (const auto *unit : {lstate.get(), lauxlib.get()}) {
    link.defined.insert(unit->slots.rules().defined.begin(),
                        unit->slots.rules().defined.end());
    link.exported.insert(unit->slots.rules().exported.begin(),
                         unit->slots.rules().exported.end());
  }
  const core::CallResolution linked = program.solve(link).resolveCall(frealloc);
  EXPECT_EQ(linked.kind, core::IndirectCallKind::ClosedSingle);
  EXPECT_EQ(linked.targets, std::vector<std::string>{"lauxlib.c:l_alloc"});
}

} // namespace
} // namespace weavec::analysis
