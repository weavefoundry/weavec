//===- KindInferenceMustAccess.cpp - Must-access requirements (RFC 0030) --===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// §7.5: requirements R1–R5 for each pointer parameter, from the function's
// CFG and post-dominator tree alone. An access is a must-access when its
// block post-dominates the entry (or, inside a canonical counted loop, the
// loop's header post-dominates the entry and the access's block
// post-dominates the loop body's entry), and nothing that may run before it
// assigns the parameter, tests its nullness, or calls a function not known
// to return.
//
// "Known to return" for a function the unit defines is the syntactic
// greatest fixpoint documented in KindInference.h.
//
//===----------------------------------------------------------------------===//

#include "KindInferenceImpl.h"

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/SourceManager.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CheckedArithmetic.h"

#include <functional>
#include <utility>

namespace weavec::analysis {

const FunctionCfg *
KindInferenceState::cfgOf(const clang::FunctionDecl &function) const {
  std::unique_ptr<FunctionCfg> &slot = cfgs[&function];
  if (slot != nullptr)
    return slot->cfg != nullptr ? slot.get() : nullptr;
  slot = std::make_unique<FunctionCfg>();
  clang::CFG::BuildOptions build;
  build.setAllAlwaysAdd();
  // `buildCFG` takes a mutable body but only reads it.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  slot->cfg = clang::CFG::buildCFG(
      &function, const_cast<clang::Stmt *>(function.getBody()), &context,
      build);
  if (slot->cfg == nullptr)
    return nullptr;
  slot->postDominators =
      std::make_unique<clang::CFGPostDomTree>(slot->cfg.get());
  for (const clang::CFGBlock *block : *slot->cfg) {
    unsigned index = 0;
    for (const clang::CFGElement &element : *block) {
      if (const auto stmt = element.getAs<clang::CFGStmt>())
        slot->where.try_emplace(stmt->getStmt(), block, index);
      ++index;
    }
    if (const clang::Stmt *terminator = block->getTerminatorStmt())
      slot->headers.try_emplace(terminator, block);
  }
  return slot.get();
}

// -- Known to return (§7.5)
// ----------------------------------------------------

/// §5.2: `function` is declared in a C library, POSIX or platform header, or
/// in the toolchain's own headers.
static bool platformDeclaration(const clang::FunctionDecl &function,
                                const core::LibrarySpec &library,
                                const clang::SourceManager &sm) {
  const clang::SourceLocation location =
      sm.getExpansionLoc(function.getCanonicalDecl()->getLocation());
  if (!sm.isInSystemHeader(location))
    return false;
  const llvm::StringRef path = sm.getFilename(location);
  if (path.empty())
    return false;
  if (path.contains("/lib/clang/"))
    return true;
  const bool darwinSdk = path.contains(".sdk/");
  // The include directory that found the header is not known here: every
  // suffix of the path is a candidate name.
  llvm::StringRef rest = path;
  while (!rest.empty()) {
    if (library.isPlatformHeader(std::string_view(rest.data(), rest.size()),
                                 darwinSdk))
      return true;
    const std::size_t slash = rest.find('/');
    if (slash == llvm::StringRef::npos)
      break;
    rest = rest.drop_front(slash + 1);
  }
  return false;
}

/// Known to return without looking at the unit's own definitions: none for
/// a call to a function the unit defines.
static std::optional<bool> returnsOutsideUnit(const clang::CallExpr &call,
                                              const core::LibrarySpec &library,
                                              const clang::SourceManager &sm) {
  const clang::FunctionDecl *callee = call.getDirectCallee();
  if (callee == nullptr)
    return false; // indirect: unknown
  if (callee->isNoReturn())
    return false;
  if (const auto match = governingLibraryEntry(*callee, library))
    return match->entry->knownToReturn();
  if (callee->getDefinition() != nullptr)
    return std::nullopt;
  if (callee->getBuiltinID() != 0)
    return true;
  return platformDeclaration(*callee, library, sm);
}

bool KindInferenceState::alwaysReturns(
    const clang::FunctionDecl &function) const {
  if (returns.empty() && !definitions.empty()) {
    // A greatest fixpoint: every definition returns until one of its calls
    // may not, which then spreads to its callers.
    const clang::SourceManager &sm = context.getSourceManager();
    llvm::DenseMap<const clang::FunctionDecl *,
                   std::vector<const clang::FunctionDecl *>>
        callers;
    std::vector<const clang::FunctionDecl *> falling;
    for (const clang::FunctionDecl *definition : definitions)
      returns[definition->getCanonicalDecl()] = !definition->isNoReturn();
    for (const clang::FunctionDecl *definition : definitions) {
      const clang::FunctionDecl *caller = definition->getCanonicalDecl();
      if (const auto calls = callsIn.find(definition); calls != callsIn.end()) {
        for (const clang::CallExpr *call : calls->second) {
          const auto outside = returnsOutsideUnit(*call, library, sm);
          if (!outside)
            callers[call->getDirectCallee()->getCanonicalDecl()].push_back(
                caller);
          else if (!*outside)
            returns[caller] = false;
        }
      }
      if (!returns[caller])
        falling.push_back(caller);
    }
    while (!falling.empty()) {
      const clang::FunctionDecl *callee = falling.back();
      falling.pop_back();
      for (const clang::FunctionDecl *caller : callers[callee]) {
        bool &flag = returns[caller];
        if (flag) {
          flag = false;
          falling.push_back(caller);
        }
      }
    }
  }
  const auto found = returns.find(function.getCanonicalDecl());
  return found != returns.end() && found->second;
}

bool KindInferenceState::knownToReturn(const clang::CallExpr &call) const {
  const auto outside =
      returnsOutsideUnit(call, library, context.getSourceManager());
  if (outside)
    return *outside;
  return alwaysReturns(*call.getDirectCallee()->getDefinition());
}

// -- Syntax helpers
// ---------------------------------------------------------------

/// `stmt` assigns, increments or decrements `variable`.
static bool modifies(const clang::Stmt *stmt, const clang::VarDecl *variable) {
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(stmt))
    return binary->isAssignmentOp() && variableOf(binary->getLHS()) == variable;
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt))
    return (unary->isIncrementDecrementOp() ||
            unary->getOpcode() == clang::UO_AddrOf) &&
           variableOf(unary->getSubExpr()) == variable;
  return false;
}

