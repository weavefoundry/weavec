//===- KindInferenceTest.cpp - Tests for KindInference (RFC 0030) ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §7.3–§7.6 before the engine: parameter defaults and reliance,
// static joins, `argv`, results, slot kinds and their demotions, must-access
// requirements R1–R5, store groups and §7.6 candidates.
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/KindInference.h"

#include "SiteTestUtils.h"
#include "weavec/Analysis/AttributeReader.h"
#include "weavec/Analysis/KindTable.h"
#include "weavec/Analysis/SiteCollector.h"
#include "weavec/Analysis/SlotCollector.h"
#include "weavec/Core/LibrarySpec.h"

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace weavec::analysis {
namespace {

/// A parsed unit with its declared, then inferred, kinds. Kept in place: the
/// result refers to the table.
struct InferredUnit {
  std::unique_ptr<clang::ASTUnit> ast;
  KindTable kinds;
  std::unique_ptr<SlotCollection> slots;
  core::SlotSolution solution;
  KindInferenceResult inferred;

  [[nodiscard]] clang::ASTContext &context() const {
    return ast->getASTContext();
  }
  [[nodiscard]] const clang::FunctionDecl *
  function(llvm::StringRef name) const {
    const clang::FunctionDecl *found = nullptr;
    for (const clang::Decl *decl : context().getTranslationUnitDecl()->decls())
      if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl);
          fn != nullptr && fn->getName() == name) {
        found = fn;
        if (fn->doesThisDeclarationHaveABody())
          return fn;
      }
    return found;
  }
  [[nodiscard]] const clang::FieldDecl *field(llvm::StringRef record,
                                              llvm::StringRef name) const {
    for (const clang::Decl *decl : context().getTranslationUnitDecl()->decls())
      if (const auto *tag = llvm::dyn_cast<clang::RecordDecl>(decl);
          tag != nullptr && tag->getName() == record &&
          tag->isThisDeclarationADefinition())
        for (const clang::FieldDecl *each : tag->fields())
          if (each->getName() == name)
            return each;
    return nullptr;
  }
  [[nodiscard]] const clang::VarDecl *variable(llvm::StringRef name) const {
    for (const clang::Decl *decl : context().getTranslationUnitDecl()->decls())
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl);
          var != nullptr && var->getName() == name)
        return var;
    return nullptr;
  }

  /// `<kind> <source>`, `+relies` when set, or `none`.
  [[nodiscard]] static std::string spell(const KindEntry *entry) {
    if (entry == nullptr)
      return "none";
    return entry->kind.toString() + " " +
           std::string(core::toString(entry->kind.source)) +
           (entry->reliesOnSingle ? " +relies" : "");
  }
  [[nodiscard]] std::string param(llvm::StringRef name, unsigned index) const {
    const clang::FunctionDecl *fn = function(name);
    return fn == nullptr ? "no function" : spell(kinds.param(*fn, index));
  }
  [[nodiscard]] std::string result(llvm::StringRef name) const {
    const clang::FunctionDecl *fn = function(name);
    return fn == nullptr ? "no function" : spell(kinds.result(*fn));
  }
  [[nodiscard]] std::string slot(llvm::StringRef record,
                                 llvm::StringRef name) const {
    const clang::FieldDecl *decl = field(record, name);
    return decl == nullptr ? "no field" : spell(kinds.field(*decl));
  }
  /// The reasons `record.name` was demoted.
  [[nodiscard]] std::vector<std::string> demotions(llvm::StringRef record,
                                                   llvm::StringRef name) const {
    std::vector<std::string> out;
    const clang::FieldDecl *decl = field(record, name);
    const KindEntry *entry = decl != nullptr ? kinds.field(*decl) : nullptr;
    if (entry != nullptr)
      for (const KindDemotion &demotion : entry->demotedBy)
        out.push_back(demotion.reason);
    return out;
  }
  /// The requirements of a parameter, spelled.
  [[nodiscard]] std::vector<std::string> requirements(llvm::StringRef name,
                                                      unsigned index) const {
    std::vector<std::string> out;
    const clang::FunctionDecl *fn = function(name);
    const KindEntry *entry = fn != nullptr ? kinds.param(*fn, index) : nullptr;
    if (entry != nullptr)
      for (const MustAccessRequirement &requirement : entry->mustAccess)
        out.push_back(requirement.toString());
    return out;
  }
};

} // namespace

