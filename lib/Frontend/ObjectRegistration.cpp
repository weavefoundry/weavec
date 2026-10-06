//===- ObjectRegistration.cpp - Stack and global objects (RFC 0032) -------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/ObjectRegistration.h"

#include "weavec/Analysis/BypassedDeclarations.h"
#include "weavec/Analysis/KindTable.h"
#include "weavec/Analysis/SiteCollector.h"
#include "weavec/Core/CheckPlan.h"
#include "weavec/Core/LibrarySpec.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/SourceManager.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"

#include <algorithm>

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

/// RFC 0034 §4: the arguments of each call that a guard looks up;
/// `GuardedArgumentAll` when the guard's operand is not one argument.
constexpr unsigned GuardedArgumentAll = ~0U;
using GuardedArguments =
    llvm::DenseMap<const clang::Stmt *, llvm::DenseSet<unsigned>>;

namespace {

/// What one function body says about its locals.
class LocalWalker : public clang::RecursiveASTVisitor<LocalWalker> {
public:
  LocalWalker(clang::Stmt &body, const core::LibrarySpec &spec,
              const GuardedArguments &guarded)
      : parents(&body), library(spec), guardedArguments(guarded) {}

  /// In declaration order.
  llvm::SmallVector<const clang::VarDecl *, 8> declared;
  llvm::DenseMap<const clang::VarDecl *, const clang::DeclStmt *> statementOf;
  llvm::DenseSet<const clang::VarDecl *> escaping;
  llvm::SmallVector<const clang::CallExpr *, 2> returnsTwice;
  llvm::SmallVector<const clang::VarDecl *, 4> statics;
  /// A compound literal or an `alloca`: automatic storage with no name.
  bool unnamedStorage = false;
  /// The functions called directly that may be inlined here.
  llvm::SmallVector<const clang::FunctionDecl *, 8> inlinable;
  /// A label: a backward `goto` can run a declaration again.
  bool labels = false;

  // RecursiveASTVisitor's CRTP hooks are found by name.
  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
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
    if (const clang::VarDecl *variable = rootLocal(cast->getSubExpr());
        variable != nullptr && reachesGuard(*cast))
      escaping.insert(variable);
    return true;
  }

  bool VisitUnaryOperator(clang::UnaryOperator *op) {
    if (op->getOpcode() != clang::UO_AddrOf)
      return true;
    if (const clang::VarDecl *variable = rootLocal(op->getSubExpr());
        variable != nullptr && reachesGuard(*op))
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
    else if (callee != nullptr && !callee->hasAttr<clang::NoInlineAttr>())
      inlinable.push_back(callee->getCanonicalDecl());
    return true;
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)

private:
  /// The body's (the unit's parent map costs a large unit too much).
  clang::ParentMap parents;
  const core::LibrarySpec &library;
  const GuardedArguments &guardedArguments;
  llvm::DenseSet<const clang::Expr *> directBases;