/// Whether anything in `root` other than `except` modifies `variable`.
static bool modifiedIn(const clang::Stmt *root, const clang::VarDecl *variable,
                       const clang::Stmt *except = nullptr) {
  if (root == nullptr)
    return false;
  if (root != except && modifies(root, variable))
    return true;
  return llvm::any_of(root->children(), [&](const clang::Stmt *child) {
    return modifiedIn(child, variable, except);
  });
}

/// `condition` tests `param` against null (`p`, `!p`, `p == NULL`, and the
/// same inside `&&`, `||` and `__builtin_expect`).
static bool testsNullness(const clang::Expr *condition,
                          const clang::ParmVarDecl &param,
                          clang::ASTContext &context) {
  if (condition == nullptr)
    return false;
  condition = condition->IgnoreParenImpCasts();
  if (parameterOf(condition) == &param)
    return true;
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(condition))
    return unary->getOpcode() == clang::UO_LNot &&
           testsNullness(unary->getSubExpr(), param, context);
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(condition)) {
    const unsigned builtin = call->getBuiltinCallee();
    return (builtin == clang::Builtin::BI__builtin_expect ||
            builtin == clang::Builtin::BI__builtin_expect_with_probability) &&
           call->getNumArgs() > 0 &&
           testsNullness(call->getArg(0), param, context);
  }
  const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(condition);
  if (binary == nullptr)
    return false;
  if (binary->isLogicalOp())
    return testsNullness(binary->getLHS(), param, context) ||
           testsNullness(binary->getRHS(), param, context);
  if (!binary->isEqualityOp())
    return false;
  const auto isNull = [&](const clang::Expr *side) {
    return side->isNullPointerConstant(
               context, clang::Expr::NPC_ValueDependentIsNotNull) !=
           clang::Expr::NPCK_NotNull;
  };
  return (parameterOf(binary->getLHS()) == &param &&
          isNull(binary->getRHS())) ||
         (parameterOf(binary->getRHS()) == &param && isNull(binary->getLHS()));
}

/// A `break` (not inside a nested loop or switch), `return` or `goto` in a
/// loop body: some iteration may not run to its end (§7.5).
static bool leavesEarly(const clang::Stmt *body, unsigned depth = 0) {
  if (body == nullptr)
    return false;
  if (llvm::isa<clang::ReturnStmt>(body) || llvm::isa<clang::GotoStmt>(body) ||
      llvm::isa<clang::IndirectGotoStmt>(body))
    return true;
  if (llvm::isa<clang::BreakStmt>(body))
    return depth == 0;
  const bool breakable =
      llvm::isa<clang::ForStmt>(body) || llvm::isa<clang::WhileStmt>(body) ||
      llvm::isa<clang::DoStmt>(body) || llvm::isa<clang::SwitchStmt>(body);
  return llvm::any_of(body->children(), [&](const clang::Stmt *child) {
    return leavesEarly(child, depth + (breakable ? 1 : 0));
  });
}

/// `stmt` is `++v`, `v++`, `v += 1` or `v = v + 1`.
static bool incrementsByOne(const clang::Stmt *stmt,
                            const clang::VarDecl *variable,
                            const KindInferenceState &state) {
  if (stmt == nullptr || variable == nullptr)
    return false;
  if (const auto *expr = llvm::dyn_cast<clang::Expr>(stmt))
    stmt = expr->IgnoreParenImpCasts();
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt))
    return unary->isIncrementOp() &&
           variableOf(unary->getSubExpr()) == variable;
  const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(stmt);
  if (binary == nullptr || variableOf(binary->getLHS()) != variable)
    return false;
  if (binary->getOpcode() == clang::BO_AddAssign)
    return state.constantOf(binary->getRHS()) == 1;
  if (binary->getOpcode() != clang::BO_Assign)
    return false;
  const auto *sum = llvm::dyn_cast<clang::BinaryOperator>(
      binary->getRHS()->IgnoreParenImpCasts());
  if (sum == nullptr || sum->getOpcode() != clang::BO_Add)
    return false;
  return (variableOf(sum->getLHS()) == variable &&
          state.constantOf(sum->getRHS()) == 1) ||
         (variableOf(sum->getRHS()) == variable &&
          state.constantOf(sum->getLHS()) == 1);
}

/// `v`, `v + k`, `k + v` or `v - k`: the constant `k` (negated for `-`).
static std::optional<std::int64_t> offsetFrom(const clang::Expr *index,
                                              const clang::VarDecl *variable,
                                              const KindInferenceState &state) {
  index = index->IgnoreParenImpCasts();
  if (variableOf(index) == variable)
    return 0;
  const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(index);
  if (binary == nullptr)
    return std::nullopt;
  if (binary->getOpcode() == clang::BO_Add) {
    if (variableOf(binary->getLHS()) == variable)
      return state.constantOf(binary->getRHS());
    if (variableOf(binary->getRHS()) == variable)
      return state.constantOf(binary->getLHS());
  }
  if (binary->getOpcode() == clang::BO_Sub &&
      variableOf(binary->getLHS()) == variable)
    if (const auto k = state.constantOf(binary->getRHS()))
      return llvm::checkedMul(*k, std::int64_t{-1});
  return std::nullopt;
}

/// An integer parameter of `function` that nothing assigns.
static std::optional<std::uint32_t>
unmodifiedParameter(const clang::Expr *expr,
                    const clang::FunctionDecl &function,
                    const KindInferenceState &state) {
  const clang::ParmVarDecl *param = parameterOf(expr);
  if (param == nullptr || param->getDeclContext() != &function)
    return std::nullopt;
  if (const auto use = state.variableUses.find(param);
      use != state.variableUses.end() &&
      (use->second.assigned || use->second.modified))
    return std::nullopt;
  return param->getFunctionScopeIndex();
}