/// Parses `code` after the site tests' prelude and runs AttributeReader,
/// SlotCollector and KindInference over it.
static std::unique_ptr<InferredUnit>
inferUnit(const std::string &code, KindInferenceOptions options = {}) {
  auto unit = std::make_unique<InferredUnit>();
  unit->ast = clang::tooling::buildASTFromCodeWithArgs(
      std::string(test::SitePrelude) + code, {"-std=gnu17", "-x", "c", "-w"},
      "input.c");
  EXPECT_NE(unit->ast, nullptr);
  if (unit->ast == nullptr)
    return unit;
  EXPECT_FALSE(unit->ast->getDiagnostics().hasErrorOccurred());
  const core::LibrarySpec &library = core::LibrarySpec::shipped();
  unit->kinds = AttributeReader(unit->context(), library).read();
  unit->slots = std::make_unique<SlotCollection>(
      SlotCollector(unit->context(), library).collect());
  unit->solution = unit->slots->solve();
  options.slots = unit->slots.get();
  options.slotSolution = &unit->solution;
  unit->inferred =
      KindInference(unit->context(), library, options).infer(unit->kinds);
  return unit;
}

namespace {

// §7.3: A1 defaults, and the uses that rest on them.
TEST(KindInference, ParameterDefaultsAndReliance) {
  const auto unit = inferUnit(R"c(
struct s { int *p; int n; };
int deref(int *p) { return *p; }
int member(struct s *o) { return o->n; }
int stored(struct s *o, int *v) { o->p = v; return 0; }
int *returned(int *v) { return v; }
int compared(int *a, int *b) { return a < b && a != 0 && !b; }
int arithmetic(int *a) { return *(a + 1); }
int indexed(int *a, int i) { return a[i] + a[2]; }
int zero(int *a) { return a[0]; }
int sized(int *a) { return (int)sizeof(*a); }
)c");
  EXPECT_EQ(unit->param("deref", 0), "single nullable default +relies");
  EXPECT_EQ(unit->param("member", 0), "single nullable default +relies");
  EXPECT_EQ(unit->param("stored", 1), "single nullable default +relies");
  EXPECT_EQ(unit->param("returned", 0), "single nullable default +relies");
  EXPECT_EQ(unit->param("compared", 0), "single nullable default");
  EXPECT_EQ(unit->param("compared", 1), "single nullable default");
  EXPECT_EQ(unit->param("arithmetic", 0), "single nullable default");
  EXPECT_EQ(unit->param("indexed", 0), "single nullable default");
  EXPECT_EQ(unit->param("zero", 0), "single nullable default +relies");
  EXPECT_EQ(unit->param("sized", 0), "single nullable default");
  const KindEntry *entry = unit->kinds.param(*unit->function("deref"), 0);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->extentClass, core::ExtentClass::LowerBound);
  EXPECT_FALSE(entry->hasDeclaredShape());
  EXPECT_FALSE(entry->isCheckOperand());
}

// §7.3: a static function whose address is not taken joins its arguments.
TEST(KindInference, StaticParametersJoinTheirArguments) {
  const auto unit = inferUnit(R"c(
static int objects(int *p) { return p != 0; }
static int cursors(int *p) { return p != 0; }
static int mixed(int *p) { return p != 0; }
static int never(int *p) { return p != 0; }
static int taken(int *p) { return p != 0; }
static int chained(int *p) { return objects(p); }
int run(int *in, int (*out)(int *)) {
  int x = 0;
  int a[4];
  out = taken;
  return objects(&x) + objects(a) + cursors(a + 1) + mixed(in) + mixed(0) +
         chained(&x) + out(in);
}
)c");
  // `&x`, an array, and `chained`'s parameter, itself `&x`: all nonnull.
  EXPECT_EQ(unit->param("objects", 0), "single nonnull inferred");
  EXPECT_EQ(unit->param("chained", 0), "single nonnull inferred");
  EXPECT_EQ(unit->param("cursors", 0), "unknown nullable inferred");
  EXPECT_EQ(unit->param("mixed", 0), "single nullable inferred");
  EXPECT_EQ(unit->param("never", 0), "single nullable default");
  EXPECT_EQ(unit->param("taken", 0), "single nullable default");
  // `run` passes its parameter on: that rests on its default.
  EXPECT_EQ(unit->param("run", 0), "single nullable default +relies");
  const KindEntry *cursor = unit->kinds.param(*unit->function("cursors"), 0);
  ASSERT_NE(cursor, nullptr);
  ASSERT_EQ(cursor->demotedBy.size(), 1U);
  EXPECT_EQ(cursor->demotedBy.front().reason, "pointer arithmetic");
}

