//===- SlotCollector.cpp - Function-pointer slots of a unit (RFC 0030) ----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/SlotCollector.h"

#include "weavec/Analysis/KindTable.h"
#include "weavec/Analysis/ProgramDatabase.h"

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"

#include <functional>
#include <set>

namespace weavec::analysis {

// -- Names --------------------------------------------------------------------

std::string SlotCollector::functionName(const clang::FunctionDecl &function,
                                        llvm::StringRef unit) {
  if (function.isExternallyVisible())
    return function.getNameAsString();
  return unit.str() + ":" + function.getNameAsString();
}

std::string SlotCollector::recordSlotKey(const clang::RecordDecl &record,
                                         const clang::ASTContext &context,
                                         llvm::StringRef unit) {
  const clang::SourceManager &sm = context.getSourceManager();
  const clang::RecordDecl *definition = record.getDefinition();
  const clang::RecordDecl &named = definition != nullptr ? *definition : record;
  std::string key = recordTypeKey(context.getCanonicalTagType(&named), context);
  if (key.empty()) {
    if (const clang::TypedefNameDecl *name = named.getTypedefNameForAnonDecl())
      key = name->getNameAsString();
  }
  if (key.empty()) {
    // An anonymous record: named by where it is declared.
    const clang::PresumedLoc where =
        sm.getPresumedLoc(sm.getExpansionLoc(named.getLocation()));
    key = std::string("<anonymous>@") +
          (where.isValid() ? std::string(where.getFilename()) + ":" +
                                 std::to_string(where.getLine()) + ":" +
                                 std::to_string(where.getColumn())
                           : std::string("?"));
  }
  // §9.3: a record a main file defines is the unit's own.
  if (sm.isInMainFile(sm.getExpansionLoc(named.getLocation())))
    return unit.str() + ":" + key;
  return key;
}

std::optional<core::SlotKey>
SlotCollection::calleeSlot(const clang::CallExpr &call) const {
  const auto found = callIndex.find(&call);
  if (found == callIndex.end())
    return std::nullopt;
  return calls[found->second].second;
}

const clang::FunctionDecl *
SlotCollection::function(llvm::StringRef name) const {
  const auto found = functions.find(name);
  return found == functions.end() ? nullptr : found->second;
}

std::optional<core::SlotKey>
SlotCollection::slotOf(const clang::ValueDecl &decl) const {
  if (const auto *field = llvm::dyn_cast<clang::FieldDecl>(&decl))
    return core::SlotKey::field(
        SlotCollector::recordSlotKey(*field->getParent(), decl.getASTContext(),
                                     unitName),
        field->getNameAsString());
  const auto *variable = llvm::dyn_cast<clang::VarDecl>(&decl);
  if (variable == nullptr || llvm::isa<clang::ParmVarDecl>(variable) ||
      !variable->hasGlobalStorage())
    return std::nullopt;
  const std::string own = variable->getNameAsString();
  if (variable->isStaticLocal()) {
    const auto *function =
        llvm::dyn_cast<clang::FunctionDecl>(variable->getDeclContext());
    return core::SlotKey::staticGlobal(
        unitName,
        (function != nullptr ? function->getNameAsString() + "." : "") + own);
  }
  return variable->isExternallyVisible()
             ? core::SlotKey::global(own)
             : core::SlotKey::staticGlobal(unitName, own);
}

/// The reference that names a direct call's callee, through `*`, `&` and
/// parentheses.
static const clang::DeclRefExpr *
directCalleeReference(const clang::CallExpr &call) {
  const clang::Expr *callee = call.getCallee();
  while (callee != nullptr) {
    callee = callee->IgnoreParenImpCasts();
    const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(callee);
    if (unary == nullptr || (unary->getOpcode() != clang::UO_Deref &&
                             unary->getOpcode() != clang::UO_AddrOf))
      break;
    callee = unary->getSubExpr();
  }
  const auto *ref = llvm::dyn_cast_or_null<clang::DeclRefExpr>(callee);
  return ref != nullptr && llvm::isa<clang::FunctionDecl>(ref->getDecl())
             ? ref
             : nullptr;
}

/// A function pointer, or an array of them (whose elements share a slot).
static bool holdsFunctionPointers(clang::QualType type,
                                  const clang::ASTContext &context) {
  while (const clang::ArrayType *array = context.getAsArrayType(type))
    type = array->getElementType();
  return type->isFunctionPointerType();
}

// The walker is the collection's friend, so it has a name outside an anonymous
// namespace.

namespace {
/// Where a function-pointer value may come from.
struct SlotSources {
  std::set<std::string> functions;
  std::vector<core::SlotKey> slots;
  std::vector<std::string> opens;
};
} // namespace

/// Every function-pointer value of the unit.
class SlotWalker : public clang::RecursiveASTVisitor<SlotWalker> {
public:
  SlotWalker(clang::ASTContext &ctx, const core::LibrarySpec &spec,
             SlotCollection &collection)
      : context(ctx), sm(ctx.getSourceManager()), library(spec),
        out(collection) {}

