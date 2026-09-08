//===- DataflowSafetyLoops.cpp - Sufficient loop bounds (RFC 0018)
//----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"
#include "IntegerSupport.h"

using namespace clang;

namespace weavec::analysis {

// Eligibility is a bounded syntax check, not induction over the CFG. Exhausting
// this budget fails projection through noteExtentRequirement's existing path.
static constexpr std::size_t MaxLoopRequirementStatements = 4096;
static constexpr unsigned MaxLoopRequirementConditionNodes = 64;

static bool
collectLoopRequirementStatements(const Stmt *root,
                                 std::vector<const Stmt *> &statements) {
  if (root)
    statements.push_back(root);
  for (std::size_t i = 0; i < statements.size(); ++i) {
    for (const auto *child : statements[i]->children()) {
      if (!child)
        continue;
      if (statements.size() == MaxLoopRequirementStatements)
        return false;
      statements.push_back(child);
    }
  }
  return true;
}

static const VarDecl *loopRequirementVariable(const Expr *expr) {
  const auto *ref =
      expr ? dyn_cast<DeclRefExpr>(expr->IgnoreParenImpCasts()) : nullptr;
  const auto *var = ref ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
  return var ? var->getCanonicalDecl() : nullptr;
}

static const VarDecl *loopRequirementIndex(const ForStmt &loop,
                                           const Expr *&initial) {
  if (const auto *decl = dyn_cast_or_null<DeclStmt>(loop.getInit());
      decl && decl->isSingleDecl()) {
    const auto *var = dyn_cast<VarDecl>(decl->getSingleDecl());
    initial = var ? var->getInit() : nullptr;
    return var ? var->getCanonicalDecl() : nullptr;
  }
  const auto *expr = dyn_cast_or_null<Expr>(loop.getInit());
  const auto *assignment =
      expr ? dyn_cast<BinaryOperator>(expr->IgnoreParens()) : nullptr;
  if (!assignment || assignment->getOpcode() != BO_Assign)
    return nullptr;
  initial = assignment->getRHS();
  return loopRequirementVariable(assignment->getLHS());
}

// Only scalar locals and parameters can be stable without tracking heap writes.
// Explicit minimum expressions are supported; arithmetic bounds are left to a
// later extension rather than assuming that their evaluation cannot overflow.
static bool stableLoopRequirementBound(
    const Expr &expr, const VarDecl &index, ASTContext &context,
    const llvm::DenseSet<const VarDecl *> &addressTaken,
    std::set<const VarDecl *> &inputs, unsigned &remaining) {
  if (remaining == 0)
    return false;
  --remaining;
  if (integerConstant(expr, context))
    return !expr.HasSideEffects(context);
  const Expr *value = expr.IgnoreParenImpCasts();
  if (const auto *var = loopRequirementVariable(value)) {
    if (var == &index || !var->hasLocalStorage() ||
        var->getType().isVolatileQualified() ||
        var->getType()->isAtomicType() || !var->getType()->isIntegerType() ||
        addressTaken.contains(var))
      return false;
    inputs.insert(var);
    return true;
  }
  if (const auto *conditional = dyn_cast<ConditionalOperator>(value))
    return stableLoopRequirementBound(*conditional->getCond(), index, context,
                                      addressTaken, inputs, remaining) &&
           stableLoopRequirementBound(*conditional->getTrueExpr(), index,
                                      context, addressTaken, inputs,
                                      remaining) &&
           stableLoopRequirementBound(*conditional->getFalseExpr(), index,
                                      context, addressTaken, inputs, remaining);
  const auto *comparison = dyn_cast<BinaryOperator>(value);
  return comparison != nullptr && comparison->isComparisonOp() &&
         stableLoopRequirementBound(*comparison->getLHS(), index, context,
                                    addressTaken, inputs, remaining) &&
         stableLoopRequirementBound(*comparison->getRHS(), index, context,
                                    addressTaken, inputs, remaining);
}

static bool canonicalLoopRequirementCondition(
    const Expr &expr, const VarDecl &index, core::IntegerType indexType,
    ASTContext &context, const llvm::DenseSet<const VarDecl *> &addressTaken,
    std::set<const VarDecl *> &inputs, bool &incrementFits,
    unsigned &remaining) {
  if (remaining == 0)
    return false;
  --remaining;
  const auto *condition = dyn_cast<BinaryOperator>(expr.IgnoreParens());
  if (!condition)
    return false;
  if (condition->getOpcode() == BO_LAnd)
    return canonicalLoopRequirementCondition(
               *condition->getLHS(), index, indexType, context, addressTaken,
               inputs, incrementFits, remaining) &&
           canonicalLoopRequirementCondition(*condition->getRHS(), index,
                                             indexType, context, addressTaken,
                                             inputs, incrementFits, remaining);
  const auto op = condition->getOpcode();
  if ((op != BO_LT && op != BO_LE) ||
      loopRequirementVariable(condition->getLHS()) != &index ||
      !stableLoopRequirementBound(*condition->getRHS(), index, context,
                                  addressTaken, inputs, remaining))
    return false;

  const auto maximum = *core::IntegerRange::full(indexType).maximum();
  const auto comparisonType =
      integerTypeOf(condition->getLHS()->getType(), context);
  const auto nonnegative = core::IntegerRange::between(
      core::IntegerValue::ofBits(indexType, 0), maximum);
  if (!comparisonType || !conversionPreserves(nonnegative, *comparisonType))
    return false;

  // At least one conjunct must stop before ++ can wrap or overflow. In
  // particular, `unsigned char i; i < unsigned_n` alone is insufficient.
  const auto boundType = integerTypeOf(condition->getRHS()->getType(), context);
  if (!boundType)
    return false;
  auto upper = core::IntegerRange::full(*boundType).maximum()->bits;
  if (const auto constant = integerConstant(*condition->getRHS(), context)) {
    if (*constant < 0)
      return false;
    upper = static_cast<std::uint64_t>(*constant);
  }
  incrementFits |= op == BO_LT ? upper <= maximum.bits : upper < maximum.bits;
  return true;
}

std::optional<core::PathAffine> FunctionDataflow::checkedLoopRequirement(
    const core::Affine &need, const Stmt &at, core::AnalysisState &state) {
  (void)state;
  const auto found = checkedLoops.find(&at);
  if (found == checkedLoops.end() || !need.place || need.scale <= 0)
    return std::nullopt;
  const auto *index = builder.varForPlace(*need.place);
  const auto &loop = *found->second;
  const Expr *initial = nullptr;
  if (!index || loopRequirementIndex(loop, initial) != index || !initial ||
      initial->HasSideEffects(context) ||
      integerConstant(*initial, context) != 0 || addressTaken.contains(index) ||
      !loop.getCond() || !loop.getInc())
    return std::nullopt;
  const auto *inc = dyn_cast<UnaryOperator>(loop.getInc()->IgnoreParens());
  const auto type = integerTypeOf(*index, context);
  if (!inc || !inc->isIncrementOp() ||
      loopRequirementVariable(inc->getSubExpr()) != index || !type)
    return std::nullopt;
  bool fits = false;
  unsigned remaining = MaxLoopRequirementConditionNodes;
  std::set<const VarDecl *> inputs{index};
  if (!canonicalLoopRequirementCondition(*loop.getCond(), *index, *type,
                                         context, addressTaken, inputs, fits,
                                         remaining) ||
      !fits)
    return std::nullopt;
  std::vector<const Stmt *> statements;
  if (!collectLoopRequirementStatements(loop.getBody(), statements))
    return std::nullopt;
  for (const auto *stmt : statements) {
    if (isa<GotoStmt, IndirectGotoStmt, AsmStmt, CallExpr, ForStmt, WhileStmt,
            DoStmt>(stmt))
      return std::nullopt;
    const Expr *written = nullptr;
    if (const auto *b = dyn_cast<BinaryOperator>(stmt);
        b && b->isAssignmentOp())
      written = b->getLHS();
    if (const auto *u = dyn_cast<UnaryOperator>(stmt);
        u && u->isIncrementDecrementOp())
      written = u->getSubExpr();
    if (written) {
      const auto *var = loopRequirementVariable(written);
      if ((var && (inputs.contains(var) || var->getType()->isPointerType())) ||
          (!var && written->getType()->isPointerType()))
        return std::nullopt;
    }
  }
  const auto *condition =
      dyn_cast<BinaryOperator>(loop.getCond()->IgnoreParens());
  if (!condition || condition->getOpcode() != BO_LT)
    return std::nullopt;
  const auto bound = builder.affineOf(*condition->getRHS());
  if (!bound)
    return std::nullopt;
  const auto scaled = bound->times(need.scale);
  std::int64_t shift = 0;
  if (__builtin_sub_overflow(need.constant, need.scale, &shift))
    return std::nullopt;
  const auto end = scaled ? scaled->shifted(shift) : std::nullopt;
  return summaryAffineOf(end);
}

// RFC 0019: only the normal exit of an unconditional, unit-stride store
// establishes a prefix. The sufficient-requirement recognizer alone is not a
// must-write proof: break/continue and conditional stores are rejected here.
void FunctionDataflow::checkedLoopExit(const ForStmt &loop,
                                       core::AnalysisState &state) {
  const Expr *initial = nullptr;
  const auto *index = loopRequirementIndex(loop, initial);
  const auto *condition =
      loop.getCond() ? dyn_cast<BinaryOperator>(loop.getCond()->IgnoreParens())
                     : nullptr;
  if (!index || !condition || condition->getOpcode() != BO_LT ||
      loopRequirementVariable(condition->getLHS()) != index)
    return;
  std::vector<const Stmt *> statements;
  if (!collectLoopRequirementStatements(loop.getBody(), statements))
    return;
  const BinaryOperator *store = nullptr;
  for (const auto *stmt : statements) {
    if (isa<IfStmt, SwitchStmt, BreakStmt, ContinueStmt, ReturnStmt, GotoStmt,
            IndirectGotoStmt, ConditionalOperator, CallExpr, ForStmt, WhileStmt,
            DoStmt, AsmStmt>(stmt))
      return;
    if (const auto *unary = dyn_cast<UnaryOperator>(stmt);
        unary && unary->isIncrementDecrementOp())
      return;
    if (const auto *binary = dyn_cast<BinaryOperator>(stmt)) {
      if (binary->isLogicalOp())
        return;
      if (binary->isAssignmentOp()) {
        if (store || binary->getOpcode() != BO_Assign)
          return;
        store = binary;
      }
    }
  }
  const auto *subscript =
      store
          ? dyn_cast<ArraySubscriptExpr>(store->getLHS()->IgnoreParenImpCasts())
          : nullptr;
  if (!subscript || loopRequirementVariable(subscript->getIdx()) != index)
    return;
  const auto unit = byteSizeOf(subscript->getType(), context);
  // Keep the stable bound's identity across every visit, including the
  // zero-iteration exit. Flow simplification can otherwise substitute the
  // loop index, whose lifetime ends immediately after this edge.
  std::optional<core::Affine> bound;
  if (const auto constant = integerConstant(*condition->getRHS(), context))
    bound = core::Affine::ofConstant(*constant);
  else if (const auto *variable = loopRequirementVariable(condition->getRHS()))
    bound = core::Affine::ofPlace(builder.placeForVar(*variable));
  const auto end = unit && bound ? bound->times(*unit) : std::nullopt;
  if (!end)
    return;
  const auto next = core::Affine{
      .place = builder.placeForVar(*index), .scale = *unit, .constant = *unit};
  if (!checkedLoopRequirement(next, *subscript, state))
    return;
  if (const auto memory =
          checkedMemory(*subscript->getBase(), {}, *end, state)) {
    state.safety->initialize(memory->storage,
                             {.begin = memory->begin, .end = memory->end});
  }
}

} // namespace weavec::analysis