// §7.3: `argv` of `main`, while `argc` and `argv` are never assigned.
TEST(KindInference, MainArgv) {
  const auto kept =
      inferUnit("int main(int argc, char **argv) { return argc; }");
  const KindEntry *argv = kept->kinds.param(*kept->function("main"), 1);
  ASSERT_NE(argv, nullptr);
  EXPECT_TRUE(argv->mainArgv);
  EXPECT_EQ(argv->kind.toString(), "counted(param 0 scale 1 plus 1) nonnull");
  EXPECT_EQ(argv->shapeLevel, KindLevel::SystemHeader);
  EXPECT_TRUE(argv->shapeFromSystemHeader());
  EXPECT_FALSE(argv->isCheckOperand());
  const auto moved =
      inferUnit("int main(int argc, char **argv) { argv++; return argc; }");
  const KindEntry *shifted = moved->kinds.param(*moved->function("main"), 1);
  ASSERT_NE(shifted, nullptr);
  EXPECT_FALSE(shifted->mainArgv);
  EXPECT_EQ(moved->param("main", 1), "single nullable default");
}

// §7.3: results over the unit's `return`s; A3 for the unit's declarations.
TEST(KindInference, Results) {
  const auto unit = inferUnit(R"c(
struct s { int a, b; };
static struct s one;
struct s *object(void) { return &one; }
struct s *maybe(int c) { return c ? &one : 0; }
int *cursor(int *p) { return p + 1; }
struct s *outside(void);
struct s *relay(void) { return outside(); }
char *text(void) { return "abc"; }
struct s *allocate(void) { return malloc(sizeof(struct s)); }
struct s *small(void) { return malloc(2); }
struct s *recurse(int n) { return n ? recurse(n - 1) : &one; }
int use(void) { return outside() != 0; }
)c");
  EXPECT_EQ(unit->result("object"), "single nonnull inferred");
  EXPECT_EQ(unit->result("maybe"), "single nullable inferred");
  EXPECT_EQ(unit->result("cursor"), "unknown nullable inferred");
  EXPECT_EQ(unit->result("outside"), "single nullable default");
  EXPECT_EQ(unit->result("relay"), "single nullable inferred");
  EXPECT_EQ(unit->result("text"), "single nonnull inferred");
  EXPECT_EQ(unit->result("allocate"), "single nullable inferred");
  EXPECT_EQ(unit->result("small"), "unknown nullable inferred");
  // A greatest fixpoint: the recursion does not demote itself.
  EXPECT_EQ(unit->result("recurse"), "single nonnull inferred");
}