/// §7.5: `expr` as an affine term over `function`'s unmodified integer
/// parameters (at most one), with constant scale and offset.
static std::optional<core::ExtentTerm>
parameterTerm(const clang::Expr *expr, const clang::FunctionDecl &function,
              const KindInferenceState &state, unsigned depth = 0) {
  if (expr == nullptr || depth > 8)
    return std::nullopt;
  expr = expr->IgnoreParenImpCasts();
  if (const auto constant = state.constantOf(expr))
    return core::ExtentTerm::constant(*constant);
  if (const auto index = unmodifiedParameter(expr, function, state)) {
    if (!parameterOf(expr)->getType()->isIntegerType())
      return std::nullopt;
    return core::ExtentTerm::of(core::ExtentPath::ofParam(*index));
  }
  const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(expr);
  if (binary == nullptr)
    return std::nullopt;
  const auto lhs = parameterTerm(binary->getLHS(), function, state, depth + 1);
  const auto rhs = parameterTerm(binary->getRHS(), function, state, depth + 1);
  if (!lhs || !rhs)
    return std::nullopt;
  switch (binary->getOpcode()) {
  case clang::BO_Add:
  case clang::BO_Sub: {
    if (!rhs->isConstant() &&
        (binary->getOpcode() == clang::BO_Sub || !lhs->isConstant()))
      return std::nullopt;
    const core::ExtentTerm &variable = rhs->isConstant() ? *lhs : *rhs;
    const std::int64_t constant = rhs->isConstant() ? rhs->offset : lhs->offset;
    const auto offset = binary->getOpcode() == clang::BO_Add
                            ? llvm::checkedAdd(variable.offset, constant)
                            : llvm::checkedSub(variable.offset, constant);
    if (!offset)
      return std::nullopt;
    core::ExtentTerm out = variable;
    out.offset = *offset;
    return out;
  }
  case clang::BO_Mul: {
    if (!lhs->isConstant() && !rhs->isConstant())
      return std::nullopt;
    const core::ExtentTerm &variable = lhs->isConstant() ? *rhs : *lhs;
    const std::int64_t factor = lhs->isConstant() ? lhs->offset : rhs->offset;
    const auto scale = llvm::checkedMul(variable.scale, factor);
    const auto offset = llvm::checkedMul(variable.offset, factor);
    if (!scale || !offset)
      return std::nullopt;
    core::ExtentTerm out = variable;
    if (!out.isConstant())
      out.scale = *scale;
    out.offset = *offset;
    return out;
  }
  default:
    return std::nullopt;
  }
}

// -- Must-access conditions
// ------------------------------------------------------

namespace {

/// What checking one function's accesses needs.
struct MustAccessContext {
  const KindInferenceState &state;
  const clang::FunctionDecl &function;
  const FunctionCfg &cfg;
  const clang::ParentMap &parents;
};

/// A canonical counted loop `for (i = c; i < e; ++i)` (§7.5 R2).
struct CountedLoop {
  const clang::ForStmt *loop = nullptr;
  const clang::VarDecl *index = nullptr;
  std::int64_t start = 0;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::ExtentTerm end = {};
  /// `i <= e`.
  bool inclusive = false;

  [[nodiscard]] RequirementGuard guard() const {
    return RequirementGuard{
        .lhs = core::ExtentTerm::constant(start),
        .relation = inclusive ? RequirementGuard::Relation::LessEqual
                              : RequirementGuard::Relation::Less,
        .rhs = end};
  }
  /// False when the guard is constantly false: the loop never runs.
  [[nodiscard]] bool mayRun() const {
    if (!end.isConstant())
      return true;
    return inclusive ? start <= end.offset : start < end.offset;
  }
};

} // namespace

/// The blocks that may run before `block` on a path from the entry, or from
/// `stop` (included, not passed), never entering `excluded`.
static std::vector<const clang::CFGBlock *>
blocksBefore(const clang::CFGBlock *block,
             const llvm::DenseSet<const clang::CFGBlock *> &excluded,
             const clang::CFGBlock *stop) {
  std::vector<const clang::CFGBlock *> out;
  llvm::DenseSet<const clang::CFGBlock *> seen;
  seen.insert(block);
  std::vector<const clang::CFGBlock *> work;
  const auto push = [&](const clang::CFGBlock *from) {
    for (const clang::CFGBlock::AdjacentBlock &pred : from->preds()) {
      const clang::CFGBlock *previous = pred.getReachableBlock();
      if (previous != nullptr && !excluded.contains(previous) &&
          seen.insert(previous).second)
        work.push_back(previous);
    }
  };
  if (block != stop)
    push(block);
  while (!work.empty()) {
    const clang::CFGBlock *next = work.back();
    work.pop_back();
    out.push_back(next);
    if (next != stop)
      push(next);
  }
  return out;
}

/// Nothing among the first `limit` elements of `block` breaks a
/// must-access of `param`: a call not known to return, or a modification
/// of `param` other than `allowed`.
static bool cleanElements(const KindInferenceState &state,
                          const clang::CFGBlock &block, unsigned limit,
                          const clang::ParmVarDecl &param,
                          const clang::Stmt *allowed) {
  unsigned index = 0;
  for (const clang::CFGElement &element : block) {
    if (index++ >= limit)
      break;
    const auto stmt = element.getAs<clang::CFGStmt>();
    if (!stmt)
      continue;
    const clang::Stmt *each = stmt->getStmt();
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(each);
        call != nullptr && !state.knownToReturn(*call))
      return false;
    if (each != allowed && modifies(each, &param))
      return false;
  }
  return true;
}

/// §7.5's conditions on everything that may run before the first `limit`
/// elements of `block`.
static bool cleanBefore(const MustAccessContext &c,
                        const clang::ParmVarDecl &param,
                        const clang::CFGBlock *block, unsigned limit,
                        const llvm::DenseSet<const clang::CFGBlock *> &excluded,
                        const clang::CFGBlock *stop,
                        const clang::Stmt *allowed) {
  for (const clang::CFGBlock *before : blocksBefore(block, excluded, stop)) {
    if (!cleanElements(c.state, *before, ~0U, param, allowed))
      return false;
    if (testsNullness(llvm::dyn_cast_or_null<clang::Expr>(
                          before->getTerminatorCondition()),
                      param, c.state.context))
      return false;
  }
  return cleanElements(c.state, *block, limit, param, allowed);
}