  // RecursiveASTVisitor's CRTP hooks are found by name.
  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool TraverseFunctionDecl(clang::FunctionDecl *function) {
    // RFC 0030 §10.2: the check prelude is WeaveC's own code.
    if (sm.isWrittenInBuiltinFile(sm.getExpansionLoc(function->getLocation())))
      return true;
    const clang::FunctionDecl *saved = current;
    if (function->doesThisDeclarationHaveABody())
      current = function;
    const bool result = RecursiveASTVisitor::TraverseFunctionDecl(function);
    current = saved;
    return result;
  }
  bool VisitFunctionDecl(clang::FunctionDecl *function);
  bool VisitRecordDecl(clang::RecordDecl *record);
  bool VisitVarDecl(clang::VarDecl *variable);
  bool VisitBinaryOperator(clang::BinaryOperator *op);
  bool VisitUnaryOperator(clang::UnaryOperator *op);
  bool VisitReturnStmt(clang::ReturnStmt *ret);
  bool VisitCallExpr(clang::CallExpr *call);
  bool VisitDeclRefExpr(clang::DeclRefExpr *ref);
  bool VisitCastExpr(clang::CastExpr *cast);
  bool VisitCompoundLiteralExpr(clang::CompoundLiteralExpr *literal);
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)

  /// After the walk: stores the syntax does not name, escaping addresses,
  /// byte-wise writes, and the closedness rules.
  void finish();

private:
  clang::ASTContext &context;
  const clang::SourceManager &sm;
  const core::LibrarySpec &library;
  SlotCollection &out;
  const clang::FunctionDecl *current = nullptr;

  /// Direct-callee references, which do not take a function's address.
  llvm::DenseSet<const clang::DeclRefExpr *> callees;
  llvm::DenseSet<const clang::FunctionDecl *> addressTaken;
  /// `&slot` operands that are arguments of calls, and whether each
  /// escapes there (a callee without a body, an indirect call, or a row
  /// that writes through it).
  llvm::DenseMap<const clang::UnaryOperator *, std::optional<std::string>>
      addressArguments;
  /// Slots whose address is taken.
  std::set<core::SlotKey> taken;
  /// Values stored through a pointer the syntax does not name.
  std::vector<SlotSources> unnamed;
  /// Record types whose objects may come from, or go to, code outside the
  /// unit (§9.3 closedness).
  llvm::DenseSet<const clang::RecordDecl *> leaked;
  /// Records whose function-pointer fields receive outside bytes.
  std::vector<std::pair<const clang::RecordDecl *, std::string>> written;
  /// Union members stored, by union.
  llvm::DenseMap<const clang::RecordDecl *,
                 std::vector<const clang::FieldDecl *>>
      unionStores;
  /// Records defined in the unit, in source order.
  std::vector<const clang::RecordDecl *> records;

  [[nodiscard]] std::string name(const clang::FunctionDecl &function) const {
    return SlotCollector::functionName(function, out.unit());
  }
  [[nodiscard]] core::SlotKey fieldSlot(const clang::FieldDecl &field) const {
    return core::SlotKey::field(
        SlotCollector::recordSlotKey(*field.getParent(), context, out.unit()),
        field.getNameAsString());
  }
  [[nodiscard]] std::optional<core::SlotKey>
  variableSlot(const clang::VarDecl &variable) const;
  [[nodiscard]] std::optional<core::SlotKey>
  lvalueSlot(const clang::Expr *lvalue) const;
  [[nodiscard]] SlotSources sources(const clang::Expr *expr);
  void lvalueSources(const clang::Expr *lvalue, SlotSources &into);
  void flow(const SlotSources &from, const core::SlotKey &to);
  void escape(const SlotSources &from);
  void initialise(const clang::Expr *init, std::optional<core::SlotKey> slot,
                  clang::QualType type);
  [[nodiscard]] core::SlotKey calleeSlotOf(const clang::CallExpr &call);
  void leak(clang::QualType type);
  void byteWrite(const clang::Expr *argument, std::string why);
  [[nodiscard]] std::string where(const clang::Stmt &stmt) const;
};