// §7.3: Single-valid stores keep a slot `single`; anything else demotes it,
// and the demotion spreads through loads (a greatest fixpoint).
TEST(KindInference, SlotKindsFromStores) {
  const auto unit = inferUnit(R"c(
struct node {
  struct node *next;
  struct node *prev;
  char *text;
  int *cursor;
  int *copy;
  int *far;
  int *from_int;
  struct node *made;
  struct node *cast_small;
};
static int buffer[8];
int *global_one = &buffer[3];
int *global_end = buffer + 8;
void build(struct node *n, int *in, unsigned long bits) {
  struct node *fresh = malloc(sizeof *fresh);
  void *raw = malloc(sizeof(struct node));
  short small[1];
  n->next = fresh;
  n->prev = n->next;
  n->text = "label";
  n->cursor = in + 1;
  n->copy = n->cursor;
  n->far = n->copy;
  n->from_int = (int *)bits;
  n->made = (struct node *)raw;
  n->cast_small = (struct node *)small;
  n->next = n->prev;
}
)c");
  EXPECT_EQ(unit->slot("node", "next"), "single nullable inferred");
  EXPECT_EQ(unit->slot("node", "prev"), "single nullable inferred");
  EXPECT_EQ(unit->slot("node", "text"), "single nullable inferred");
  EXPECT_EQ(unit->slot("node", "cursor"), "unknown nullable inferred");
  EXPECT_EQ(unit->demotions("node", "cursor"),
            std::vector<std::string>{"pointer arithmetic"});
  EXPECT_EQ(unit->slot("node", "copy"), "unknown nullable inferred");
  EXPECT_EQ(
      unit->demotions("node", "copy"),
      std::vector<std::string>{"a load from 'node.cursor', which is unknown"});
  EXPECT_EQ(unit->slot("node", "far"), "unknown nullable inferred");
  EXPECT_EQ(unit->slot("node", "from_int"), "unknown nullable inferred");
  EXPECT_EQ(unit->demotions("node", "from_int"),
            std::vector<std::string>{"a conversion from an integer"});
  // A local carries the least width stored into it.
  EXPECT_EQ(unit->slot("node", "made"), "single nullable inferred");
  EXPECT_EQ(unit->slot("node", "cast_small"), "unknown nullable inferred");
  const clang::VarDecl *one = unit->variable("global_one");
  const clang::VarDecl *end = unit->variable("global_end");
  ASSERT_NE(one, nullptr);
  ASSERT_NE(end, nullptr);
  EXPECT_EQ(InferredUnit::spell(unit->kinds.variable(*one)),
            "single nullable inferred");
  EXPECT_EQ(InferredUnit::spell(unit->kinds.variable(*end)),
            "unknown nullable inferred");
}

// §7.3: stores the syntax does not show.
TEST(KindInference, HiddenStoresDemote) {
  const auto unit = inferUnit(R"c(
struct box { int *a; int *b; char *c; };
struct raw { int *p; };
struct zero { int *p; };
struct same { int *p; };
union u { int *p; long n; };
void *memset(void *, int, size_t);
void external(int **where);
static void keep(int **where, int *v) { *where = v; }
void run(struct box *x, struct raw *r, struct zero *z, struct same *s,
         const struct same *t, union u *v, int *in, const char *bytes) {
  int **through = &x->a;
  *through = in + 2;
  keep(&x->b, in);
  external(&x->b);
  memcpy(r, bytes, sizeof *r);
  memset(z, 0, sizeof *z);
  memcpy(s, t, sizeof *s);
  v->n = 3;
  ((char *)&x->c)[0] = 1;
}
)c");
  EXPECT_EQ(unit->demotions("box", "a"),
            std::vector<std::string>{"pointer arithmetic"});
  // `keep` only stores Single-valid values through its parameter; the
  // external callee may store anything.
  EXPECT_EQ(unit->demotions("box", "b"),
            (std::vector<std::string>{"pointer arithmetic",
                                      "its address is passed to 'external'"}));
  // `c` is address-taken too, and a `char *` slot is compatible with any
  // store through a pointer.
  EXPECT_EQ(unit->demotions("box", "c"),
            (std::vector<std::string>{"pointer arithmetic",
                                      "a character-typed store into it"}));
  EXPECT_EQ(unit->demotions("raw", "p"),
            std::vector<std::string>{"a byte-wise write by 'memcpy'"});
  EXPECT_EQ(unit->slot("zero", "p"), "single nullable inferred");
  EXPECT_EQ(unit->slot("same", "p"), "single nullable inferred");
  EXPECT_EQ(unit->demotions("u", "p"),
            std::vector<std::string>{
                "a store to 'u.n', another member of the union"});
}

