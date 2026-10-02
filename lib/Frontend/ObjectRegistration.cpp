//===- ObjectRegistration.cpp - Stack and global objects (RFC 0032) -------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/ObjectRegistration.h"

#include "weavec/Analysis/BypassedDeclarations.h"
#include "weavec/Analysis/SiteCollector.h"
#include "weavec/Core/LibrarySpec.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/SourceManager.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"

namespace weavec::frontend {

/// The local variable an lvalue is, or is part of: through parentheses, `.`
/// and subscripts of array lvalues. Null when the lvalue is reached through
/// a pointer.
static const clang::VarDecl *rootLocal(const clang::Expr *expr) {
  for (unsigned depth = 0; expr != nullptr && depth < 64; ++depth) {
    expr = expr->IgnoreParens();
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
      if (member->isArrow())
        return nullptr;
      expr = member->getBase();
      continue;
    }
    if (const auto *subscript =
            llvm::dyn_cast<clang::ArraySubscriptExpr>(expr)) {
      const auto *decay = llvm::dyn_cast<clang::ImplicitCastExpr>(
          subscript->getBase()->IgnoreParens());
      if (decay == nullptr ||
          decay->getCastKind() != clang::CK_ArrayToPointerDecay)
        return nullptr;
      expr = decay->getSubExpr();
      continue;
    }
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
      const auto *variable = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
      return variable != nullptr && variable->hasLocalStorage() ? variable
                                                                : nullptr;
    }
    return nullptr;
  }
  return nullptr;
}

/// Whether a record type has a flexible array member, at any depth.
static bool hasFlexibleArray(clang::QualType type) {
  const clang::Type *element = type->getBaseElementTypeUnsafe();
  const clang::RecordDecl *record =
      element != nullptr ? element->getAsRecordDecl() : nullptr;
  return record != nullptr && record->hasFlexibleArrayMember();
}

namespace {

/// What one function body says about its locals.
class LocalWalker : public clang::RecursiveASTVisitor<LocalWalker> {
public:
  explicit LocalWalker(const core::LibrarySpec &spec) : library(spec) {}

  /// In declaration order.
  llvm::SmallVector<const clang::VarDecl *, 8> declared;
  llvm::DenseMap<const clang::VarDecl *, const clang::DeclStmt *> statementOf;
  llvm::DenseSet<const clang::VarDecl *> escaping;
  llvm::SmallVector<const clang::CallExpr *, 2> returnsTwice;
  llvm::SmallVector<const clang::VarDecl *, 4> statics;
  /// A compound literal or an `alloca`: automatic storage with no name.
  bool unnamedStorage = false;
  /// A label: a backward `goto` can run a declaration again.
  bool labels = false;

  // RecursiveASTVisitor's CRTP hooks are found by name.
  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool TraverseBlockExpr(clang::BlockExpr * /*block*/) { return true; }

  bool VisitDeclStmt(clang::DeclStmt *statement) {
    for (const clang::Decl *decl : statement->decls()) {
      const auto *variable = llvm::dyn_cast<clang::VarDecl>(decl);
      if (variable == nullptr)
        continue;
      if (variable->isStaticLocal()) {
        statics.push_back(variable);
        continue;
      }
      // The compiler's own locals (a range cache) are not the program's.
      if (!variable->hasLocalStorage() || variable->isImplicit())
        continue;
      declared.push_back(variable);
      statementOf[variable] = statement;
      // A cleanup function is handed the variable's address.
      if (variable->hasAttr<clang::CleanupAttr>())
        escaping.insert(variable);
    }
    return true;
  }

  bool VisitLabelStmt(clang::LabelStmt * /*label*/) {
    labels = true;
    return true;
  }

  bool VisitArraySubscriptExpr(clang::ArraySubscriptExpr *subscript) {
    // The decay under a subscript does not let the address out.
    if (const auto *decay = llvm::dyn_cast<clang::ImplicitCastExpr>(
            subscript->getBase()->IgnoreParens()))
      directBases.insert(decay);
    return true;
  }