/// The blocks of a loop: those the body's entry reaches without passing its
/// header.
static llvm::DenseSet<const clang::CFGBlock *>
loopBlocks(const clang::CFGBlock *header, const clang::CFGBlock *body) {
  llvm::DenseSet<const clang::CFGBlock *> out;
  out.insert(body);
  std::vector<const clang::CFGBlock *> work{body};
  while (!work.empty()) {
    const clang::CFGBlock *next = work.back();
    work.pop_back();
    for (const clang::CFGBlock::AdjacentBlock &succ : next->succs()) {
      const clang::CFGBlock *after = succ.getReachableBlock();
      if (after != nullptr && after != header && out.insert(after).second)
        work.push_back(after);
    }
  }
  return out;
}

/// An unguarded must-access at element `limit` of `block` (§7.5).
static bool mustAccessAlways(const MustAccessContext &c,
                             const clang::ParmVarDecl &param,
                             const clang::CFGBlock *block, unsigned limit) {
  if (!c.cfg.postDominates(block, &c.cfg.cfg->getEntry()))
    return false;
  return cleanBefore(c, param, block, limit, {}, nullptr, nullptr);
}

/// §7.5 inside a canonical loop: its header post-dominates the entry,
/// nothing before it breaks a must-access, every call in the loop returns,
/// `param` changes in the loop only by `allowed`, and the access at element
/// `limit` of `block` runs on every iteration (or is in the condition).
static bool mustAccessInLoop(const MustAccessContext &c,
                             const clang::ParmVarDecl &param,
                             const clang::Stmt &loop,
                             const clang::CFGBlock *block, unsigned limit,
                             const clang::Stmt *allowed) {
  const auto found = c.cfg.headers.find(&loop);
  if (found == c.cfg.headers.end())
    return false;
  const clang::CFGBlock *header = found->second;
  if (!c.cfg.postDominates(header, &c.cfg.cfg->getEntry()) ||
      header->succ_empty())
    return false;
  const clang::CFGBlock *body = header->succ_begin()->getReachableBlock();
  if (body == nullptr)
    return false;
  const llvm::DenseSet<const clang::CFGBlock *> inside =
      loopBlocks(header, body);
  if (!cleanBefore(c, param, header, header->size(), inside, nullptr,
                   allowed) ||
      testsNullness(
          llvm::dyn_cast_or_null<clang::Expr>(header->getTerminatorCondition()),
          param, c.state.context))
    return false;
  for (const clang::CFGBlock *each : inside)
    if (!cleanElements(c.state, *each, ~0U, param, allowed))
      return false;
  if (block == header)
    return true;
  if (!inside.contains(block) || !c.cfg.postDominates(block, body))
    return false;
  llvm::DenseSet<const clang::CFGBlock *> outside;
  outside.insert(header);
  for (const clang::CFGBlock *before : blocksBefore(block, outside, body))
    if (testsNullness(llvm::dyn_cast_or_null<clang::Expr>(
                          before->getTerminatorCondition()),
                      param, c.state.context))
      return false;
  (void)limit;
  return true;
}

// -- Canonical loops (§7.5)
// -------------------------------------------------------

/// The variable a loop's init sets and the value it sets it to:
/// `v = x` or `T v = x`.
static std::pair<const clang::VarDecl *, const clang::Expr *>
initialisation(const clang::Stmt *init) {
  if (const auto *assign = llvm::dyn_cast_or_null<clang::BinaryOperator>(init);
      assign != nullptr && assign->getOpcode() == clang::BO_Assign)
    return {variableOf(assign->getLHS()), assign->getRHS()};
  if (const auto *decl = llvm::dyn_cast_or_null<clang::DeclStmt>(init);
      decl != nullptr && decl->isSingleDecl())
    if (const auto *variable =
            llvm::dyn_cast<clang::VarDecl>(decl->getSingleDecl());
        variable != nullptr && variable->getInit() != nullptr)
      return {variable, variable->getInit()};
  return {nullptr, nullptr};
}

/// `v < e`, `e > v`, `v != e`, `e != v` (exclusive), `v <= e`, `e >= v`
/// (inclusive): the bound `e`.
static std::pair<const clang::Expr *, bool>
upperBound(const clang::Expr *condition, const clang::VarDecl *variable) {
  const auto *compare = llvm::dyn_cast_or_null<clang::BinaryOperator>(
      condition != nullptr ? condition->IgnoreParenImpCasts() : nullptr);
  if (compare == nullptr || variable == nullptr)
    return {nullptr, false};
  const bool left = variableOf(compare->getLHS()) == variable;
  const bool right = variableOf(compare->getRHS()) == variable;
  switch (compare->getOpcode()) {
  case clang::BO_LT:
    return {left ? compare->getRHS() : nullptr, false};
  case clang::BO_GT:
    return {right ? compare->getLHS() : nullptr, false};
  case clang::BO_NE:
    if (left)
      return {compare->getRHS(), false};
    return {right ? compare->getLHS() : nullptr, false};
  case clang::BO_LE:
    return {left ? compare->getRHS() : nullptr, true};
  case clang::BO_GE:
    return {right ? compare->getLHS() : nullptr, true};
  default:
    return {nullptr, false};
  }
}

/// §7.5 R2: `for (i = c; i < e; ++i)` with `c >= 0` constant, `e` affine
/// over parameters, no `break`, `return` or `goto` out of the body, and `i`
/// not otherwise modified in it.
static std::optional<CountedLoop> countedLoop(const clang::ForStmt &loop,
                                              const MustAccessContext &c) {
  const auto [index, start] = initialisation(loop.getInit());
  if (index == nullptr || index->hasGlobalStorage() ||
      !index->getType()->isIntegerType())
    return std::nullopt;
  const auto first = c.state.constantOf(start);
  if (!first || *first < 0)
    return std::nullopt;
  const auto [bound, inclusive] = upperBound(loop.getCond(), index);
  if (bound == nullptr)
    return std::nullopt;
  const auto end = parameterTerm(bound, c.function, c.state);
  if (!end || !incrementsByOne(loop.getInc(), index, c.state) ||
      leavesEarly(loop.getBody()) || modifiedIn(loop.getBody(), index))
    return std::nullopt;
  CountedLoop counted{.loop = &loop,
                      .index = index,
                      .start = *first,
                      .end = *end,
                      .inclusive = inclusive};
  if (!counted.mayRun())
    return std::nullopt;
  return counted;
}