// §7.3: a pointer to pointers made from anything but a fresh allocation may
// point into any object; an address converted to a pointer to non-pointers
// may be overwritten with other bytes.
TEST(KindInference, PunnedPointersToSlots) {
  const auto punned = inferUnit(R"c(
struct cell { int *p; };
struct other { int *q; };
void pun(struct cell *c, void *any, int *in) {
  int **slot = any;
  *slot = in + 1;
  *(unsigned long *)&c->p = 4;
}
)c");
  EXPECT_EQ(punned->demotions("cell", "p"),
            (std::vector<std::string>{
                "pointer arithmetic",
                "its address is converted to 'unsigned long *'"}));
  EXPECT_EQ(punned->demotions("other", "q"),
            std::vector<std::string>{"pointer arithmetic"});
  const auto fresh = inferUnit(R"c(
struct cell { int *p; };
void fill(int *in) {
  int **slots = malloc(4 * sizeof(int *));
  slots[0] = in + 1;
}
)c");
  EXPECT_EQ(fresh->slot("cell", "p"), "single nullable inferred");
}

// §7.5: R1–R5, their guards, and how each is enforced.
TEST(KindInference, MustAccessRules) {
  const auto unit = inferUnit(R"c(
struct w { int x0; };
int r1m(struct w *p) { return p->x0; }
void r2(int *p, int n) { for (int i = 1; i < n; ++i) p[i - 1] = 0; }
void r2e(int *p, unsigned n) { for (unsigned i = 0; i != 2 * n; i++) p[i] = 0; }
long r3(const long *p, const long *q) {
  long s = 0;
  for (const long *x = p; x <= q; x++) s += *x;
  return s;
}
unsigned long r4(const char *s) { const char *c = s; while (*c) c++; return c - s; }
unsigned long r4f(const char *s) { unsigned long n; for (n = 0; s[n] != 0; n++) {} return n; }
unsigned long r4l(const char *s) { return strlen(s); }
int r5(const int *p, int n) { return p[2 * n + 1]; }
int r5c(const int *p) { return p[7]; }
static int local(const int *p) { return *p; }
int call(const int *p) { return local(p); }
)c");
  EXPECT_EQ(unit->requirements("r1m", 0),
            std::vector<std::string>{"single nonnull (R1)"});
  EXPECT_EQ(unit->requirements("r2", 0),
            std::vector<std::string>{
                "1 < param 1 scale 1 plus 0 -> counted(param 1 scale 1 plus "
                "-1) nonnull (R2)"});
  EXPECT_EQ(unit->requirements("r2e", 0),
            std::vector<std::string>{
                "0 < param 1 scale 2 plus 0 -> counted(param 1 scale 2 plus "
                "0) nonnull (R2)"});
  EXPECT_EQ(unit->requirements("r3", 0),
            std::vector<std::string>{
                "param 0 scale 1 plus 0 <= param 1 scale 1 plus 0 -> "
                "ended-by(param 1 scale 1 plus 1) nonnull (R3)"});
  EXPECT_EQ(unit->requirements("r4", 0),
            std::vector<std::string>{"nul-terminated nonnull (R4)"});
  EXPECT_EQ(unit->requirements("r4f", 0),
            std::vector<std::string>{"nul-terminated nonnull (R4)"});
  EXPECT_EQ(unit->requirements("r4l", 0),
            std::vector<std::string>{"nul-terminated nonnull (R4)"});
  EXPECT_EQ(
      unit->requirements("r5", 0),
      std::vector<std::string>{"counted(param 1 scale 2 plus 2) nonnull (R5)"});
  EXPECT_EQ(unit->requirements("r5c", 0),
            std::vector<std::string>{"counted(8) nonnull (R5)"});
  const KindEntry *local = unit->kinds.param(*unit->function("local"), 0);
  ASSERT_NE(local, nullptr);
  EXPECT_EQ(local->enforcement, RequirementEnforcement::CallSites);
  EXPECT_TRUE(local->hasEnforcedRequirement());
  ASSERT_EQ(local->mustAccess.size(), 1U);
  EXPECT_NE(local->mustAccess.front().access, nullptr);
  const KindEntry *exported = unit->kinds.param(*unit->function("r1m"), 0);
  ASSERT_NE(exported, nullptr);
  EXPECT_EQ(exported->enforcement, RequirementEnforcement::CallerContract);
  EXPECT_FALSE(exported->hasEnforcedRequirement());
}

