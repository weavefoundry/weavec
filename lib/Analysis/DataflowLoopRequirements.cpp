//===- DataflowLoopRequirements.cpp - Loop boundaries (RFC 0017) ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

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

bool FunctionDataflow::loopBoundaryEligible(const core::Affine &need) {
  const auto *index = need.place ? builder.varForPlace(*need.place) : nullptr;
  if (!index || !index->hasLocalStorage() || isa<ParmVarDecl>(index) ||
      index->getType().isVolatileQualified() ||
      index->getType()->isAtomicType() || addressTaken.contains(index))
    return false;
  const auto [cached, inserted] = loopBoundaryEligibility.emplace(index, false);
  if (!inserted)
    return cached->second;
  const auto indexType = integerTypeOf(*index, context);
  if (!indexType || indexType->isBoolean)
    return false;

  std::vector<const Stmt *> statements;
  if (!collectLoopRequirementStatements(function.getBody(), statements))
    return false;
  const ForStmt *loop = nullptr;
  for (const auto *stmt : statements) {
    // A jump elsewhere in the function can enter the body past initialization.
    if (isa<GotoStmt, IndirectGotoStmt, AsmStmt>(stmt))
      return false;
    const auto *candidate = dyn_cast<ForStmt>(stmt);
    const Expr *initial = nullptr;
    if (!candidate || loopRequirementIndex(*candidate, initial) != index)
      continue;
    if (loop || !initial || initial->HasSideEffects(context) ||
        integerConstant(*initial, context) != 0)
      return false;
    loop = candidate;
  }
  if (!loop || !loop->getCond() || !loop->getInc())
    return false;
  const auto *increment =
      dyn_cast<UnaryOperator>(loop->getInc()->IgnoreParens());
  if (!increment || !increment->isIncrementOp() ||
      loopRequirementVariable(increment->getSubExpr()) != index)
    return false;
  std::set<const VarDecl *> inputs{index};
  bool incrementFits = false;
  unsigned remaining = MaxLoopRequirementConditionNodes;
  if (!canonicalLoopRequirementCondition(*loop->getCond(), *index, *indexType,
                                         context, addressTaken, inputs,
                                         incrementFits, remaining) ||
      !incrementFits)
    return false;

  std::vector<const Stmt *> loopStatements;
  if (!collectLoopRequirementStatements(loop, loopStatements))
    return false;
  const llvm::DenseSet<const Stmt *> inside(loopStatements.begin(),
                                            loopStatements.end());
  for (const auto *stmt : statements) {
    const auto *ref = dyn_cast<DeclRefExpr>(stmt);
    // The call site has no AST access location: do not grant an index blanket
    // eligibility for uses after this loop or in another loop using the local.
    if (ref && ref->getDecl()->getCanonicalDecl() == index &&
        !inside.contains(stmt))
      return false;
  }

  std::vector<const Stmt *> body;
  if (!collectLoopRequirementStatements(loop->getBody(), body))
    return false;
  for (const auto *stmt : body) {
    // Reject the whole boundary export, including an access before an exit.
    // Conditional accesses and nested loops need path/iteration reasoning that
    // this syntactic eligibility check deliberately does not attempt.
    if (isa<BreakStmt, ContinueStmt, ReturnStmt, GotoStmt, IndirectGotoStmt,
            IfStmt, SwitchStmt, ForStmt, WhileStmt, DoStmt, LabelStmt, AsmStmt,
            CallExpr, AbstractConditionalOperator, StmtExpr>(stmt))
      return false;
    if (const auto *binary = dyn_cast<BinaryOperator>(stmt)) {
      if (binary->isLogicalOp())
        return false;
      if (binary->isAssignmentOp()) {
        const auto *target = loopRequirementVariable(binary->getLHS());
        if (target &&
            (inputs.contains(target) || target->getType()->isPointerType()))
          return false;
      }
    }
    if (const auto *unary = dyn_cast<UnaryOperator>(stmt);
        unary && unary->isIncrementDecrementOp()) {
      const auto *target = loopRequirementVariable(unary->getSubExpr());
      if (target &&
          (inputs.contains(target) || target->getType()->isPointerType()))
        return false;
    }
  }
  cached->second = true;
  return true;
}

} // namespace weavec::analysis