std::optional<core::SlotKey>
SlotWalker::variableSlot(const clang::VarDecl &variable) const {
  if (const auto *param = llvm::dyn_cast<clang::ParmVarDecl>(&variable)) {
    const auto *function =
        llvm::dyn_cast<clang::FunctionDecl>(param->getDeclContext());
    if (function == nullptr)
      return std::nullopt;
    return core::SlotKey::param(name(*function),
                                param->getFunctionScopeIndex());
  }
  const std::string own = variable.getNameAsString();
  if (variable.isStaticLocal()) {
    const auto *function =
        llvm::dyn_cast<clang::FunctionDecl>(variable.getDeclContext());
    return core::SlotKey::staticGlobal(
        out.unit(),
        (function != nullptr ? function->getNameAsString() + "." : "") + own);
  }
  if (variable.hasGlobalStorage())
    return variable.isExternallyVisible()
               ? core::SlotKey::global(own)
               : core::SlotKey::staticGlobal(out.unit(), own);
  const auto *function =
      llvm::dyn_cast<clang::FunctionDecl>(variable.getDeclContext());
  std::string scope = out.unit();
  if (function != nullptr)
    scope = name(*function);
  else if (current != nullptr)
    scope = name(*current);
  return core::SlotKey::local(scope, own);
}

std::optional<core::SlotKey>
SlotWalker::lvalueSlot(const clang::Expr *lvalue) const {
  lvalue = lvalue->IgnoreParens();
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(lvalue)) {
    if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
      return variableSlot(*variable);
    return std::nullopt;
  }
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(lvalue)) {
    if (const auto *field =
            llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl()))
      return fieldSlot(*field);
    return std::nullopt;
  }
  if (const auto *subscript =
          llvm::dyn_cast<clang::ArraySubscriptExpr>(lvalue)) {
    // An element of an array of function pointers shares the array's slot;
    // an element through a pointer has no name.
    const clang::Expr *base = subscript->getBase()->IgnoreParenImpCasts();
    if (base->getType()->isArrayType())
      return lvalueSlot(base);
  }
  return std::nullopt;
}

void SlotWalker::lvalueSources(const clang::Expr *lvalue, SlotSources &into) {
  lvalue = lvalue->IgnoreParens();
  if (const auto slot = lvalueSlot(lvalue)) {
    into.slots.push_back(*slot);
    return;
  }
  if (llvm::isa<clang::CompoundLiteralExpr>(lvalue)) {
    into.opens.emplace_back("an unmodelled compound literal");
    return;
  }
  into.opens.emplace_back("a load through a pointer");
}