// §7.5: what keeps an access from being a must-access.
TEST(KindInference, MustAccessConditions) {
  const auto unit = inferUnit(R"c(
void unknown(void);
static void helper(int *q) { (void)q; }
static void fatal(void) { exit(1); }
static void loops(void) { while (1) {} }
int tested(int *p) { if (p) return *p; return 0; }
int nulltest(int *p) { if (p == 0) exit(1); return *p; }
int early(int *p, int c) { if (c) return 0; return *p; }
int unknowns(int *p) { unknown(); return *p; }
int helped(int *p) { helper(p); return *p; }
int fatals(int *p, int c) { if (c) fatal(); return *p; }
int changed(int *p, int *q) { p = q; return *p; }
int taken(int *p) { int **pp = &p; (void)pp; return *p; }
int branched(int *p, int c) { return c ? *p : 1; }
int breaks(int *p, int n) {
  int s = 0;
  for (int i = 0; i < n; ++i) { if (s > 3) break; s += p[i]; }
  return s;
}
int modified(int *p, int n) {
  int s = 0;
  for (int i = 0; i < n; ++i) { s += p[i]; i += 0; }
  return s;
}
int infinite(int *p) { loops(); return *p; }
)c");
  for (const char *name :
       {"tested", "nulltest", "early", "unknowns", "fatals", "changed", "taken",
        "branched", "breaks", "modified"})
    EXPECT_TRUE(unit->requirements(name, 0).empty()) << name;
  // A helper the unit defines that always returns keeps the must-access;
  // one that never returns, or may exit, does not.
  EXPECT_EQ(unit->requirements("helped", 0),
            std::vector<std::string>{"single nonnull (R1)"});
  EXPECT_TRUE(unit->inferred.alwaysReturns(*unit->function("helper")));
  EXPECT_FALSE(unit->inferred.alwaysReturns(*unit->function("fatal")));
  EXPECT_FALSE(unit->inferred.alwaysReturns(*unit->function("fatals")));
  EXPECT_TRUE(unit->inferred.alwaysReturns(*unit->function("loops")))
      << "not terminating is not exiting";
  EXPECT_EQ(unit->requirements("infinite", 0),
            std::vector<std::string>{"single nonnull (R1)"});
}

/// Every call of a function, with whether it is known to return.
class CallReturns : public clang::RecursiveASTVisitor<CallReturns> {
public:
  CallReturns(const KindInferenceResult &inferred,
              std::vector<std::pair<std::string, bool>> &seen)
      : result(inferred), out(seen) {}
  // The CRTP hook is found by name.
  // NOLINTNEXTLINE(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool VisitCallExpr(clang::CallExpr *call) {
    const clang::FunctionDecl *callee = call->getDirectCallee();
    out.emplace_back(callee != nullptr ? callee->getNameAsString() : "*fp",
                     result.knownToReturn(*call));
    return true;
  }

private:
  const KindInferenceResult &result;
  std::vector<std::pair<std::string, bool>> &out;
};

// §7.5 "known to return": the table, platform declarations, builtins, the
// unit's definitions; never an indirect or unknown callee.
TEST(KindInference, KnownToReturn) {
  const auto unit = inferUnit(R"c(
void unknown(void);
static int twice(int n) { return n < 1 ? 0 : twice(n - 1); }
static void bail(void) { exit(2); }
static void outer(void) { bail(); }
void calls(void (*fp)(void), const char *s) {
  (void)strlen(s);
  exit(0);
  unknown();
  (void)twice(3);
  outer();
  fp();
  (void)__builtin_expect(1, 1);
}
)c");
  std::vector<std::pair<std::string, bool>> seen;
  CallReturns visitor(unit->inferred, seen);
  // RecursiveASTVisitor takes a mutable declaration but only reads it.
  // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast)
  visitor.TraverseDecl(
      const_cast<clang::FunctionDecl *>(unit->function("calls")));
  // NOLINTEND(cppcoreguidelines-pro-type-const-cast)
  EXPECT_EQ(seen, (std::vector<std::pair<std::string, bool>>{
                      {"strlen", true},
                      {"exit", false},
                      {"unknown", false},
                      {"twice", true},
                      {"outer", false},
                      {"*fp", false},
                      {"__builtin_expect", true}}));
}

