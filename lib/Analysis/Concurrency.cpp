//===- Concurrency.cpp - What entry points share (RFC 0030 §5.3) ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/Concurrency.h"

#include "weavec/Analysis/KindTable.h"
#include "weavec/Analysis/SiteCollector.h"

#include "clang/AST/RecursiveASTVisitor.h"

#include "llvm/ADT/STLExtras.h"

#include <vector>

using namespace clang;

namespace weavec::analysis {

/// Whether a value of `type` holds a pointer: itself, an element or a field.
static bool carriesPointer(QualType type, unsigned depth = 0) {
  type = type.getCanonicalType();
  if (type->isPointerType())
    return true;
  if (const auto *array = type->getAsArrayTypeUnsafe())
    return carriesPointer(array->getElementType(), depth);
  if (const RecordDecl *record = type->getAsRecordDecl();
      record != nullptr && depth < 8 && record->getDefinition() != nullptr)
    return llvm::any_of(record->getDefinition()->fields(),
                        [depth](const FieldDecl *field) {
                          return carriesPointer(field->getType(), depth + 1);
                        });
  return false;
}

/// The variable an lvalue or pointer expression is rooted at, through
/// members, subscripts, dereferences and address-of.
static const VarDecl *rootVariable(const Expr *e) {
  for (unsigned depth = 0; e != nullptr && depth < 64; ++depth) {
    e = e->IgnoreParenCasts();
    if (const auto *ref = dyn_cast<DeclRefExpr>(e))
      return dyn_cast<VarDecl>(ref->getDecl());
    if (const auto *member = dyn_cast<MemberExpr>(e))
      e = member->getBase();
    else if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(e))
      e = subscript->getBase();
    else if (const auto *unary = dyn_cast<UnaryOperator>(e);
             unary != nullptr && (unary->getOpcode() == UO_Deref ||
                                  unary->getOpcode() == UO_AddrOf))
      e = unary->getSubExpr();
    else
      return nullptr;
  }
  return nullptr;
}

namespace {
/// The entry targets of the unit and the variables passed to them.
class EntryCollector : public RecursiveASTVisitor<EntryCollector> {
public:
  EntryCollector(ASTContext &ctx, const core::LibrarySpec &spec)
      : context(ctx), library(spec) {}

  std::vector<const FunctionDecl *> targets;
  llvm::DenseSet<const VarDecl *> passed;
  llvm::DenseSet<const FunctionDecl *> addressTaken;
  bool unknownTarget = false;

  bool TraverseFunctionDecl(FunctionDecl *function) { // NOLINT
    const FunctionDecl *outer = current;
    current = function;
    const bool result = RecursiveASTVisitor::TraverseFunctionDecl(function);
    current = outer;
    return result;
  }
  bool VisitDeclRefExpr(DeclRefExpr *ref) { // NOLINT
    if (const auto *function = dyn_cast<FunctionDecl>(ref->getDecl()))
      addressTaken.insert(function->getCanonicalDecl());
    return true;
  }
  /// `&g`, `&g.f`, an array `g` decaying: external code may reach `g`.
  bool VisitUnaryOperator(UnaryOperator *unary) { // NOLINT
    if (unary->getOpcode() == UO_AddrOf)
      if (const VarDecl *var = rootVariable(unary->getSubExpr()))
        addressTakenVars.insert(var->getCanonicalDecl());
    return true;
  }
  bool VisitImplicitCastExpr(ImplicitCastExpr *cast) { // NOLINT
    if (cast->getCastKind() == CK_ArrayToPointerDecay)
      if (const VarDecl *var = rootVariable(cast->getSubExpr()))
        addressTakenVars.insert(var->getCanonicalDecl());
    return true;
  }
  llvm::DenseSet<const VarDecl *> addressTakenVars;
  bool VisitCallExpr(CallExpr *call) { // NOLINT
    const FunctionDecl *callee = call->getDirectCallee();
    const auto match = callee != nullptr
                           ? governingLibraryEntry(*callee, library)
                           : std::nullopt;
    if (!match)
      return true;
    for (unsigned i = 0; i < call->getNumArgs(); ++i) {
      const core::LibraryParam *param = match->param(i);
      if (param == nullptr || !param->callback ||
          param->callback->kind != core::LibCallback::Kind::Entry)
        continue;
      if (param->type == core::LibraryParam::Type::Function)
        addTarget(call->getArg(i));
      else
        addStoredTargets(call->getArg(i));
      for (const std::uint8_t row : param->callback->arguments)
        if (const int at = match->callArgument(row);
            at >= 0 && static_cast<unsigned>(at) < call->getNumArgs())
          if (const VarDecl *var =
                  rootVariable(call->getArg(static_cast<unsigned>(at))))
            passed.insert(var->getCanonicalDecl());
    }
    return true;
  }

private:
  ASTContext &context;
  const core::LibrarySpec &library;
  const FunctionDecl *current = nullptr;