SlotSources SlotWalker::sources(const clang::Expr *expr) {
  SlotSources into;
  std::vector<const clang::Expr *> work{expr};
  unsigned steps = 0;
  while (!work.empty() && ++steps < 256) {
    const clang::Expr *each = work.back();
    work.pop_back();
    if (each == nullptr)
      continue;
    each = each->IgnoreParens();
    if (!each->isValueDependent() &&
        each->isNullPointerConstant(context,
                                    clang::Expr::NPC_ValueDependentIsNotNull) !=
            clang::Expr::NPCK_NotNull)
      continue;
    if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(each)) {
      const clang::Expr *sub = cast->getSubExpr();
      switch (cast->getCastKind()) {
      case clang::CK_LValueToRValue:
        lvalueSources(sub, into);
        continue;
      case clang::CK_FunctionToPointerDecay:
      case clang::CK_NoOp:
        work.push_back(sub);
        continue;
      case clang::CK_NullToPointer:
        continue;
      case clang::CK_BitCast:
        // Casts between function-pointer types preserve the value (§9.3).
        if (sub->getType()->isFunctionPointerType() ||
            sub->getType()->isFunctionType())
          work.push_back(sub);
        else
          into.opens.emplace_back("a conversion from a data pointer");
        continue;
      case clang::CK_IntegralToPointer:
        into.opens.emplace_back("a conversion from an integer");
        continue;
      default:
        into.opens.emplace_back("an unmodelled conversion");
        continue;
      }
    }
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(each)) {
      if (const auto *function =
              llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl()))
        into.functions.insert(name(*function));
      else
        lvalueSources(each, into);
      continue;
    }
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(each)) {
      // `&f` and `*fp` keep the value; `*pp` loads through a pointer.
      if (unary->getOpcode() == clang::UO_AddrOf ||
          (unary->getOpcode() == clang::UO_Deref &&
           unary->getType()->isFunctionType()))
        work.push_back(unary->getSubExpr());
      else if (unary->getOpcode() == clang::UO_Deref)
        into.opens.emplace_back("a load through a pointer");
      else
        into.opens.emplace_back("an unmodelled expression");
      continue;
    }
    if (llvm::isa<clang::MemberExpr>(each) ||
        llvm::isa<clang::ArraySubscriptExpr>(each)) {
      lvalueSources(each, into);
      continue;
    }
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(each)) {
      if (const clang::FunctionDecl *callee = call->getDirectCallee())
        into.slots.push_back(core::SlotKey::result(name(*callee)));
      else
        into.slots.push_back(core::SlotKey::callResult(calleeSlotOf(*call)));
      continue;
    }
    if (const auto *conditional =
            llvm::dyn_cast<clang::AbstractConditionalOperator>(each)) {
      work.push_back(conditional->getTrueExpr());
      work.push_back(conditional->getFalseExpr());
      continue;
    }
    if (const auto *opaque = llvm::dyn_cast<clang::OpaqueValueExpr>(each)) {
      if (opaque->getSourceExpr() != nullptr)
        work.push_back(opaque->getSourceExpr());
      continue;
    }
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(each);
        binary != nullptr && (binary->getOpcode() == clang::BO_Comma ||
                              binary->getOpcode() == clang::BO_Assign)) {
      work.push_back(binary->getRHS());
      continue;
    }
    if (const auto *choose = llvm::dyn_cast<clang::ChooseExpr>(each)) {
      work.push_back(choose->getChosenSubExpr());
      continue;
    }
    if (const auto *generic = llvm::dyn_cast<clang::GenericSelectionExpr>(each);
        generic != nullptr && !generic->isResultDependent()) {
      work.push_back(generic->getResultExpr());
      continue;
    }
    into.opens.emplace_back(llvm::isa<clang::VAArgExpr>(each)
                                ? "a value from va_arg"
                                : "an unmodelled expression");
  }
  if (!work.empty())
    into.opens.emplace_back("an expression too deep to follow");
  return into;
}

void SlotWalker::flow(const SlotSources &from, const core::SlotKey &to) {
  for (const std::string &function : from.functions)
    out.all.addMember(function, to);
  for (const core::SlotKey &slot : from.slots)
    if (slot != to)
      out.all.addSubset(slot, to);
  for (const std::string &why : from.opens)
    out.all.addOpen(to, why);
}

void SlotWalker::escape(const SlotSources &from) {
  // Code outside the unit may call what it receives.
  flow(from, core::SlotKey::param(std::string(core::UnknownFunction), 0));
}

void SlotWalker::initialise(const clang::Expr *init,
                            std::optional<core::SlotKey> slot,
                            clang::QualType type) {
  if (init == nullptr || llvm::isa<clang::ImplicitValueInitExpr>(init))
    return;
  const auto *list = llvm::dyn_cast<clang::InitListExpr>(init->IgnoreParens());
  if (list == nullptr) {
    if (slot && type->isFunctionPointerType())
      flow(sources(init), *slot);
    return;
  }
  if (!list->isSemanticForm() && list->getSemanticForm() != nullptr)
    list = list->getSemanticForm();
  if (const clang::ArrayType *array = context.getAsArrayType(type)) {
    // Array elements share the array's slot.
    for (const clang::Expr *element : list->inits())
      initialise(element, slot, array->getElementType());
    return;
  }
  const clang::RecordDecl *record = type->getAsRecordDecl();
  if (record == nullptr) {
    if (list->getNumInits() == 1)
      initialise(list->getInit(0), slot, type);
    return;
  }
  if (record->isUnion()) {
    if (const clang::FieldDecl *field = list->getInitializedFieldInUnion();
        field != nullptr && list->getNumInits() > 0) {
      unionStores[record].push_back(field);
      initialise(list->getInit(0),
                 holdsFunctionPointers(field->getType(), context)
                     ? std::optional(fieldSlot(*field))
                     : std::nullopt,
                 field->getType());
    }
    return;
  }
  unsigned index = 0;
  for (const clang::FieldDecl *field : record->fields()) {
    if (field->isUnnamedBitField())
      continue;
    if (index >= list->getNumInits())
      break;
    initialise(list->getInit(index++),
               holdsFunctionPointers(field->getType(), context)
                   ? std::optional(fieldSlot(*field))
                   : std::nullopt,
               field->getType());
  }
}