// §7.4 rule 7: stores of a relation in one block, with no call, loop or
// access through the object between them, form one group.
TEST(KindInference, StoreGroups) {
  const auto unit = inferUnit(R"c(
#define COUNTED_BY(n) __attribute__((annotate("weavec.counted_by." #n)))
struct buf { char *COUNTED_BY(cap) data; unsigned long cap; unsigned long len; };
void grow(struct buf *b, unsigned long n) {
  b->data = malloc(n);
  b->cap = n;
}
void split(struct buf *b, unsigned long n) {
  b->cap = n;
  b->data = malloc(n);
}
void touched(struct buf *b, struct buf *other, unsigned long n) {
  b->cap = n;
  other->cap = b->cap;
  b->data = 0;
}
void looped(struct buf *b, unsigned long n) {
  b->cap = 0;
  for (unsigned long i = 0; i < n; i++)
    b->len++;
  b->data = 0;
}
)c");
  std::vector<std::string> groups;
  for (const StoreGroup &group : unit->inferred.storeGroups()) {
    std::string text = group.function->getNameAsString() + ":";
    for (const clang::FieldDecl *field : group.fields)
      text += " " + field->getNameAsString();
    text += " x" + std::to_string(group.stores.size());
    groups.push_back(text);
    EXPECT_EQ(group.last(), group.stores.back());
    EXPECT_EQ(group.record, unit->field("buf", "data")->getParent());
  }
  EXPECT_EQ(groups,
            (std::vector<std::string>{"grow: data cap x2",
                                      // A call between the stores splits them.
                                      "split: cap x1", "split: data x1",
                                      // So does an access through the object.
                                      "touched: cap x1", "touched: cap x1",
                                      "touched: data x1",
                                      // And a loop.
                                      "looped: cap x1", "looped: data x1"}));
}

// §7.6: candidates of the unit's structs, pruned to related fields and
// disqualified by the writes the store-group rule cannot see.
TEST(KindInference, FieldCandidates) {
  const char *code = R"c(
struct vec { int *data; unsigned long len; unsigned long cap; char *name; };
struct flat { int *p; int n; };
struct holder { struct flat inner; int other; };
struct view { int *p; int n; };
struct outside { void *mem; int size; };
void use(struct vec *v, struct holder *h, const char *raw, long *l,
         struct outside *o) {
  for (unsigned long i = 0; i < v->len; i++)
    v->data[i] = 0;
  memcpy(h, raw, sizeof *h);
  struct view *w = (struct view *)l;
  w->p = 0;
  w->n = 0;
  o->mem = malloc(o->size);
}
)c";
  const auto unit = inferUnit(code);
  std::vector<std::string> candidates;
  for (const FieldCandidate &candidate : unit->inferred.fieldCandidates())
    candidates.push_back(candidate.record + " " +
                         (candidate.bytes ? "bytes(" : "count(") +
                         candidate.pointer + ") == " + candidate.count + " + " +
                         std::to_string(candidate.offset));
  EXPECT_EQ(candidates,
            (std::vector<std::string>{"struct outside bytes(mem) == size + 0",
                                      "struct outside bytes(mem) == size + 1",
                                      "struct vec count(data) == len + 0",
                                      "struct vec count(data) == len + 1",
                                      "struct vec bytes(data) == len + 0",
                                      "struct vec bytes(data) == len + 1"}));
  std::vector<std::string> dropped;
  for (const DisqualifiedRecord &record : unit->inferred.disqualified())
    dropped.push_back(record.key + ": " + record.reason);
  EXPECT_EQ(dropped,
            (std::vector<std::string>{
                "struct flat: an object of 'flat' is written byte-wise (a "
                "byte-wise write by 'memcpy')",
                "struct view: an object of 'view' is converted from another "
                "type"}));
  // Without pruning, every pointer and integer field pair is a candidate.
  KindInferenceOptions all;
  all.pruneUnrelatedCandidates = false;
  const auto unpruned = inferUnit(code, all);
  EXPECT_EQ(unpruned->inferred.fieldCandidates().size(), 14U);
  // The designated first cut: no candidates at all.
  KindInferenceOptions cut;
  cut.fieldCandidates = false;
  EXPECT_TRUE(inferUnit(code, cut)->inferred.fieldCandidates().empty());
}