  bool VisitImplicitCastExpr(clang::ImplicitCastExpr *cast) {
    if (cast->getCastKind() != clang::CK_ArrayToPointerDecay ||
        directBases.contains(cast))
      return true;
    if (const clang::VarDecl *variable = rootLocal(cast->getSubExpr()))
      escaping.insert(variable);
    return true;
  }

  bool VisitUnaryOperator(clang::UnaryOperator *op) {
    if (op->getOpcode() != clang::UO_AddrOf)
      return true;
    if (const clang::VarDecl *variable = rootLocal(op->getSubExpr()))
      escaping.insert(variable);
    return true;
  }

  bool VisitCompoundLiteralExpr(clang::CompoundLiteralExpr * /*literal*/) {
    unnamedStorage = true;
    return true;
  }

  bool VisitCallExpr(clang::CallExpr *call) {
    switch (call->getBuiltinCallee()) {
    case clang::Builtin::BIalloca:
    case clang::Builtin::BI_alloca:
    case clang::Builtin::BI__builtin_alloca:
    case clang::Builtin::BI__builtin_alloca_with_align:
    case clang::Builtin::BI__builtin_alloca_uninitialized:
    case clang::Builtin::BI__builtin_alloca_with_align_uninitialized:
      unnamedStorage = true;
      break;
    default:
      break;
    }
    if (const clang::FunctionDecl *callee = call->getDirectCallee();
        callee != nullptr && analysis::isReturnsTwice(*callee, library))
      returnsTwice.push_back(call);
    return true;
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)

private:
  const core::LibrarySpec &library;
  llvm::DenseSet<const clang::Expr *> directBases;
};

} // namespace

/// Whether a local can be entered: an object of a complete type with a
/// size, whose address C lets the program take.
static bool isEnterable(const clang::VarDecl &variable,
                        const clang::ASTContext &context) {
  if (variable.isInvalidDecl() ||
      variable.getStorageClass() == clang::SC_Register ||
      variable.hasAttr<clang::BlocksAttr>())
    return false;
  const clang::QualType type = variable.getType();
  if (type.isNull() || type->isDependentType() || type->isIncompleteType() ||
      type->isReferenceType() || type->isFunctionType() ||
      type.getAddressSpace() != clang::LangAS::Default)
    return false;
  // A size known to be zero is no object to point into.
  return !type->isConstantSizeType() ||
         !context.getTypeSizeInChars(type).isZero();
}

/// Whether the unit's definition of a static-storage variable gets a
/// descriptor (§5).
static bool isRegisteredGlobal(const clang::VarDecl &variable,
                               const clang::ASTContext &context) {
  if (variable.isInvalidDecl() || !variable.hasGlobalStorage() ||
      variable.getTLSKind() != clang::VarDecl::TLS_None ||
      variable.hasAttr<clang::SectionAttr>() ||
      variable.hasAttr<clang::WeakAttr>() ||
      variable.hasAttr<clang::AliasAttr>() ||
      variable.hasAttr<clang::WeakRefAttr>() ||
      variable.hasAttr<clang::WeakImportAttr>())
    return false;
  const clang::IdentifierInfo *name = variable.getIdentifier();
  if (name != nullptr && name->getName().starts_with("__weavec_"))
    return false;
  const clang::QualType type = variable.getType();
  if (type.isNull() || type->isDependentType() || type->isIncompleteType() ||
      !type->isConstantSizeType() || type->isFunctionType() ||
      type.getAddressSpace() != clang::LangAS::Default ||
      hasFlexibleArray(type))
    return false;
  return !context.getTypeSizeInChars(type).isZero();
}

/// The declaration of `variable` that defines it in this unit: its
/// definition, or the tentative one that acts as it.
static const clang::VarDecl *definitionOf(const clang::VarDecl &variable) {
  if (const clang::VarDecl *definition = variable.getDefinition())
    return definition;
  return variable.getActingDefinition();
}