core::SlotKey SlotWalker::calleeSlotOf(const clang::CallExpr &call) {
  const SlotSources from = sources(call.getCallee());
  if (from.functions.empty() && from.opens.empty() && from.slots.size() == 1)
    return from.slots.front();
  // A callee expression with no single slot gets a slot of its own.
  const core::SlotKey slot =
      core::SlotKey::local(current != nullptr ? name(*current) : out.unit(),
                           "<callee " + where(call) + ">");
  flow(from, slot);
  return slot;
}

std::string SlotWalker::where(const clang::Stmt &stmt) const {
  const clang::PresumedLoc at =
      sm.getPresumedLoc(sm.getExpansionLoc(stmt.getBeginLoc()));
  if (!at.isValid())
    return "?";
  return std::to_string(at.getLine()) + ":" + std::to_string(at.getColumn());
}

void SlotWalker::leak(clang::QualType type) {
  std::vector<clang::QualType> work{type};
  llvm::DenseSet<const clang::Type *> seen;
  while (!work.empty()) {
    clang::QualType each = work.back();
    work.pop_back();
    if (each.isNull() ||
        !seen.insert(each.getCanonicalType().getTypePtr()).second)
      continue;
    each = each.getCanonicalType();
    if (const auto *pointer = each->getAs<clang::PointerType>()) {
      work.push_back(pointer->getPointeeType());
    } else if (const clang::ArrayType *array = context.getAsArrayType(each)) {
      work.push_back(array->getElementType());
    } else if (const clang::RecordDecl *record = each->getAsRecordDecl()) {
      const clang::RecordDecl *definition = record->getDefinition();
      if (definition == nullptr)
        continue;
      leaked.insert(definition);
      for (const clang::FieldDecl *field : definition->fields())
        work.push_back(field->getType());
    }
  }
}

void SlotWalker::byteWrite(const clang::Expr *argument, std::string why) {
  clang::QualType type = argument->getType();
  if (!type->isPointerType())
    return;
  type = type->getPointeeType();
  while (const clang::ArrayType *array = context.getAsArrayType(type))
    type = array->getElementType();
  if (const clang::RecordDecl *record = type->getAsRecordDecl())
    written.emplace_back(record, std::move(why));
}

/// `expr` without parentheses and conversions between pointer types.
static const clang::Expr *withoutPointerCasts(const clang::Expr *expr) {
  while (true) {
    expr = expr->IgnoreParens();
    const auto *cast = llvm::dyn_cast<clang::CastExpr>(expr);
    if (cast == nullptr ||
        (cast->getCastKind() != clang::CK_BitCast &&
         cast->getCastKind() != clang::CK_NoOp) ||
        !cast->getSubExpr()->getType()->isPointerType())
      return expr;
    expr = cast->getSubExpr();
  }
}

bool SlotWalker::VisitFunctionDecl(clang::FunctionDecl *function) {
  const std::string own = name(*function);
  const clang::FunctionDecl *known = out.function(own);
  if (known == nullptr || function->doesThisDeclarationHaveABody())
    out.functions[own] = function;
  if (function->doesThisDeclarationHaveABody()) {
    out.unitRules.defined.insert(own);
    if (function->isExternallyVisible())
      out.unitRules.exported.insert(own);
  }
  return true;
}

bool SlotWalker::VisitVarDecl(clang::VarDecl *variable) {
  if (llvm::isa<clang::ParmVarDecl>(variable))
    return true;
  const clang::QualType type = variable->getType();
  // §9.3 closedness: objects reachable from an externally visible variable
  // may be stored by other units.
  if (variable->hasGlobalStorage() && variable->isExternallyVisible())
    leak(type);
  if (const clang::Expr *init = variable->getInit())
    initialise(init,
               holdsFunctionPointers(type, context) ? variableSlot(*variable)
                                                    : std::nullopt,
               type);
  return true;
}

bool SlotWalker::VisitCompoundLiteralExpr(clang::CompoundLiteralExpr *literal) {
  initialise(literal->getInitializer(), std::nullopt, literal->getType());
  return true;
}

bool SlotWalker::VisitReturnStmt(clang::ReturnStmt *ret) {
  if (current != nullptr && ret->getRetValue() != nullptr &&
      current->getReturnType()->isFunctionPointerType())
    flow(sources(ret->getRetValue()), core::SlotKey::result(name(*current)));
  return true;
}

bool SlotWalker::VisitRecordDecl(clang::RecordDecl *record) {
  if (record->isThisDeclarationADefinition())
    records.push_back(record);
  return true;
}