  /// Whether the value of `call` is used: not a statement of its own (`memcpy`
  /// as a statement), nor cast to `void`.
  bool resultUsed(const clang::CallExpr &call) {
    const clang::Stmt *at = &call;
    for (const clang::Stmt *parent = parents.getParent(at); parent != nullptr;
         at = parent, parent = parents.getParent(at)) {
      if (llvm::isa<clang::ParenExpr>(parent))
        continue;
      if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(parent))
        return !cast->getType()->isVoidType();
      return llvm::isa<clang::Expr>(parent) ||
             llvm::isa<clang::DeclStmt>(parent) ||
             llvm::isa<clang::ReturnStmt>(parent) ||
             llvm::isa<clang::IfStmt>(parent) ||
             llvm::isa<clang::WhileStmt>(parent) ||
             llvm::isa<clang::ForStmt>(parent) ||
             llvm::isa<clang::DoStmt>(parent) ||
             llvm::isa<clang::SwitchStmt>(parent);
    }
    return false;
  }

  /// RFC 0034 §4: whether a guard can look up the local whose address
  /// `address` is. Not when the address is only an argument of a library
  /// function (no guard runs inside it) that takes no callback (which a
  /// guard could run in) and whose call guards none of its arguments.
  bool reachesGuard(const clang::Expr &address) {
    const clang::Expr *at = &address;
    for (unsigned depth = 0; depth < 16; ++depth) {
      const clang::Stmt *parent = parents.getParent(at);
      if (parent == nullptr)
        return true;
      if (const auto *paren = llvm::dyn_cast<clang::ParenExpr>(parent)) {
        at = paren;
        continue;
      }
      if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(parent);
          cast != nullptr && cast->getType()->isPointerType() &&
          (llvm::isa<clang::ImplicitCastExpr>(cast) ||
           llvm::isa<clang::CStyleCastExpr>(cast))) {
        at = cast;
        continue;
      }
      const auto *call = llvm::dyn_cast<clang::CallExpr>(parent);
      if (call == nullptr || call->getCallee() == at)
        return true;
      if (const auto guarded = guardedArguments.find(call);
          guarded != guardedArguments.end()) {
        unsigned position = 0;
        while (position < call->getNumArgs() && call->getArg(position) != at)
          ++position;
        if (guarded->second.contains(GuardedArgumentAll) ||
            guarded->second.contains(position))
          return true;
      }
      const clang::FunctionDecl *callee = call->getDirectCallee();
      if (callee == nullptr ||
          !analysis::governingLibraryEntry(*callee, library).has_value())
        return true;
      // A library function that returns a pointer may return one into its
      // argument (`strtok`, `strchr`): when the program keeps it.
      if (call->getType()->isPointerType() && resultUsed(*call))
        return true;
      // (The type as passed: a function's name decays to its pointer.)
      return std::ranges::any_of(
          call->arguments(), [](const clang::Expr *argument) {
            const clang::QualType type = argument->getType();
            return type->isFunctionPointerType() || type->isBlockPointerType();
          });
    }
    return true;
  }
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
                       const core::CheckPlan *checks,
                       const ObjectOptions &options) {
  ObjectPlan plan;
  // RFC 0034 §4: the arguments a guard looks up, per call.
  GuardedArguments guardedArguments;
  if (checks != nullptr)
    for (const core::CheckPlanEntry &entry : checks->entries)
      if (entry.kind == core::CheckPlanEntry::Template::Object ||
          entry.kind == core::CheckPlanEntry::Template::Live ||
          entry.kind == core::CheckPlanEntry::Template::Release)
        if (const analysis::SiteInfo *info = sites.info(entry.site);
            info != nullptr && info->stmt != nullptr &&
            llvm::isa<clang::CallExpr>(info->stmt))
          guardedArguments[info->stmt].insert(
              entry.placement ==
                          core::CheckPlanEntry::Placement::WrapArgument &&
                      entry.form != core::CheckPlanEntry::Form::Length
                  ? entry.argument
                  : GuardedArgumentAll);
  llvm::DenseSet<const clang::VarDecl *> seenGlobals;
  const auto addGlobal = [&](const clang::VarDecl &variable) {
    const clang::VarDecl *definition = definitionOf(variable);
    if (options.globals && definition != nullptr &&
        isRegisteredGlobal(*definition, context) &&
        seenGlobals.insert(definition->getCanonicalDecl()).second)
      plan.globals.push_back(definition);
  };

  // Functions with automatic storage the plan does not enter, and the
  // functions each one calls that may be inlined into it.
  llvm::DenseSet<const clang::FunctionDecl *> looseFunctions;
  llvm::DenseMap<const clang::FunctionDecl *,
                 llvm::SmallVector<const clang::FunctionDecl *, 8>>
      inlinableCallees;
  for (const analysis::SiteIndex::FunctionSites &function : sites.functions()) {
    const clang::FunctionDecl *definition = nullptr;
    if (function.decl == nullptr || !function.decl->hasBody(definition) ||
        definition == nullptr || definition->hasAttr<clang::NakedAttr>())
      continue;
    LocalWalker walker(*definition->getBody(), library, guardedArguments);
    walker.TraverseStmt(definition->getBody());
    for (const clang::VarDecl *variable : walker.statics)
      addGlobal(*variable);
    if (!options.stack)
      continue;
    const clang::FunctionDecl *canonical = definition->getCanonicalDecl();
    inlinableCallees[canonical] = std::move(walker.inlinable);
    for (const clang::CallExpr *call : walker.returnsTwice)
      plan.rewinds.push_back(
          ReturnsTwiceCall{.call = call, .function = definition});
    // A `longjmp` back into this function skips the cleanups of the scopes
    // it leaves, so nothing here is entered (§4.3).
    if (!walker.returnsTwice.empty() || function.callsSetjmp) {
      looseFunctions.insert(canonical);
      continue;
    }
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
    if (loose)
      looseFunctions.insert(canonical);
  }
  // Inlined, a callee's storage lies in its caller's frame: a function that
  // calls a loose one, directly or through others, enters its objects loose.
  for (bool grew = true; grew;) {
    grew = false;
    for (const auto &[caller, callees] : inlinableCallees)
      if (!looseFunctions.contains(caller) &&
          llvm::any_of(callees, [&](const clang::FunctionDecl *callee) {
            return looseFunctions.contains(callee);
          })) {
        looseFunctions.insert(caller);
        grew = true;
      }
  }
  for (StackObject &object : plan.stack)
    object.loose = object.loose ||
                   looseFunctions.contains(object.function->getCanonicalDecl());

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