/// A pointer parameter of the function that nothing assigns.
static const clang::ParmVarDecl *unmodifiedPointer(const clang::Expr *expr,
                                                   const MustAccessContext &c) {
  const clang::ParmVarDecl *param = parameterOf(expr);
  if (param == nullptr || param->getDeclContext() != &c.function ||
      !isObjectPointer(param->getType()))
    return nullptr;
  const auto use = c.state.variableUses.find(param);
  if (use != c.state.variableUses.end() &&
      (use->second.assigned || use->second.modified))
    return nullptr;
  return param;
}

namespace {
/// §7.5 R3: `for (x = p; x < q; ++x)` or `while (p < q) { ...; p++; }`.
struct RangeLoop {
  const clang::Stmt *loop = nullptr;
  const clang::VarDecl *cursor = nullptr;
  const clang::ParmVarDecl *start = nullptr;
  const clang::ParmVarDecl *end = nullptr;
  bool inclusive = false;
  /// The while form's closing increment of the parameter itself.
  const clang::Stmt *increment = nullptr;
};
} // namespace

static std::optional<RangeLoop> rangeLoop(const clang::Stmt &loop,
                                          const MustAccessContext &c) {
  RangeLoop range;
  range.loop = &loop;
  const clang::Expr *condition = nullptr;
  if (const auto *forLoop = llvm::dyn_cast<clang::ForStmt>(&loop)) {
    const auto [cursor, init] = initialisation(forLoop->getInit());
    const clang::ParmVarDecl *start =
        init != nullptr ? parameterOf(init) : nullptr;
    if (cursor == nullptr || cursor->hasGlobalStorage() || start == nullptr ||
        start->getDeclContext() != &c.function ||
        !isObjectPointer(start->getType()) ||
        !incrementsByOne(forLoop->getInc(), cursor, c.state) ||
        leavesEarly(forLoop->getBody()) ||
        modifiedIn(forLoop->getBody(), cursor))
      return std::nullopt;
    range.cursor = cursor;
    range.start = start;
    condition = forLoop->getCond();
  } else if (const auto *whileLoop = llvm::dyn_cast<clang::WhileStmt>(&loop)) {
    const auto *body =
        llvm::dyn_cast<clang::CompoundStmt>(whileLoop->getBody());
    const auto *compare = llvm::dyn_cast_or_null<clang::BinaryOperator>(
        whileLoop->getCond()->IgnoreParenImpCasts());
    if (body == nullptr || body->body_empty() || compare == nullptr)
      return std::nullopt;
    const clang::ParmVarDecl *start = parameterOf(compare->getLHS());
    if (start == nullptr || start->getDeclContext() != &c.function ||
        !isObjectPointer(start->getType()) ||
        !incrementsByOne(body->body_back(), start, c.state) ||
        leavesEarly(body) || modifiedIn(body, start, body->body_back()))
      return std::nullopt;
    range.cursor = start;
    range.start = start;
    range.increment = body->body_back();
    if (const auto *expr = llvm::dyn_cast<clang::Expr>(range.increment))
      range.increment = expr->IgnoreParenImpCasts();
    condition = compare;
  } else {
    return std::nullopt;
  }
  const auto [bound, inclusive] = upperBound(condition, range.cursor);
  range.end = bound != nullptr ? unmodifiedPointer(bound, c) : nullptr;
  if (range.end == nullptr || range.end == range.start ||
      !clang::ASTContext::hasSameUnqualifiedType(
          range.end->getType()->getPointeeType(),
          range.start->getType()->getPointeeType()))
    return std::nullopt;
  range.inclusive = inclusive;
  return range;
}

namespace {
/// §7.5 R4: `while (*s) s++`, `while (p[i]) i++`, `for (; *s; ++s)`.
struct Scan {
  const clang::Stmt *loop = nullptr;
  const clang::ParmVarDecl *param = nullptr;
  /// The condition's read of the element.
  const clang::Expr *read = nullptr;
  /// The increment of the parameter itself, when it is the cursor.
  const clang::Stmt *increment = nullptr;
};
} // namespace

/// Counts the statements that modify `variable` in `root`.
static unsigned modificationCount(const clang::Stmt *root,
                                  const clang::VarDecl *variable) {
  if (root == nullptr)
    return 0;
  unsigned count = modifies(root, variable) ? 1 : 0;
  for (const clang::Stmt *child : root->children())
    count += modificationCount(child, variable);
  return count;
}

/// A scan's condition `E`, `E != 0` or `0 != E`: the element read `E`.
static const clang::Expr *scannedElement(const clang::Expr *condition,
                                         const KindInferenceState &state) {
  if (condition == nullptr)
    return nullptr;
  condition = condition->IgnoreParenImpCasts();
  if (const auto *compare = llvm::dyn_cast<clang::BinaryOperator>(condition);
      compare != nullptr && compare->getOpcode() == clang::BO_NE) {
    if (state.constantOf(compare->getRHS()) == 0)
      return compare->getLHS()->IgnoreParenImpCasts();
    if (state.constantOf(compare->getLHS()) == 0)
      return compare->getRHS()->IgnoreParenImpCasts();
    return nullptr;
  }
  return condition;
}