bool SlotWalker::VisitBinaryOperator(clang::BinaryOperator *op) {
  if (!op->isAssignmentOp())
    return true;
  const clang::Expr *lhs = op->getLHS()->IgnoreParens();
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(lhs))
    if (const auto *field =
            llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
        field != nullptr && field->getParent()->isUnion())
      unionStores[field->getParent()].push_back(field);
  // A character-typed store into an object writes its bytes.
  if (lhs->getType()->isCharType()) {
    const clang::Expr *base = nullptr;
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(lhs);
        unary != nullptr && unary->getOpcode() == clang::UO_Deref)
      base = unary->getSubExpr();
    else if (const auto *subscript =
                 llvm::dyn_cast<clang::ArraySubscriptExpr>(lhs))
      base = subscript->getBase();
    if (base != nullptr)
      byteWrite(withoutPointerCasts(base), "a character-typed store");
  }
  if (op->getOpcode() != clang::BO_Assign ||
      !lhs->getType()->isFunctionPointerType())
    return true;
  SlotSources value = sources(op->getRHS());
  if (const auto slot = lvalueSlot(lhs))
    flow(value, *slot);
  else
    unnamed.push_back(std::move(value));
  return true;
}

bool SlotWalker::VisitUnaryOperator(clang::UnaryOperator *op) {
  if (op->getOpcode() != clang::UO_AddrOf)
    return true;
  const clang::Expr *target = op->getSubExpr()->IgnoreParens();
  if (!holdsFunctionPointers(target->getType(), context))
    return true;
  const auto slot = lvalueSlot(target);
  if (!slot)
    return true;
  taken.insert(*slot);
  // An address that is only an argument of a function the unit defines (or
  // of a row that does not write through it) is followed by the stores
  // through it; any other escapes.
  const auto argument = addressArguments.find(op);
  const std::optional<std::string> escapes =
      argument == addressArguments.end()
          ? std::optional<std::string>("its address escapes")
          : argument->second;
  if (!escapes)
    return true;
  if (slot->kind == core::SlotKind::Static)
    out.unitRules.escapedStatics.insert(slot->staticName());
  else
    out.all.addOpen(*slot, *escapes);
  return true;
}

/// A call to a `LibrarySpec` allocator.
static bool isAllocation(const clang::Expr *expr,
                         const core::LibrarySpec &library) {
  const auto *call = llvm::dyn_cast<clang::CallExpr>(
      withoutPointerCasts(expr)->IgnoreParenImpCasts());
  const clang::FunctionDecl *callee =
      call != nullptr ? call->getDirectCallee() : nullptr;
  if (callee == nullptr)
    return false;
  const auto match = governingLibraryEntry(*callee, library);
  return match && match->entry->result.kind == core::LibraryResult::Kind::Fresh;
}

bool SlotWalker::VisitCallExpr(clang::CallExpr *call) {
  if (const clang::DeclRefExpr *ref = directCalleeReference(*call))
    callees.insert(ref);
  const clang::FunctionDecl *callee = call->getDirectCallee();
  const auto match = callee != nullptr ? governingLibraryEntry(*callee, library)
                                       : std::nullopt;
  const bool body = callee != nullptr && callee->getDefinition() != nullptr;
  const std::string who = callee != nullptr
                              ? "'" + callee->getNameAsString() + "'"
                              : std::string("an indirect call");
  std::optional<core::SlotKey> through;
  if (callee == nullptr) {
    through = calleeSlotOf(*call);
    out.callIndex[call] = out.calls.size();
    out.calls.emplace_back(call, *through);
  }
  for (unsigned i = 0; i < call->getNumArgs(); ++i) {
    const clang::Expr *arg = call->getArg(i);
    const core::LibraryParam *row = match ? match->param(i) : nullptr;
    const bool rowWrites =
        row != nullptr &&
        (row->access == core::LibraryParam::Access::Write ||
         row->access == core::LibraryParam::Access::ReadWrite);
    const clang::Expr *stripped = withoutPointerCasts(arg);
    if (const auto *address = llvm::dyn_cast<clang::UnaryOperator>(stripped);
        address != nullptr && address->getOpcode() == clang::UO_AddrOf)
      addressArguments[address] =
          !body && (!match || rowWrites || (row != nullptr && row->out))
              ? std::optional<std::string>("its address is passed to " + who)
              : std::nullopt;
    if (arg->getType()->isFunctionPointerType()) {
      const SlotSources value = sources(arg);
      if (through)
        flow(value, core::SlotKey::callParam(*through, i));
      else if (i < callee->getNumParams())
        flow(value, core::SlotKey::param(name(*callee), i));
      else
        escape(value); // a variadic argument reaches no parameter slot
    }
    // §9.3 closedness: objects handed to code the unit does not define.
    if (!body && !match)
      leak(arg->getType());
    if (rowWrites) {
      // A zero fill leaves null, and a copy from the same type copies the
      // fields' own values.
      bool keeps = false;
      for (const core::LibFill &fill : match->entry->fills)
        keeps =
            keeps || (match->callArgument(fill.dst) == static_cast<int>(i) &&
                      fill.value.kind == core::LibTerm::Kind::Constant &&
                      fill.value.value == 0);
      for (const core::LibCopy &copy : match->entry->copies) {
        const int source = match->callArgument(copy.src);
        if (match->callArgument(copy.dst) != static_cast<int>(i) ||
            source < 0 || static_cast<unsigned>(source) >= call->getNumArgs())
          continue;
        const clang::QualType from =
            withoutPointerCasts(call->getArg(static_cast<unsigned>(source)))
                ->getType();
        keeps = keeps || (from->isPointerType() &&
                          stripped->getType()->isPointerType() &&
                          clang::ASTContext::hasSameUnqualifiedType(
                              from->getPointeeType(),
                              stripped->getType()->getPointeeType()));
      }
      if (!keeps)
        byteWrite(stripped,
                  "a byte-wise write by '" + match->entry->name + "'");
    }
  }
  if ((callee != nullptr && !body && !match) || callee == nullptr)
    leak(call->getType());
  return true;
}