ObjectPlan planObjects(clang::ASTContext &context,
                       const analysis::SiteIndex &sites,
                       const core::LibrarySpec &library,
                       const ObjectOptions &options) {
  ObjectPlan plan;
  llvm::DenseSet<const clang::VarDecl *> seenGlobals;
  const auto addGlobal = [&](const clang::VarDecl &variable) {
    const clang::VarDecl *definition = definitionOf(variable);
    if (options.globals && definition != nullptr &&
        isRegisteredGlobal(*definition, context) &&
        seenGlobals.insert(definition->getCanonicalDecl()).second)
      plan.globals.push_back(definition);
  };

  for (const analysis::SiteIndex::FunctionSites &function : sites.functions()) {
    const clang::FunctionDecl *definition = nullptr;
    if (function.decl == nullptr || !function.decl->hasBody(definition) ||
        definition == nullptr || definition->hasAttr<clang::NakedAttr>())
      continue;
    LocalWalker walker(library);
    walker.TraverseStmt(definition->getBody());
    for (const clang::VarDecl *variable : walker.statics)
      addGlobal(*variable);
    if (!options.stack)
      continue;
    for (const clang::CallExpr *call : walker.returnsTwice)
      plan.rewinds.push_back(
          ReturnsTwiceCall{.call = call, .function = definition});
    // A `longjmp` back into this function skips the cleanups of the scopes
    // it leaves, so nothing here is entered (§4.3).
    if (!walker.returnsTwice.empty() || function.callsSetjmp)
      continue;
    bool loose = walker.unnamedStorage;
    // A declaration a jump bypasses does not run its registration either:
    // the local is automatic storage the list does not know.
    llvm::DenseSet<const clang::VarDecl *> bypassed;
    for (const clang::VarDecl *variable :
         analysis::bypassedDeclarationsOfAnyType(*definition->getBody()))
      bypassed.insert(variable);
    llvm::SmallVector<const clang::VarDecl *, 8> entered;
    for (const clang::ParmVarDecl *parameter : definition->parameters())
      if (walker.escaping.contains(parameter)) {
        if (isEnterable(*parameter, context))
          entered.push_back(parameter);
        else
          loose = true;
      }
    for (const clang::VarDecl *variable : walker.declared)
      if (walker.escaping.contains(variable)) {
        if (isEnterable(*variable, context) && !bypassed.contains(variable))
          entered.push_back(variable);
        else
          loose = true;
      }
    // The declarations of the body's own block live as long as the frame.
    llvm::DenseSet<const clang::DeclStmt *> outermost;
    if (const auto *body =
            llvm::dyn_cast_or_null<clang::CompoundStmt>(definition->getBody()))
      for (const clang::Stmt *statement : body->body())
        if (const auto *declaration =
                llvm::dyn_cast<clang::DeclStmt>(statement))
          outermost.insert(declaration);
    for (const clang::VarDecl *variable : entered) {
      const clang::DeclStmt *statement = walker.statementOf.lookup(variable);
      // What may leave scope, or be declared again with another size, while
      // the function runs: a nested scope's local, a variable-length array,
      // and any local of a function a `goto` can run backwards in.
      const bool scoped = statement != nullptr &&
                          (!outermost.contains(statement) || walker.labels ||
                           variable->getType()->isVariablyModifiedType());
      plan.stack.push_back(StackObject{.variable = variable,
                                       .statement = statement,
                                       .function = definition,
                                       .loose = loose,
                                       .scoped = scoped});
    }
    // A function with storage the list does not know and nothing to enter
    // still says so: once it is inlined, that storage lies in a frame whose
    // other objects would otherwise be taken to be all there is (§4.3).
    if (loose && entered.empty())
      plan.stack.push_back(StackObject{.variable = nullptr,
                                       .statement = nullptr,
                                       .function = definition,
                                       .loose = true,
                                       .scoped = false});
  }

  if (options.globals) {
    const clang::SourceManager &sm = context.getSourceManager();
    for (const clang::Decl *decl : context.getTranslationUnitDecl()->decls()) {
      const auto *variable = llvm::dyn_cast<clang::VarDecl>(decl);
      if (variable == nullptr || !variable->isFileVarDecl())
        continue;
      // The predefines hold the check prelude.
      if (sm.isWrittenInBuiltinFile(
              sm.getExpansionLoc(variable->getLocation())))
        continue;
      addGlobal(*variable);
    }
  }
  return plan;
}

} // namespace weavec::frontend