static std::optional<Scan> scanLoop(const clang::Stmt &loop,
                                    const MustAccessContext &c) {
  const clang::Expr *condition = nullptr;
  const clang::Stmt *increment = nullptr;
  const clang::Stmt *body = nullptr;
  const clang::Stmt *init = nullptr;
  if (const auto *whileLoop = llvm::dyn_cast<clang::WhileStmt>(&loop)) {
    // The body is the increment alone.
    condition = whileLoop->getCond();
    increment = whileLoop->getBody();
    if (const auto *compound = llvm::dyn_cast<clang::CompoundStmt>(increment)) {
      if (compound->size() != 1)
        return std::nullopt;
      increment = compound->body_front();
    }
  } else if (const auto *forLoop = llvm::dyn_cast<clang::ForStmt>(&loop)) {
    condition = forLoop->getCond();
    increment = forLoop->getInc();
    body = forLoop->getBody();
    init = forLoop->getInit();
    if (leavesEarly(body))
      return std::nullopt;
  } else {
    return std::nullopt;
  }
  const clang::Expr *element = scannedElement(condition, c.state);
  const clang::Expr *base = nullptr;
  const clang::Expr *index = nullptr;
  if (const auto *unary = llvm::dyn_cast_or_null<clang::UnaryOperator>(element);
      unary != nullptr && unary->getOpcode() == clang::UO_Deref) {
    base = unary->getSubExpr();
  } else if (const auto *subscript =
                 llvm::dyn_cast_or_null<clang::ArraySubscriptExpr>(element)) {
    base = subscript->getBase();
    index = subscript->getIdx();
    if (c.state.constantOf(index) == 0)
      index = nullptr; // `s[0]`
  }
  const clang::VarDecl *cursor = base != nullptr ? variableOf(base) : nullptr;
  if (cursor == nullptr || cursor->hasGlobalStorage() ||
      !isObjectPointer(cursor->getType()))
    return std::nullopt;
  if (const auto *expr = llvm::dyn_cast_or_null<clang::Expr>(increment))
    increment = expr->IgnoreParenImpCasts();
  Scan scan{
      .loop = &loop, .param = nullptr, .read = element, .increment = nullptr};
  const auto *param = llvm::dyn_cast<clang::ParmVarDecl>(cursor);
  if (index != nullptr) {
    // `p[i]`: the parameter itself, and an index from zero.
    const clang::VarDecl *counter = variableOf(index);
    if (param == nullptr || counter == nullptr || counter->hasGlobalStorage() ||
        !incrementsByOne(increment, counter, c.state) ||
        modifiedIn(body, param))
      return std::nullopt;
    if (const auto [variable, value] = initialisation(init);
        variable != nullptr) {
      if (variable != counter || c.state.constantOf(value) != 0 ||
          modifiedIn(body, counter))
        return std::nullopt;
    } else if (init != nullptr || counter->getInit() == nullptr ||
               c.state.constantOf(counter->getInit()) != 0 ||
               modificationCount(c.function.getBody(), counter) != 1) {
      return std::nullopt;
    }
    scan.param = param;
  } else if (param != nullptr) {
    // `while (*p) p++`, `for (; *p; ++p)`: the parameter is the cursor.
    if (init != nullptr || !incrementsByOne(increment, param, c.state) ||
        modifiedIn(body, param))
      return std::nullopt;
    scan.param = param;
    scan.increment = increment;
  } else {
    // A local cursor that starts at the parameter.
    if (!incrementsByOne(increment, cursor, c.state))
      return std::nullopt;
    const clang::Expr *start = nullptr;
    if (const auto [variable, value] = initialisation(init);
        variable != nullptr) {
      if (variable != cursor || modifiedIn(body, cursor))
        return std::nullopt;
      start = value;
    } else if (init == nullptr &&
               modificationCount(c.function.getBody(), cursor) == 1) {
      start = cursor->getInit();
    }
    scan.param = start != nullptr ? parameterOf(start) : nullptr;
    if (scan.param == nullptr || modifiedIn(body, scan.param))
      return std::nullopt;
  }
  if (scan.param->getDeclContext() != &c.function ||
      !isObjectPointer(scan.param->getType()))
    return std::nullopt;
  return scan;
}

// -- Accesses
// ----------------------------------------------------------------------

namespace {
/// An access through a pointer variable (§7.5).
struct Access {
  /// The dereference, member access, subscript, or the call.
  const clang::Expr *expr = nullptr;
  const clang::VarDecl *base = nullptr;
  /// `*p`, `p->f`: an element; `p[e]`, `*(p + e)`: an index; a string
  /// argument of a `LibrarySpec` row (R4).
  enum class Form : std::uint8_t { Element, Index, String };
  Form form = Form::Element;
  const clang::Expr *index = nullptr;
  core::Nullability nullability = core::Nullability::Nonnull;
};
} // namespace

/// The lvalue only designates an object whose address is taken: `&p->f`,
/// or `p->a` decaying into a pointer nothing dereferences.
static bool addressOnly(const clang::Expr *expr,
                        const clang::ParentMap &parents) {
  const clang::Stmt *child = expr;
  const clang::Stmt *parent = parents.getParent(child);
  while (parent != nullptr) {
    const auto *member = llvm::dyn_cast<clang::MemberExpr>(parent);
    if (llvm::isa<clang::ParenExpr>(parent) ||
        (member != nullptr && !member->isArrow() &&
         member->getBase() == child)) {
      child = parent;
      parent = parents.getParent(parent);
      continue;
    }
    if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(parent);
        cast != nullptr &&
        cast->getCastKind() == clang::CK_ArrayToPointerDecay) {
      const clang::Stmt *user = parents.getParentIgnoreParens(parent);
      if (const auto *subscript =
              llvm::dyn_cast_or_null<clang::ArraySubscriptExpr>(user);
          subscript != nullptr &&
          subscript->getBase()->IgnoreParens() == parent)
        return false;
      if (const auto *unary =
              llvm::dyn_cast_or_null<clang::UnaryOperator>(user);
          unary != nullptr && unary->getOpcode() == clang::UO_Deref)
        return false;
      return true;
    }
    const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(parent);
    return unary != nullptr && unary->getOpcode() == clang::UO_AddrOf;
  }
  return false;
}