bool SlotWalker::VisitDeclRefExpr(clang::DeclRefExpr *ref) {
  if (const auto *function =
          llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl());
      function != nullptr && !callees.contains(ref))
    addressTaken.insert(function->getCanonicalDecl());
  return true;
}

bool SlotWalker::VisitCastExpr(clang::CastExpr *cast) {
  const clang::QualType to = cast->getType();
  const clang::Expr *sub = cast->getSubExpr();
  const clang::QualType from = sub->getType();
  // A function converted to a data pointer or an integer escapes (§9.3).
  if ((from->isFunctionPointerType() || from->isFunctionType()) &&
      (cast->getCastKind() == clang::CK_PointerToIntegral ||
       (cast->getCastKind() == clang::CK_BitCast &&
        !to->isFunctionPointerType())))
    escape(sources(sub));
  // A record reached by converting another pointer may be any object, and
  // one made from a byte buffer holds bytes in its function pointers.
  if (cast->getCastKind() == clang::CK_BitCast && to->isPointerType() &&
      from->isPointerType()) {
    const clang::QualType target =
        to->getPointeeType().getCanonicalType().getUnqualifiedType();
    const clang::QualType source =
        from->getPointeeType().getCanonicalType().getUnqualifiedType();
    if (target->getAsRecordDecl() != nullptr && target != source) {
      if (!isAllocation(sub, library))
        leak(to);
      if (source->isCharType())
        byteWrite(cast, "its object is converted from a byte buffer");
    }
  }
  return true;
}

void SlotWalker::finish() {
  // Values stored through a pointer the syntax does not name reach every
  // address-taken slot, and whatever else it points to: they escape.
  for (const SlotSources &value : unnamed) {
    for (const core::SlotKey &slot : taken)
      flow(value, slot);
    escape(value);
  }

  // Outside bytes in function-pointer fields: byte-wise writes, objects
  // made from byte buffers, and stores to other members of a union.
  const std::function<void(const clang::RecordDecl *, const std::string &,
                           unsigned)>
      openFields = [&](const clang::RecordDecl *record, const std::string &why,
                       unsigned depth) {
        if (record == nullptr || depth > 16)
          return;
        if (const clang::RecordDecl *definition = record->getDefinition())
          record = definition;
        for (const clang::FieldDecl *field : record->fields()) {
          clang::QualType type = field->getType();
          while (const clang::ArrayType *array = context.getAsArrayType(type))
            type = array->getElementType();
          if (type->isFunctionPointerType())
            out.all.addOpen(fieldSlot(*field), why);
          else if (const clang::RecordDecl *nested = type->getAsRecordDecl())
            openFields(nested, why, depth + 1);
        }
      };
  for (const auto &[record, why] : written)
    openFields(record, why, 0);
  for (const auto &[record, stored] : unionStores) {
    for (const clang::FieldDecl *field : record->fields()) {
      if (!holdsFunctionPointers(field->getType(), context))
        continue;
      for (const clang::FieldDecl *other : stored) {
        if (other == field || clang::ASTContext::hasSameUnqualifiedType(
                                  other->getType(), field->getType()))
          continue;
        out.all.addOpen(fieldSlot(*field),
                        "a store to '" + other->getNameAsString() +
                            "', another member of the union");
        break;
      }
    }
  }

  // Functions code outside the unit may call: their parameters and results
  // carry outside objects.
  for (const auto &[own, function] : out.functions) {
    if (!function->doesThisDeclarationHaveABody() ||
        (!function->isExternallyVisible() &&
         !addressTaken.contains(function->getCanonicalDecl())))
      continue;
    for (const clang::ParmVarDecl *param : function->parameters())
      leak(param->getType());
    leak(function->getReturnType());
  }

  // §9.3: the fields of a record the main file defines are closed when no
  // object of it comes from or goes to code outside the unit.
  for (const clang::RecordDecl *record : records) {
    if (!sm.isInMainFile(sm.getExpansionLoc(record->getLocation())) ||
        leaked.contains(record))
      continue;
    if (llvm::any_of(record->fields(), [&](const clang::FieldDecl *field) {
          return holdsFunctionPointers(field->getType(), context);
        }))
      out.unitRules.confinedRecords.insert(
          SlotCollector::recordSlotKey(*record, context, out.unit()));
  }
}