  void addTarget(const Expr *value) {
    const Expr *e = value->IgnoreParenCasts();
    if (const auto *ref = dyn_cast<DeclRefExpr>(e))
      if (const auto *function = dyn_cast<FunctionDecl>(ref->getDecl())) {
        targets.push_back(function);
        return;
      }
    // `SIG_DFL` (null) and `SIG_IGN` (an integer) are no function.
    if (value->isNullPointerConstant(context, Expr::NPC_ValueDependentIsNull) ||
        isa<IntegerLiteral>(e))
      return;
    unknownTarget = true;
  }

  /// `sigaction(sig, &sa, old)`: the handlers stored into `sa` in this
  /// function, or in its initialiser.
  void addStoredTargets(const Expr *object) {
    if (object->isNullPointerConstant(context, Expr::NPC_ValueDependentIsNull))
      return;
    const VarDecl *var = rootVariable(object);
    if (var == nullptr || current == nullptr || !current->hasBody()) {
      unknownTarget = true;
      return;
    }
    const auto designators = [this](const Stmt *stmt,
                                    const auto &self) -> void {
      if (stmt == nullptr)
        return;
      if (const auto *ref = dyn_cast<DeclRefExpr>(stmt);
          ref != nullptr && isa<FunctionDecl>(ref->getDecl()))
        addTarget(ref);
      for (const Stmt *child : stmt->children())
        self(child, self);
    };
    designators(var->getInit(), designators);
    const auto stores = [&](const Stmt *stmt, const auto &self) -> void {
      if (stmt == nullptr)
        return;
      if (const auto *assign = dyn_cast<BinaryOperator>(stmt);
          assign != nullptr && assign->isAssignmentOp() &&
          rootVariable(assign->getLHS()) == var)
        designators(assign->getRHS(), designators);
      for (const Stmt *child : stmt->children())
        self(child, self);
    };
    stores(current->getBody(), stores);
  }
};
} // namespace

ConcurrencyShare collectConcurrencyShare(ASTContext &context,
                                         const core::LibrarySpec &library) {
  ConcurrencyShare share;
  EntryCollector entries(context, library);
  entries.TraverseDecl(context.getTranslationUnitDecl());
  if (entries.targets.empty() && !entries.unknownTarget)
    return share;
  for (const VarDecl *var : entries.passed)
    share.roots.insert(var);
  // Every function reachable from a target, and the globals they touch.
  std::vector<const FunctionDecl *> work;
  for (const FunctionDecl *target : entries.targets)
    if (const FunctionDecl *definition = target->getDefinition()) {
      work.push_back(definition);
      for (const ParmVarDecl *param : definition->parameters())
        if (carriesPointer(param->getType()))
          share.roots.insert(param);
    }
  bool external = entries.unknownTarget;
  const SourceManager &sm = context.getSourceManager();
  while (!work.empty()) {
    const FunctionDecl *function = work.back();
    work.pop_back();
    if (!share.entryReachable.insert(function->getCanonicalDecl()).second)
      continue;
    const auto visit = [&](const Stmt *stmt, const auto &self) -> void {
      if (stmt == nullptr)
        return;
      if (const auto *ref = dyn_cast<DeclRefExpr>(stmt)) {
        if (const auto *var = dyn_cast<VarDecl>(ref->getDecl());
            var != nullptr && var->hasGlobalStorage() &&
            carriesPointer(var->getType()))
          share.roots.insert(var->getCanonicalDecl());
      } else if (const auto *call = dyn_cast<CallExpr>(stmt)) {
        const FunctionDecl *callee = call->getDirectCallee();
        if (callee == nullptr) {
          // Until the slots of §9.3: any address-taken function.
          for (const FunctionDecl *taken : entries.addressTaken)
            if (const FunctionDecl *definition = taken->getDefinition())
              work.push_back(definition);
        } else if (const FunctionDecl *definition = callee->getDefinition()) {
          work.push_back(definition);
        } else if (!governingLibraryEntry(*callee, library) &&
                   !isPlatformDeclaration(*callee, library, sm)) {
          external = true;
        }
      }
      for (const Stmt *child : stmt->children())
        self(child, self);
    };
    visit(function->getBody(), visit);
  }
  // Code WeaveC cannot see may reach every global of external linkage and
  // every global whose address is taken.
  if (external) {
    const auto globals = [&](const Decl *decl, const auto &self) -> void {
      if (const auto *var = dyn_cast<VarDecl>(decl);
          var != nullptr && var->hasGlobalStorage() &&
          carriesPointer(var->getType()) &&
          (var->hasExternalFormalLinkage() ||
           entries.addressTakenVars.contains(var->getCanonicalDecl())))
        share.roots.insert(var->getCanonicalDecl());
      if (const auto *nested = dyn_cast<DeclContext>(decl))
        for (const Decl *child : nested->decls())
          self(child, self);
    };
    globals(context.getTranslationUnitDecl(), globals);
  }
  return share;
}

bool rootedInShare(const Expr &operand, const ConcurrencyShare &share) {
  const VarDecl *root = rootVariable(&operand);
  return root != nullptr && share.roots.contains(root->getCanonicalDecl());
}

} // namespace weavec::analysis