/// Every access in `stmt` through a variable `wanted` accepts, outside
/// unevaluated operands.
static void
collectAccesses(const clang::Stmt *stmt, const MustAccessContext &c,
                const std::function<bool(const clang::VarDecl *)> &wanted,
                std::vector<Access> &out) {
  if (stmt == nullptr || llvm::isa<clang::UnaryExprOrTypeTraitExpr>(stmt))
    return;
  const auto base = [&](const clang::Expr *expr) -> const clang::VarDecl * {
    const clang::VarDecl *variable = variableOf(expr);
    return variable != nullptr && wanted(variable) ? variable : nullptr;
  };
  const auto push = [&](const clang::Expr *expr, const clang::VarDecl *variable,
                        Access::Form form, const clang::Expr *index) {
    if (variable != nullptr && !addressOnly(expr, c.parents))
      out.push_back(Access{.expr = expr,
                           .base = variable,
                           .form = form,
                           .index = index,
                           .nullability = core::Nullability::Nonnull});
  };
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt);
      unary != nullptr && unary->getOpcode() == clang::UO_Deref) {
    const clang::Expr *sub = unary->getSubExpr()->IgnoreParenImpCasts();
    if (const clang::VarDecl *variable = base(sub)) {
      push(unary, variable, Access::Form::Element, nullptr);
    } else if (const auto *sum = llvm::dyn_cast<clang::BinaryOperator>(sub);
               sum != nullptr && sum->getOpcode() == clang::BO_Add) {
      if (const clang::VarDecl *left = base(sum->getLHS()))
        push(unary, left, Access::Form::Index, sum->getRHS());
      else if (const clang::VarDecl *right = base(sum->getRHS()))
        push(unary, right, Access::Form::Index, sum->getLHS());
    }
  } else if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(stmt);
             member != nullptr && member->isArrow()) {
    push(member, base(member->getBase()), Access::Form::Element, nullptr);
  } else if (const auto *subscript =
                 llvm::dyn_cast<clang::ArraySubscriptExpr>(stmt)) {
    push(subscript, base(subscript->getBase()), Access::Form::Index,
         subscript->getIdx());
  } else if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
    // R4: a string argument of a `LibrarySpec` row.
    const clang::FunctionDecl *callee = call->getDirectCallee();
    const auto match = callee != nullptr
                           ? governingLibraryEntry(*callee, c.state.library)
                           : std::nullopt;
    for (unsigned i = 0; match && i < call->getNumArgs(); ++i) {
      const core::LibraryParam *param = match->param(i);
      const clang::VarDecl *variable =
          param != nullptr && param->string
              ? base(stripPointerCasts(call->getArg(i)))
              : nullptr;
      if (variable != nullptr)
        out.push_back(Access{
            .expr = call,
            .base = variable,
            .form = Access::Form::String,
            .index = nullptr,
            .nullability = param->null == core::LibraryParam::Null::Forbidden
                               ? core::Nullability::Nonnull
                               : core::Nullability::Nullable});
    }
  }
  for (const clang::Stmt *child : stmt->children())
    collectAccesses(child, c, wanted, out);
}

/// The requirement an access gives (§7.5 R1, R2, R4, R5), under the guard
/// of the canonical loop it is in, if any.
static std::optional<MustAccessRequirement>
requirementOf(const Access &access, const CountedLoop *loop,
              const MustAccessContext &c) {
  MustAccessRequirement out;
  out.access = access.expr;
  if (loop != nullptr)
    out.guard = loop->guard();
  const auto inferred = [](core::PointerKind kind) {
    kind.source = core::KindSource::Inferred;
    return kind;
  };
  switch (access.form) {
  case Access::Form::Element:
    out.kind = inferred(core::PointerKind::single(core::Nullability::Nonnull));
    out.rule = MustAccessRule::R1;
    return out;
  case Access::Form::String:
    out.kind = inferred(core::PointerKind::nulTerminated(access.nullability));
    out.rule = MustAccessRule::R4;
    return out;
  case Access::Form::Index:
    break;
  }
  if (loop != nullptr) {
    if (const auto k = offsetFrom(access.index, loop->index, c.state)) {
      // `p[i + k]` for `c <= i < e`: `e + k` elements (`e + k + 1` for
      // `i <= e`), as mathematical integers.
      core::ExtentTerm count = loop->end;
      auto offset = llvm::checkedAdd(count.offset, *k);
      if (offset && loop->inclusive)
        offset = llvm::checkedAdd(*offset, std::int64_t{1});
      if (!offset)
        return std::nullopt;
      count.offset = *offset;
      if (count.isConstant() && count.offset <= 0)
        return std::nullopt;
      out.kind = inferred(
          core::PointerKind::counted(count, core::Nullability::Nonnull));
      out.rule = MustAccessRule::R2;
      return out;
    }
  }
  if (const auto constant = c.state.constantOf(access.index)) {
    if (*constant < 0)
      return std::nullopt;
    out.kind = inferred(
        *constant == 0 ? core::PointerKind::single(core::Nullability::Nonnull)
                       : core::PointerKind::counted(
                             core::ExtentTerm::constant(*constant + 1),
                             core::Nullability::Nonnull));
    out.rule = *constant == 0 ? MustAccessRule::R1 : MustAccessRule::R5;
    return out;
  }
  auto term = parameterTerm(access.index, c.function, c.state);
  if (!term)
    return std::nullopt;
  const auto offset = llvm::checkedAdd(term->offset, std::int64_t{1});
  if (!offset)
    return std::nullopt;
  term->offset = *offset;
  out.kind =
      inferred(core::PointerKind::counted(*term, core::Nullability::Nonnull));
  out.rule = MustAccessRule::R5;
  return out;
}

/// The `for` loop whose body holds `stmt` when that is its innermost loop.
static const clang::ForStmt *enclosingForBody(const clang::Stmt *stmt,
                                              const clang::ParentMap &parents) {
  const clang::Stmt *child = stmt;
  for (const clang::Stmt *parent = parents.getParent(child); parent != nullptr;
       child = parent, parent = parents.getParent(parent)) {
    if (const auto *loop = llvm::dyn_cast<clang::ForStmt>(parent))
      return loop->getBody() == child ? loop : nullptr;
    if (llvm::isa<clang::WhileStmt>(parent) || llvm::isa<clang::DoStmt>(parent))
      return nullptr;
  }
  return nullptr;
}

/// Every `for` and `while` loop in `stmt`.
static void loopsIn(const clang::Stmt *stmt,
                    std::vector<const clang::Stmt *> &out) {
  if (stmt == nullptr)
    return;
  if (llvm::isa<clang::ForStmt>(stmt) || llvm::isa<clang::WhileStmt>(stmt))
    out.push_back(stmt);
  for (const clang::Stmt *child : stmt->children())
    loopsIn(child, out);
}

// -- The requirements
// --------------------------------------------------------------