// §13.1 `imports.calls` and the §7.3 Call-site row: Single-valid arguments.
TEST(KindInference, SingleValidQueries) {
  const auto unit = inferUnit(R"c(
struct pair { int a, b; };
void take(struct pair *p);
void calls(struct pair *in, int *ints) {
  struct pair here;
  take(&here);
  take(in);
  take((struct pair *)ints);
  take((struct pair *)(in + 1));
  take(0);
}
)c");
  std::vector<bool> valid;
  for (const clang::CallExpr *call : [&] {
         std::vector<const clang::CallExpr *> calls;
         const auto *body = llvm::cast<clang::CompoundStmt>(
             unit->function("calls")->getBody());
         for (const clang::Stmt *stmt : body->body())
           if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt))
             calls.push_back(call);
         return calls;
       }())
    valid.push_back(unit->inferred.argumentIsSingleValid(*call, 0));
  EXPECT_EQ(valid, (std::vector<bool>{true, true, false, false, true}));
}

// §9.3: an indirect call's result joins its closed targets' results.
TEST(KindInference, IndirectCallResults) {
  const char *code = R"c(
struct s { int a; };
static struct s one;
static struct s *get(void) { return &one; }
static struct s *(*getter)(void) = get;
struct holder { struct s *p; };
void keep(struct holder *h) { h->p = getter(); }
)c";
  EXPECT_EQ(inferUnit(code)->slot("holder", "p"), "single nullable inferred");
  // Without the slots, an indirect call's result is never Single-valid.
  const auto alone = std::make_unique<InferredUnit>();
  alone->ast = clang::tooling::buildASTFromCodeWithArgs(
      code, {"-std=gnu17", "-x", "c", "-w"}, "input.c");
  ASSERT_NE(alone->ast, nullptr);
  alone->kinds =
      AttributeReader(alone->context(), core::LibrarySpec::shipped()).read();
  alone->inferred =
      KindInference(alone->context(), core::LibrarySpec::shipped())
          .infer(alone->kinds);
  EXPECT_EQ(alone->slot("holder", "p"), "unknown nullable inferred");
}

// The inferred entries leave the unit's sites as the declared ones made
// them: only declared shapes are required positions (§7.4 rule 4).
TEST(KindInference, SitesDoNotChange) {
  const std::string code = R"c(
static int first(int *p) { return p[0]; }
int *cursor(int *p) { return p + 1; }
struct s { int *p; };
void use(struct s *o, int *q) {
  o->p = cursor(q) + 1;
  (void)first(q + 2);
}
)c";
  const test::CollectedUnit before = test::collectUnit(code);
  const auto after = inferUnit(code);
  const SiteIndex sites = SiteCollector(after->context(), after->kinds,
                                        core::LibrarySpec::shipped())
                              .collect();
  const auto describe = [](const SiteIndex &index, llvm::StringRef function) {
    std::vector<std::string> out;
    for (const core::FunctionLedger &row : index.ledgers()) {
      if (row.name != function)
        continue;
      for (const core::Site &site : row.sites) {
        std::string text(core::toString(site.kind));
        if (site.boundary)
          text += "/" + std::string(core::toString(*site.boundary));
        text += " " + site.text;
        for (const core::Facet facet : core::AllFacets)
          if (site.hasFacet(facet))
            text += " " + std::string(core::toString(facet));
        out.push_back(text);
      }
    }
    return out;
  };
  for (const char *function : {"first", "cursor", "use"}) {
    EXPECT_FALSE(describe(sites, function).empty()) << function;
    EXPECT_EQ(describe(sites, function), describe(before.sites, function))
        << function;
  }
}

} // namespace
} // namespace weavec::analysis
