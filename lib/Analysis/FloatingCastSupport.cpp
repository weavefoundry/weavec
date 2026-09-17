//===- FloatingCastSupport.cpp - Finite conversions (RFC 0029) ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "FloatingCastSupport.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMapContext.h"

#include "llvm/ADT/APSInt.h"

#include <vector>

using namespace clang;
namespace weavec::analysis {

static bool standardFloatingMode(const Expr &expression,
                                 const ASTContext &context) {
  const auto options = expression.getFPFeaturesInEffect(context.getLangOpts());
  return !options.isFPConstrained() && !options.getNoHonorNaNs() &&
         !options.getNoHonorInfs() && !options.getAllowFPReassociate() &&
         options.getFPEvalMethod() == LangOptions::FEM_Source;
}

static const VarDecl *floatingVariable(const Expr *expression) {
  const auto *reference =
      dyn_cast<DeclRefExpr>(expression->IgnoreParenImpCasts());
  return reference ? dyn_cast<VarDecl>(reference->getDecl()) : nullptr;
}

static bool unchangedFloatingVariable(const VarDecl &variable,
                                      const FunctionDecl &function,
                                      bool allowAssignments) {
  if (!variable.hasLocalStorage() || variable.getType().isVolatileQualified() ||
      !variable.getType()->isRealFloatingType())
    return false;
  std::vector<const Stmt *> pending{function.getBody()};
  for (std::size_t i = 0; i < pending.size(); ++i) {
    const auto *statement = pending[i];
    // Lexical true branches are not dominance proofs in the presence of a
    // jump into their bodies, including switch labels nested inside an if.
    if (isa<IndirectGotoStmt, AsmStmt>(statement) ||
        (!allowAssignments && isa<GotoStmt, LabelStmt, SwitchStmt>(statement)))
      return false;
    if (const auto *unary = dyn_cast<UnaryOperator>(statement);
        unary &&
        (unary->isIncrementDecrementOp() || unary->getOpcode() == UO_AddrOf) &&
        floatingVariable(unary->getSubExpr()) == &variable)
      return false;
    if (const auto *binary = dyn_cast<BinaryOperator>(statement);
        binary && binary->isAssignmentOp() &&
        floatingVariable(binary->getLHS()) == &variable && !allowAssignments)
      return false;
    for (const auto *child : statement->children()) {
      if (!child)
        continue;
      if (pending.size() == 65536)
        return false;
      pending.push_back(child);
    }
  }
  return true;
}

static bool unchangedFloatingBranch(const Stmt &branch,
                                    const VarDecl &variable) {
  std::vector<const Stmt *> pending{&branch};
  for (std::size_t i = 0; i < pending.size(); ++i) {
    const auto *statement = pending[i];
    if (isa<LabelStmt, SwitchCase, AsmStmt>(statement))
      return false;
    if (const auto *binary = dyn_cast<BinaryOperator>(statement);
        binary && binary->isAssignmentOp() &&
        floatingVariable(binary->getLHS()) == &variable)
      return false;
    if (const auto *unary = dyn_cast<UnaryOperator>(statement);
        unary && unary->isIncrementDecrementOp() &&
        floatingVariable(unary->getSubExpr()) == &variable)
      return false;
    for (const auto *child : statement->children()) {
      if (!child)
        continue;
      if (pending.size() == 65536)
        return false;
      pending.push_back(child);
    }
  }
  return true;
}

static bool representableFloatingEndpoint(const llvm::APFloat &value,
                                          QualType target,
                                          const ASTContext &context) {
  if (!value.isFinite())
    return false;
  llvm::APSInt integer(context.getIntWidth(target),
                       !target->isSignedIntegerType());
  bool exact = false;
  const auto status =
      value.convertToInteger(integer, llvm::APFloat::rmTowardZero, &exact);
  // Discarding a fractional part is the defined C conversion. Invalid or
  // overflowing endpoints cannot justify any conversion in the interval.
  return status == llvm::APFloat::opOK || status == llvm::APFloat::opInexact;
}

static void floatingBounds(const Expr &condition, const VarDecl &variable,
                           QualType target, ASTContext &context, bool &lower,
                           bool &upper, unsigned &remaining, bool conditionTrue,
                           bool excludesNan) {
  if (remaining == 0)
    return;
  --remaining;
  const auto *binary =
      dyn_cast<BinaryOperator>(condition.IgnoreParenImpCasts());
  if (!binary || !standardFloatingMode(*binary, context))
    return;
  if ((conditionTrue && binary->getOpcode() == BO_LAnd) ||
      (!conditionTrue && excludesNan && binary->getOpcode() == BO_LOr)) {
    floatingBounds(*binary->getLHS(), variable, target, context, lower, upper,
                   remaining, conditionTrue, excludesNan);
    floatingBounds(*binary->getRHS(), variable, target, context, lower, upper,
                   remaining, conditionTrue, excludesNan);
    return;
  }
  if (!binary->isComparisonOp() || (!conditionTrue && !excludesNan))
    return;
  auto operation = binary->getOpcode();
  if (!conditionTrue)
    operation = BinaryOperator::negateComparisonOp(operation);
  const Expr *constant = binary->getRHS();
  if (floatingVariable(binary->getLHS()) != &variable) {
    if (floatingVariable(binary->getRHS()) != &variable)
      return;
    constant = binary->getLHS();
    operation = BinaryOperator::reverseComparisonOp(operation);
  }
  llvm::APFloat endpoint(0.0);
  if (!constant->EvaluateAsFloat(endpoint, context) || !endpoint.isFinite())
    return;
  if (operation == BO_LT)
    endpoint.next(true);
  if (operation == BO_GT)
    endpoint.next(false);
  if (!representableFloatingEndpoint(endpoint, target, context))
    return;
  lower |= operation == BO_GT || operation == BO_GE || operation == BO_EQ;
  upper |= operation == BO_LT || operation == BO_LE || operation == BO_EQ;
}

bool finiteFloatingCast(const CastExpr &cast, const FunctionDecl &function,
                        ASTContext &context, bool excludesNan) {
  if (cast.getCastKind() != CK_FloatingToIntegral ||
      !cast.getType()->isIntegerType() || context.getLangOpts().FastMath ||
      context.getLangOpts().RoundingMath ||
      !standardFloatingMode(cast, context))
    return false;
  llvm::APFloat constant(0.0);
  if (cast.getSubExpr()->EvaluateAsFloat(constant, context))
    return representableFloatingEndpoint(constant, cast.getType(), context);
  const auto *variable = floatingVariable(cast.getSubExpr());
  if (!variable ||
      !ASTContext::hasSameUnqualifiedType(variable->getType(),
                                          cast.getSubExpr()->getType()) ||
      !unchangedFloatingVariable(*variable, function, excludesNan))
    return false;
  bool lower = false;
  bool upper = false;
  const bool earlyReturns =
      unchangedFloatingVariable(*variable, function, false);
  const Stmt *child = &cast;
  unsigned remaining = 256;
  for (unsigned depth = 0; depth < 128; ++depth) {
    const auto parents = context.getParents(*child);
    if (parents.size() != 1)
      break;
    const auto *parent = parents[0].get<Stmt>();
    if (!parent)
      break;
    if (const auto *compound = dyn_cast<CompoundStmt>(parent);
        compound && earlyReturns)
      for (const auto *sibling : compound->body()) {
        if (sibling == child)
          break;
        const auto *branch = dyn_cast<IfStmt>(sibling);
        if (!branch || branch->getElse())
          continue;
        const Stmt *arm = branch->getThen();
        if (const auto *body = dyn_cast<CompoundStmt>(arm);
            body && body->size() == 1)
          arm = *body->body_begin();
        if (isa<ReturnStmt>(arm))
          floatingBounds(*branch->getCond(), *variable, cast.getType(), context,
                         lower, upper, remaining, false, excludesNan);
      }
    if (const auto *branch = dyn_cast<IfStmt>(parent);
        branch && (branch->getThen() == child || branch->getElse() == child) &&
        unchangedFloatingBranch(*child, *variable))
      floatingBounds(*branch->getCond(), *variable, cast.getType(), context,
                     lower, upper, remaining, branch->getThen() == child,
                     excludesNan);
    child = parent;
  }
  return lower && upper && remaining != 0;
}
} // namespace weavec::analysis