SlotCollection SlotCollector::collect() {
  SlotCollection out;
  out.unitName = options.unit;
  if (out.unitName.empty()) {
    const clang::SourceManager &sm = context.getSourceManager();
    if (const auto entry = sm.getFileEntryRefForID(sm.getMainFileID()))
      out.unitName = entry->getName().str();
    else
      out.unitName = "<unit>";
  }
  out.unitRules.scope = core::SlotScope::Unit;
  SlotWalker walker(context, library, out);
  walker.TraverseDecl(context.getTranslationUnitDecl());
  walker.finish();
  return out;
}

// -- The dump
// -----------------------------------------------------------------------

static void dumpSlotLines(const SlotCollection &slots,
                          const core::SlotSolution &solution,
                          const clang::SourceManager &sm,
                          llvm::raw_ostream &os) {
  os << "slot constraints:\n";
  for (const core::SlotConstraint &constraint :
       slots.constraints().constraints()) {
    switch (constraint.kind) {
    case core::SlotConstraint::Kind::Member:
      os << "  member " << constraint.function << " -> "
         << constraint.slot.toString() << "\n";
      break;
    case core::SlotConstraint::Kind::Subset:
      os << "  subset " << constraint.from.toString() << " -> "
         << constraint.slot.toString() << "\n";
      break;
    case core::SlotConstraint::Kind::Open:
      os << "  open " << constraint.slot.toString() << " (" << constraint.detail
         << ")\n";
      break;
    }
  }
  os << "slot rules:";
  for (const std::string &record : slots.rules().confinedRecords)
    os << " confined(" << record << ")";
  for (const std::string &name : slots.rules().escapedStatics)
    os << " escaped(" << name << ")";
  os << "\nindirect calls:\n";
  for (const auto &[call, slot] : slots.indirectCalls()) {
    const clang::PresumedLoc at =
        sm.getPresumedLoc(sm.getExpansionLoc(call->getBeginLoc()));
    const core::CallResolution resolution = solution.resolveCall(slot);
    os << "  "
       << (at.isValid() ? std::to_string(at.getLine()) + ":" +
                              std::to_string(at.getColumn())
                        : std::string("?"))
       << ": through " << slot.toString() << ": "
       << core::toString(resolution.kind);
    if (!resolution.targets.empty()) {
      os << " {";
      llvm::interleaveComma(resolution.targets, os);
      os << "}";
    }
    if (resolution.open)
      os << " (" << resolution.open->detail << ")";
    os << "\n";
  }
}

void dumpSlots(const SlotCollection &slots, const core::SlotSolution &solution,
               clang::ASTContext &context, llvm::raw_ostream &os) {
  std::string text;
  llvm::raw_string_ostream lines(text);
  dumpSlotLines(slots, solution, context.getSourceManager(), lines);
  // The unit's own names read `<unit>:<name>` here, for brevity.
  const std::string prefix = slots.unit() + ":";
  const llvm::StringRef shown = "<unit>:";
  for (std::size_t at = text.find(prefix); at != std::string::npos;
       at = text.find(prefix, at + shown.size()))
    text.replace(at, prefix.size(), shown.str());
  os << text;
}

} // namespace weavec::analysis