void KindInferenceState::inferMustAccess() {
  const clang::SourceManager &sm = context.getSourceManager();
  for (const clang::FunctionDecl *definition : definitions) {
    if (sm.isInSystemHeader(definition->getLocation()))
      continue;
    llvm::DenseSet<const clang::VarDecl *> params;
    for (const clang::ParmVarDecl *param : definition->parameters()) {
      const auto use = variableUses.find(param);
      if (isObjectPointer(param->getType()) &&
          (use == variableUses.end() || !use->second.addressTaken))
        params.insert(param);
    }
    if (params.empty())
      continue;
    const FunctionCfg *cfg = cfgOf(*definition);
    if (cfg == nullptr)
      continue;
    const MustAccessContext c{.state = *this,
                              .function = *definition,
                              .cfg = *cfg,
                              .parents = parentsOf(*definition)};
    llvm::DenseMap<const clang::ParmVarDecl *,
                   std::vector<MustAccessRequirement>>
        found;
    const auto add = [&](const clang::ParmVarDecl *param,
                         MustAccessRequirement requirement) {
      std::vector<MustAccessRequirement> &list = found[param];
      if (llvm::none_of(list, [&](const MustAccessRequirement &each) {
            return each.kind == requirement.kind &&
                   each.guard == requirement.guard;
          }))
        list.push_back(std::move(requirement));
    };
    const auto position = [&](const clang::Stmt *stmt)
        -> std::optional<std::pair<const clang::CFGBlock *, unsigned>> {
      const auto where = cfg->where.find(stmt);
      if (where == cfg->where.end())
        return std::nullopt;
      return where->second;
    };

    // R1, R2, R5, and R4 through `LibrarySpec` strings.
    std::vector<Access> accesses;
    collectAccesses(
        definition->getBody(), c,
        [&](const clang::VarDecl *variable) {
          return params.contains(variable);
        },
        accesses);
    for (const Access &access : accesses) {
      const auto *param = llvm::cast<clang::ParmVarDecl>(access.base);
      const auto at = position(access.expr);
      if (!at)
        continue;
      if (mustAccessAlways(c, *param, at->first, at->second)) {
        if (auto requirement = requirementOf(access, nullptr, c))
          add(param, std::move(*requirement));
        continue;
      }
      const clang::ForStmt *loop = enclosingForBody(access.expr, c.parents);
      const auto counted =
          loop != nullptr ? countedLoop(*loop, c) : std::nullopt;
      if (!counted ||
          !mustAccessInLoop(c, *param, *loop, at->first, at->second, nullptr))
        continue;
      if (auto requirement = requirementOf(access, &*counted, c))
        add(param, std::move(*requirement));
    }

    // R3 and R4 scans.
    std::vector<const clang::Stmt *> loops;
    loopsIn(definition->getBody(), loops);
    for (const clang::Stmt *loop : loops) {
      if (const auto range = rangeLoop(*loop, c);
          range && params.contains(range->start)) {
        const clang::Stmt *body =
            llvm::isa<clang::ForStmt>(loop)
                ? llvm::cast<clang::ForStmt>(loop)->getBody()
                : llvm::cast<clang::WhileStmt>(loop)->getBody();
        std::vector<Access> through;
        collectAccesses(
            body, c,
            [&](const clang::VarDecl *variable) {
              return variable == range->cursor;
            },
            through);
        for (const Access &access : through) {
          const auto at = position(access.expr);
          const bool element = access.form == Access::Form::Element ||
                               (access.form == Access::Form::Index &&
                                constantOf(access.index) == 0);
          if (!element || !at ||
              !mustAccessInLoop(c, *range->start, *loop, at->first, at->second,
                                range->increment))
            continue;
          add(range->start,
              MustAccessRequirement{
                  .kind = core::PointerKind::endedBy(
                      core::ExtentPath::ofParam(
                          range->end->getFunctionScopeIndex()),
                      range->inclusive ? 1 : 0, core::Nullability::Nonnull,
                      core::KindSource::Inferred),
                  .guard =
                      RequirementGuard{
                          .lhs = core::ExtentTerm::of(core::ExtentPath::ofParam(
                              range->start->getFunctionScopeIndex())),
                          .relation =
                              range->inclusive
                                  ? RequirementGuard::Relation::LessEqual
                                  : RequirementGuard::Relation::Less,
                          .rhs = core::ExtentTerm::of(core::ExtentPath::ofParam(
                              range->end->getFunctionScopeIndex()))},
                  .rule = MustAccessRule::R3,
                  .access = access.expr});
          break;
        }
      }
      if (const auto scan = scanLoop(*loop, c);
          scan && params.contains(scan->param)) {
        const auto at = position(scan->read);
        if (at && mustAccessInLoop(c, *scan->param, *loop, at->first,
                                   at->second, scan->increment))
          add(scan->param,
              MustAccessRequirement{
                  .kind = core::PointerKind::nulTerminated(
                      core::Nullability::Nonnull, core::KindSource::Inferred),
                  .guard = std::nullopt,
                  .rule = MustAccessRule::R4,
                  .access = scan->read});
      }
    }

    // §7.5 enforcement: at every direct call of a static function whose
    // address is not taken; otherwise as the caller's contract.
    const auto facts = functions.find(definition->getCanonicalDecl());
    const bool local =
        !definition->isExternallyVisible() &&
        (facts == functions.end() || !facts->second.addressTaken);
    for (auto &[param, requirements] : found) {
      std::ranges::stable_sort(
          requirements, [&sm](const MustAccessRequirement &a,
                              const MustAccessRequirement &b) {
            const clang::SourceLocation left = a.access->getBeginLoc();
            const clang::SourceLocation right = b.access->getBeginLoc();
            return left != right && sm.isBeforeInTranslationUnit(left, right);
          });
      const unsigned index = param->getFunctionScopeIndex();
      const KindEntry *existing = kinds.param(*definition, index);
      KindEntry entry = existing != nullptr ? *existing : KindEntry{};
      entry.mustAccess = std::move(requirements);
      entry.enforcement = local ? RequirementEnforcement::CallSites
                                : RequirementEnforcement::CallerContract;
      kinds.setParam(*definition, index, std::move(entry));
    }
  }
}

} // namespace weavec::analysis
