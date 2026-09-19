//===- KindInferenceCollect.cpp - The stores of a unit (RFC 0030) ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// §7.3: one walk over the unit collects every value that reaches a slot, a
// result, a static parameter or a variable, and the stores the syntax does
// not show: stores through `*q`, slot addresses passed to callees,
// byte-wise writes, union punning and conversions from byte buffers.
// `addHiddenStores` then turns the latter into node inputs.
//
//===----------------------------------------------------------------------===//

#include "KindInferenceImpl.h"

#include "clang/AST/RecursiveASTVisitor.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <utility>

namespace weavec::analysis {

namespace {

/// Every declaration, store, call and conversion of the unit.
class Collector : public clang::RecursiveASTVisitor<Collector> {
public:
  explicit Collector(KindInferenceState &inference) : state(inference) {}

  // RecursiveASTVisitor's CRTP hooks are found by name.
  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool TraverseFunctionDecl(clang::FunctionDecl *function) {
    const clang::FunctionDecl *saved = current;
    std::vector<const clang::FieldDecl *> savedFields = std::move(mentioned);
    mentioned.clear();
    if (function->doesThisDeclarationHaveABody())
      current = function;
    const bool result = RecursiveASTVisitor::TraverseFunctionDecl(function);
    relateMentioned();
    current = saved;
    mentioned = std::move(savedFields);
    return result;
  }

  bool VisitFunctionDecl(clang::FunctionDecl *function);
  bool VisitFieldDecl(clang::FieldDecl *field);
  bool VisitRecordDecl(clang::RecordDecl *record);
  bool VisitVarDecl(clang::VarDecl *variable);
  bool VisitBinaryOperator(clang::BinaryOperator *op);
  bool VisitUnaryOperator(clang::UnaryOperator *op);
  bool VisitCallExpr(clang::CallExpr *call);
  bool VisitDeclRefExpr(clang::DeclRefExpr *ref);
  bool VisitReturnStmt(clang::ReturnStmt *ret);
  bool VisitInitListExpr(clang::InitListExpr *list);
  bool VisitCastExpr(clang::CastExpr *cast);
  bool VisitMemberExpr(clang::MemberExpr *member);
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)

private:
  KindInferenceState &state;
  const clang::FunctionDecl *current = nullptr;
  /// Direct-callee references, which do not take the function's address.
  llvm::DenseSet<const clang::DeclRefExpr *> callees;
  llvm::DenseSet<const clang::FunctionDecl *> referencedSeen;
  /// The fields the current function mentions (§7.6 pruning).
  std::vector<const clang::FieldDecl *> mentioned;

  void store(const clang::Expr *lhs, const clang::Expr *value,
             const clang::Stmt *where);
  void arithmetic(const clang::Expr *lvalue, const clang::Stmt *where);
  void characterStore(const clang::Expr *lhs, const clang::Stmt *where);
  void noteVariable(const clang::Expr *lvalue, bool assigned, bool modified);
  void libraryCall(const clang::CallExpr &call,
                   const core::LibraryMatch &match);
  void slotArguments(const clang::CallExpr &call);
  void byteWrite(const clang::Expr *argument, const clang::Stmt *where,
                 std::string what, bool keepsPointers);
  void relateMentioned();
};

} // namespace

bool Collector::VisitFunctionDecl(clang::FunctionDecl *function) {
  const clang::FunctionDecl *canonical = function->getCanonicalDecl();
  KindInferenceState::FunctionFacts &facts = state.functions[canonical];
  if (!function->doesThisDeclarationHaveABody() ||
      function->isDependentContext())
    return true;
  facts.definition = function;
  state.definitions.push_back(function);
  for (const clang::ParmVarDecl *param : function->parameters()) {
    if (isObjectPointer(param->getType()))
      state.addNode(KindNode::Kind::Value, param,
                    param->getType()->getPointeeType());
  }
  const clang::QualType result = function->getReturnType();
  const KindEntry *declared = state.kinds.result(*function);
  if (isObjectPointer(result) &&
      (declared == nullptr || !declared->hasDeclaredShape()))
    state.addNode(KindNode::Kind::Result, canonical, result->getPointeeType());
  return true;
}

bool Collector::VisitFieldDecl(clang::FieldDecl *field) {
  if (!isObjectPointer(field->getType()) ||
      state.nodeOf.contains(field->getCanonicalDecl()))
    return true;
  const KindEntry *declared = state.kinds.field(*field);
  if (declared != nullptr && declared->hasDeclaredShape())
    return true;
  state.addNode(KindNode::Kind::Field, field->getCanonicalDecl(),
                field->getType()->getPointeeType());
  return true;
}

bool Collector::VisitRecordDecl(clang::RecordDecl *record) {
  if (record->isThisDeclarationADefinition() && !record->isDependentType())
    state.records.push_back(record);
  return true;
}

bool Collector::VisitVarDecl(clang::VarDecl *variable) {
  if (llvm::isa<clang::ParmVarDecl>(variable) ||
      !isObjectPointer(variable->getType()))
    return true;
  const clang::VarDecl *canonical = variable->getCanonicalDecl();
  if (variable->hasGlobalStorage()) {
    const KindEntry *declared = state.kinds.variable(*variable);
    if (declared != nullptr && declared->hasDeclaredShape())
      return true;
    if (!state.nodeOf.contains(canonical))
      state.addNode(KindNode::Kind::Global, canonical,
                    variable->getType()->getPointeeType());
  } else if (!state.nodeOf.contains(canonical)) {
    state.addNode(KindNode::Kind::Value, canonical,
                  variable->getType()->getPointeeType());
  }
  const auto found = state.nodeOf.find(canonical);
  if (found == state.nodeOf.end())
    return true;
  KindNode &node = state.nodes[found->second];
  if (const clang::Expr *init = variable->getInit()) {
    // `T *p = {v}`: the one element is the value.
    if (const auto *list =
            llvm::dyn_cast<clang::InitListExpr>(init->IgnoreParenImpCasts());
        list != nullptr && list->getNumInits() == 1)
      init = list->getInit(0);
    node.incoming.push_back(Incoming{.value = init,
                                     .fixed = std::nullopt,
                                     .node = std::nullopt,
                                     .store = variable->getInit()});
  } else if (!variable->hasGlobalStorage() &&
             variable->isThisDeclarationADefinition() ==
                 clang::VarDecl::Definition) {
    // Uninitialised: null under zero-initialisation (§11), and read only
    // after a store otherwise.
    node.incoming.push_back(Incoming{.value = nullptr,
                                     .fixed = ValueWidth::null(),
                                     .node = std::nullopt,
                                     .store = nullptr});
  }
  return true;
}

void Collector::noteVariable(const clang::Expr *lvalue, bool assigned,
                             bool modified) {
  const auto *ref =
      llvm::dyn_cast<clang::DeclRefExpr>(lvalue->IgnoreParenImpCasts());
  const auto *variable =
      ref != nullptr ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
  if (variable == nullptr || variable->hasGlobalStorage())
    return;
  KindInferenceState::VariableUse &use =
      state.variableUses[variable->getCanonicalDecl()];
  use.assigned = use.assigned || assigned;
  use.modified = use.modified || modified;
}

/// A slot whose stores are not inferred: a declared kind is checked at each
/// store instead (§7.4 rule 5), and an element of an array of pointers is no
/// slot (a load from one is never Single-valid).
static bool untrackedLvalue(const clang::Expr *lvalue) {
  if (llvm::isa<clang::DeclRefExpr>(lvalue) ||
      llvm::isa<clang::MemberExpr>(lvalue))
    return true;
  const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(lvalue);
  if (subscript == nullptr)
    return false;
  const auto *decay = llvm::dyn_cast<clang::ImplicitCastExpr>(
      subscript->getBase()->IgnoreParens());
  return decay != nullptr &&
         decay->getCastKind() == clang::CK_ArrayToPointerDecay;
}

void Collector::store(const clang::Expr *lhs, const clang::Expr *value,
                      const clang::Stmt *where) {
  lhs = lhs->IgnoreParens();
  if (const auto node = state.slotNode(lhs)) {
    state.nodes[*node].incoming.push_back(Incoming{.value = value,
                                                   .fixed = std::nullopt,
                                                   .node = std::nullopt,
                                                   .store = where});
    return;
  }
  if (untrackedLvalue(lhs))
    return;
  // `*q = v`, `q[i] = v`: the syntax does not name the slot (§7.3).
  state.indirectStores.push_back(IndirectStore{.value = value,
                                               .type = lhs->getType(),
                                               .store = where,
                                               .arithmetic = false});
}

void Collector::arithmetic(const clang::Expr *lvalue,
                           const clang::Stmt *where) {
  lvalue = lvalue->IgnoreParens();
  if (const auto node = state.slotNode(lvalue)) {
    state.nodes[*node].forced.push_back(
        KindDemotion{.store = where, .reason = "pointer arithmetic"});
    return;
  }
  if (untrackedLvalue(lvalue))
    return;
  state.indirectStores.push_back(IndirectStore{.value = nullptr,
                                               .type = lvalue->getType(),
                                               .store = where,
                                               .arithmetic = true});
}

void Collector::byteWrite(const clang::Expr *argument, const clang::Stmt *where,
                          std::string what, bool keepsPointers) {
  const clang::QualType type = argument->getType();
  if (!type->isPointerType())
    return;
  clang::QualType pointee = type->getPointeeType();
  while (const clang::ArrayType *array = state.context.getAsArrayType(pointee))
    pointee = array->getElementType();
  if (const clang::RecordDecl *record = pointee->getAsRecordDecl())
    state.byteWrites.push_back(KindInferenceState::ByteWrite{
        .record = record->getDefinition() != nullptr ? record->getDefinition()
                                                     : record,
        .demotion = KindDemotion{.store = where, .reason = std::move(what)},
        .keepsPointers = keepsPointers});
}

void Collector::characterStore(const clang::Expr *lhs,
                               const clang::Stmt *where) {
  if (!isCharacter(lhs->getType()))
    return;
  lhs = lhs->IgnoreParens();
  const clang::Expr *base = nullptr;
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(lhs);
      unary != nullptr && unary->getOpcode() == clang::UO_Deref)
    base = unary->getSubExpr();
  else if (const auto *subscript =
               llvm::dyn_cast<clang::ArraySubscriptExpr>(lhs))
    base = subscript->getBase();
  if (base == nullptr)
    return;
  const clang::Expr *object = stripPointerCasts(base);
  if (const auto *address = llvm::dyn_cast<clang::UnaryOperator>(object);
      address != nullptr && address->getOpcode() == clang::UO_AddrOf) {
    if (const auto node = state.slotNode(address->getSubExpr()->IgnoreParens()))
      state.nodes[*node].forced.push_back(KindDemotion{
          .store = where, .reason = "a character-typed store into it"});
  }
  byteWrite(object, where, "a character-typed store", false);
}

bool Collector::VisitBinaryOperator(clang::BinaryOperator *op) {
  const clang::Expr *lhs = op->getLHS();
  const clang::QualType type = lhs->getType();
  if (!op->isAssignmentOp())
    return true;
  noteVariable(lhs, /*assigned=*/true,
               /*modified=*/op->isCompoundAssignmentOp());
  characterStore(lhs, op);
  if (const auto *member =
          llvm::dyn_cast<clang::MemberExpr>(lhs->IgnoreParens())) {
    const auto *field =
        llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (field != nullptr && field->getParent()->isUnion())
      state.unionStores[field->getParent()].emplace_back(field, op);
  }
  if (!isObjectPointer(type))
    return true;
  if (op->getOpcode() == clang::BO_Assign)
    store(lhs, op->getRHS(), op);
  else
    arithmetic(lhs, op);
  return true;
}

bool Collector::VisitUnaryOperator(clang::UnaryOperator *op) {
  const clang::Expr *sub = op->getSubExpr();
  if (op->isIncrementDecrementOp()) {
    noteVariable(sub, /*assigned=*/false, /*modified=*/true);
    characterStore(sub, op);
    if (isObjectPointer(sub->getType()))
      arithmetic(sub, op);
    return true;
  }
  if (op->getOpcode() != clang::UO_AddrOf)
    return true;
  const clang::Expr *target = sub->IgnoreParens();
  if (const clang::VarDecl *variable = variableOf(target);
      variable != nullptr && !variable->hasGlobalStorage()) {
    KindInferenceState::VariableUse &use =
        state.variableUses[variable->getCanonicalDecl()];
    use.addressTaken = true;
    use.modified = true;
  }
  if (const auto node = state.slotNode(target)) {
    KindNode &slot = state.nodes[*node];
    if (slot.kind == KindNode::Kind::Value)
      slot.forced.push_back(
          KindDemotion{.store = op, .reason = "its address is taken"});
    else
      state.addressTaken.insert(*node);
  }
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(target))
    if (const auto *field =
            llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl()))
      state.fieldAddresses.try_emplace(field->getCanonicalDecl(), op);
  return true;
}

/// The call argument a row's argument `rowArg` is, if any.
static const clang::Expr *rowArgument(const clang::CallExpr &call,
                                      const core::LibraryMatch &match,
                                      unsigned rowArg) {
  const int index = match.callArgument(rowArg);
  if (index < 0 || static_cast<unsigned>(index) >= call.getNumArgs())
    return nullptr;
  return call.getArg(static_cast<unsigned>(index));
}

void Collector::libraryCall(const clang::CallExpr &call,
                            const core::LibraryMatch &match) {
  const core::LibraryEntry &entry = *match.entry;
  for (unsigned i = 0; i < call.getNumArgs(); ++i) {
    const core::LibraryParam *param = match.param(i);
    if (param == nullptr)
      continue;
    const bool writes = param->access == core::LibraryParam::Access::Write ||
                        param->access == core::LibraryParam::Access::ReadWrite;
    if (!writes && !param->out)
      continue;
    const clang::Expr *object = stripPointerCasts(call.getArg(i));
    if (const auto *address = llvm::dyn_cast<clang::UnaryOperator>(object);
        address != nullptr && address->getOpcode() == clang::UO_AddrOf) {
      if (const auto node =
              state.slotNode(address->getSubExpr()->IgnoreParens());
          node && state.nodes[*node].kind != KindNode::Kind::Value) {
        state.slotAddressArguments.push_back(
            SlotAddressArgument{.call = &call,
                                .index = i,
                                .node = node,
                                .type = object->getType()});
        continue;
      }
    }
    if (param->out && object->getType()->isPointerType() &&
        isObjectPointer(object->getType()->getPointeeType())) {
      state.slotAddressArguments.push_back(
          SlotAddressArgument{.call = &call,
                              .index = i,
                              .node = std::nullopt,
                              .type = object->getType()});
      continue;
    }
    if (!writes)
      continue;
    // A zero fill writes null pointers, and a copy from an object of the
    // same type copies Single-valid ones (§7.3); §7.6 still counts both.
    bool keeps = false;
    for (const core::LibFill &fill : entry.fills) {
      if (!std::cmp_equal(match.callArgument(fill.dst), i))
        continue;
      const auto zero = fill.value.evaluate(core::LibTerm::Values{
          .argument = [&](unsigned rowArg) -> std::optional<std::int64_t> {
            return state.constantOf(rowArgument(call, match, rowArg));
          },
          .stringLength = nullptr,
          .formatLength = nullptr,
          .macro = nullptr});
      keeps = keeps || zero == 0;
    }
    for (const core::LibCopy &copy : entry.copies) {
      const clang::Expr *source = rowArgument(call, match, copy.src);
      if (!std::cmp_equal(match.callArgument(copy.dst), i) || source == nullptr)
        continue;
      const clang::QualType from = stripPointerCasts(source)->getType();
      const clang::QualType to = object->getType();
      keeps = keeps || (from->isPointerType() && to->isPointerType() &&
                        clang::ASTContext::hasSameUnqualifiedType(
                            from->getPointeeType(), to->getPointeeType()));
    }
    byteWrite(object, &call, "a byte-wise write by '" + entry.name + "'",
              keeps);
  }
}

void Collector::slotArguments(const clang::CallExpr &call) {
  for (unsigned i = 0; i < call.getNumArgs(); ++i) {
    const clang::Expr *object = stripPointerCasts(call.getArg(i));
    if (const auto *address = llvm::dyn_cast<clang::UnaryOperator>(object);
        address != nullptr && address->getOpcode() == clang::UO_AddrOf) {
      if (const auto node =
              state.slotNode(address->getSubExpr()->IgnoreParens());
          node && state.nodes[*node].kind != KindNode::Kind::Value)
        state.slotAddressArguments.push_back(
            SlotAddressArgument{.call = &call,
                                .index = i,
                                .node = node,
                                .type = object->getType()});
      continue;
    }
    const clang::QualType type = object->getType();
    if (type->isPointerType() && isObjectPointer(type->getPointeeType()))
      state.slotAddressArguments.push_back(SlotAddressArgument{
          .call = &call, .index = i, .node = std::nullopt, .type = type});
  }
}

bool Collector::VisitCallExpr(clang::CallExpr *call) {
  if (current != nullptr)
    state.callsIn[current].push_back(call);
  if (const clang::DeclRefExpr *ref = calleeReference(*call)) {
    callees.insert(ref);
    const auto *callee = llvm::cast<clang::FunctionDecl>(ref->getDecl());
    state.functions[callee->getCanonicalDecl()].calls.push_back(call);
  }
  if (const clang::FunctionDecl *callee = call->getDirectCallee()) {
    if (const auto match = governingLibraryEntry(*callee, state.library)) {
      libraryCall(*call, *match);
      return true;
    }
  }
  slotArguments(*call);
  return true;
}

bool Collector::VisitDeclRefExpr(clang::DeclRefExpr *ref) {
  if (const auto *function =
          llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl())) {
    const clang::FunctionDecl *canonical = function->getCanonicalDecl();
    if (referencedSeen.insert(canonical).second)
      state.referenced.push_back(canonical);
    if (!callees.contains(ref))
      state.functions[canonical].addressTaken = true;
    return true;
  }
  if (const auto *param = llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl());
      param != nullptr && current != nullptr &&
      param->getDeclContext() == current)
    state.parameterReferences[param].push_back(ref);
  return true;
}

bool Collector::VisitReturnStmt(clang::ReturnStmt *ret) {
  if (current == nullptr || ret->getRetValue() == nullptr)
    return true;
  const auto found = state.nodeOf.find(current->getCanonicalDecl());
  if (found != state.nodeOf.end() &&
      state.nodes[found->second].kind == KindNode::Kind::Result)
    state.nodes[found->second].incoming.push_back(
        Incoming{.value = ret->getRetValue(),
                 .fixed = std::nullopt,
                 .node = std::nullopt,
                 .store = ret});
  return true;
}

bool Collector::VisitInitListExpr(clang::InitListExpr *list) {
  if (!list->isSemanticForm())
    return true;
  const clang::RecordDecl *record = list->getType()->getAsRecordDecl();
  if (record == nullptr)
    return true;
  const auto initialise = [&](const clang::FieldDecl *field,
                              const clang::Expr *init) {
    if (field == nullptr || init == nullptr ||
        llvm::isa<clang::ImplicitValueInitExpr>(init) ||
        !isObjectPointer(field->getType()))
      return;
    const auto found = state.nodeOf.find(field->getCanonicalDecl());
    if (found != state.nodeOf.end())
      state.nodes[found->second].incoming.push_back(
          Incoming{.value = init,
                   .fixed = std::nullopt,
                   .node = std::nullopt,
                   .store = list});
  };
  if (record->isUnion()) {
    const clang::FieldDecl *field = list->getInitializedFieldInUnion();
    if (field != nullptr && list->getNumInits() > 0) {
      state.unionStores[record].emplace_back(field, list);
      initialise(field, list->getInit(0));
    }
    return true;
  }
  unsigned index = 0;
  for (const clang::FieldDecl *field : record->fields()) {
    // CodeGen's order: unnamed bit-fields take no initialiser.
    if (field->isUnnamedBitField())
      continue;
    if (index >= list->getNumInits())
      break;
    initialise(field, list->getInit(index++));
  }
  return true;
}

bool Collector::VisitCastExpr(clang::CastExpr *cast) {
  const clang::QualType to = cast->getType();
  const clang::QualType from = cast->getSubExpr()->getType();
  if (cast->getCastKind() != clang::CK_BitCast || !to->isPointerType() ||
      !from->isPointerType())
    return true;
  const clang::QualType toPointee =
      to->getPointeeType().getCanonicalType().getUnqualifiedType();
  const clang::QualType fromPointee =
      from->getPointeeType().getCanonicalType().getUnqualifiedType();
  // `(void **)&s`: the record's pointer fields may be stored through the new
  // type (§7.3), as if their addresses were taken.
  if (const clang::RecordDecl *record = fromPointee->getAsRecordDecl();
      record != nullptr && toPointee->isPointerType()) {
    for (const clang::FieldDecl *field : record->fields())
      if (const auto found = state.nodeOf.find(field->getCanonicalDecl());
          found != state.nodeOf.end())
        state.addressTaken.insert(found->second);
  }
  // `(T **)bytes` from anything but a fresh allocation may point into any
  // object: every slot is as good as address-taken.
  if (toPointee->isPointerType() && !fromPointee->isPointerType() &&
      fromPointee->getAsRecordDecl() == nullptr) {
    const auto *call = llvm::dyn_cast<clang::CallExpr>(
        stripPointerCasts(cast->getSubExpr())->IgnoreParenImpCasts());
    const clang::FunctionDecl *callee =
        call != nullptr ? call->getDirectCallee() : nullptr;
    const auto match = callee != nullptr
                           ? governingLibraryEntry(*callee, state.library)
                           : std::nullopt;
    if (!match || match->entry->result.kind != core::LibraryResult::Kind::Fresh)
      state.everySlotTaken = true;
  }
  // `(uintptr_t *)&s->p`: the slot may be overwritten with other bytes.
  if (const auto *address = llvm::dyn_cast<clang::UnaryOperator>(
          stripPointerCasts(cast->getSubExpr()));
      address != nullptr && address->getOpcode() == clang::UO_AddrOf &&
      !toPointee->isPointerType() && !toPointee->isVoidType() &&
      !isCharacter(toPointee))
    if (const auto node = state.slotNode(address->getSubExpr()))
      state.nodes[*node].forced.push_back(KindDemotion{
          .store = cast,
          .reason = "its address is converted to '" + to.getAsString() + "'"});
  // A record made from another object type (§7.6), or from a byte buffer
  // no store into its fields wrote (§7.3).
  const clang::RecordDecl *record = toPointee->getAsRecordDecl();
  if (record == nullptr || fromPointee->isVoidType() ||
      fromPointee == toPointee || fromPointee->isIncompleteType())
    return true;
  state.conversions.push_back(KindInferenceState::Conversion{
      .record =
          record->getDefinition() != nullptr ? record->getDefinition() : record,
      .where = cast,
      .fromBytes = isCharacter(fromPointee)});
  return true;
}

bool Collector::VisitMemberExpr(clang::MemberExpr *member) {
  if (current == nullptr)
    return true;
  if (const auto *field =
          llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl()))
    mentioned.push_back(field->getCanonicalDecl());
  return true;
}

void Collector::relateMentioned() {
  llvm::sort(mentioned);
  mentioned.erase(std::ranges::unique(mentioned).begin(), mentioned.end());
  for (const clang::FieldDecl *pointer : mentioned) {
    if (!isObjectPointer(pointer->getType()))
      continue;
    for (const clang::FieldDecl *count : mentioned)
      if (count->getParent() == pointer->getParent() &&
          count->getType()->isIntegerType())
        state.related.insert({pointer, count});
  }
}

/// §7.3: `argv` of `main(int argc, char **argv[, char **envp])`, while
/// neither `argc` nor `argv` is assigned.
bool KindInferenceState::isMainArgv(
    const clang::FunctionDecl &definition) const {
  if (!definition.isMain() || definition.getNumParams() < 2)
    return false;
  const clang::ParmVarDecl *argc = definition.getParamDecl(0);
  const clang::ParmVarDecl *argv = definition.getParamDecl(1);
  const clang::QualType type = argv->getType();
  if (!argc->getType()->isIntegerType() || !type->isPointerType() ||
      !type->getPointeeType()->isPointerType() ||
      !isCharacter(type->getPointeeType()->getPointeeType()))
    return false;
  return llvm::all_of(std::array{argc, argv},
                      [&](const clang::ParmVarDecl *param) {
                        const auto use = variableUses.find(param);
                        return use == variableUses.end() ||
                               (!use->second.assigned && !use->second.modified);
                      });
}

void KindInferenceState::collect() {
  Collector collector(*this);
  collector.TraverseDecl(context.getTranslationUnitDecl());

  // Each parameter's value starts as its entry value (§7.3): declared, the
  // join of a static function's arguments, or the A1 default.
  for (const clang::FunctionDecl *definition : definitions) {
    const bool join = joinsArguments(*definition);
    for (unsigned i = 0; i < definition->getNumParams(); ++i) {
      const clang::ParmVarDecl *param = definition->getParamDecl(i);
      const auto found = nodeOf.find(param);
      if (found == nodeOf.end())
        continue;
      const unsigned value = found->second;
      const clang::QualType pointee = nodes[value].pointee;
      const KindEntry *entry = kinds.param(*definition, i);
      Incoming start{.value = nullptr,
                     .fixed = std::nullopt,
                     .node = std::nullopt,
                     .store = nullptr};
      if (entry != nullptr && entry->hasDeclaredShape()) {
        start.fixed = declaredWidth(*entry, pointee, nullptr);
      } else if (i == 1 && isMainArgv(*definition)) {
        start.fixed = ValueWidth::of(objectWidth(pointee, context),
                                     core::Nullability::Nonnull);
      } else if (join) {
        const unsigned entryNode =
            addNode(KindNode::Kind::ParamEntry, param, pointee);
        entryOf[param] = entryNode;
        for (const clang::CallExpr *call :
             functions[definition->getCanonicalDecl()].calls)
          if (i < call->getNumArgs())
            nodes[entryNode].incoming.push_back(
                Incoming{.value = call->getArg(i),
                         .fixed = std::nullopt,
                         .node = std::nullopt,
                         .store = call});
        start.node = entryNode;
      } else {
        start.fixed = ValueWidth::of(
            nodes[value].needed, entry != nullptr && entry->declaresNonnull()
                                     ? core::Nullability::Nonnull
                                     : core::Nullability::Nullable);
      }
      nodes[value].incoming.insert(nodes[value].incoming.begin(), start);
    }
  }
}

/// Every pointer field of `record` and of the records nested in it by value.
static void
forEachPointerField(const clang::RecordDecl *record,
                    const clang::ASTContext &context,
                    const std::function<void(const clang::FieldDecl &)> &visit,
                    unsigned depth = 0) {
  if (record == nullptr || depth > 16)
    return;
  if (const clang::RecordDecl *definition = record->getDefinition())
    record = definition;
  for (const clang::FieldDecl *field : record->fields()) {
    clang::QualType type = field->getType();
    while (const clang::ArrayType *array = context.getAsArrayType(type))
      type = array->getElementType();
    if (isObjectPointer(type))
      visit(*field);
    else if (const clang::RecordDecl *nested = type->getAsRecordDecl())
      forEachPointerField(nested, context, visit, depth + 1);
  }
}

void KindInferenceState::addHiddenStores() {
  // §7.3: a store through `*q` of type `T *` reaches every address-taken
  // slot of a compatible type; through `void *` and character lvalues,
  // every address-taken slot.
  const auto compatible = [&](unsigned node, clang::QualType pointer) {
    const clang::QualType through =
        pointer->getPointeeType().getCanonicalType().getUnqualifiedType();
    const clang::QualType slot =
        nodes[node].pointee.getCanonicalType().getUnqualifiedType();
    return through->isVoidType() || isCharacter(through) ||
           slot->isVoidType() || isCharacter(slot) || through == slot;
  };
  if (everySlotTaken)
    for (unsigned node = 0; node < nodes.size(); ++node)
      if (nodes[node].kind == KindNode::Kind::Field ||
          nodes[node].kind == KindNode::Kind::Global)
        addressTaken.insert(node);
  std::vector<unsigned> taken(addressTaken.begin(), addressTaken.end());
  llvm::sort(taken);
  for (const IndirectStore &store : indirectStores) {
    for (const unsigned node : taken) {
      if (!compatible(node, store.type))
        continue;
      if (store.arithmetic)
        nodes[node].forced.push_back(KindDemotion{
            .store = store.store,
            .reason = "pointer arithmetic through a pointer to it"});
      else
        nodes[node].incoming.push_back(Incoming{.value = store.value,
                                                .fixed = std::nullopt,
                                                .node = std::nullopt,
                                                .store = store.store});
    }
  }

  // §7.3: a slot whose address is passed to a callee is unknown unless the
  // callee only stores through it what the rules above already see.
  for (const SlotAddressArgument &argument : slotAddressArguments) {
    std::vector<unsigned> targets;
    if (argument.node)
      targets.push_back(*argument.node);
    else
      for (const unsigned node : taken)
        if (compatible(node, argument.type->getPointeeType()))
          targets.push_back(node);
    if (targets.empty())
      continue;
    const clang::CallExpr &call = *argument.call;
    const clang::FunctionDecl *callee = call.getDirectCallee();
    if (callee != nullptr) {
      if (const auto match = governingLibraryEntry(*callee, library)) {
        const core::LibraryParam *param = match->param(argument.index);
        const bool writes =
            param != nullptr &&
            (param->access == core::LibraryParam::Access::Write ||
             param->access == core::LibraryParam::Access::ReadWrite);
        for (const unsigned target : targets) {
          if (param != nullptr && param->out)
            nodes[target].incoming.push_back(
                Incoming{.value = nullptr,
                         .fixed = libraryValue(*param->out, *match, call,
                                               nodes[target].pointee),
                         .node = std::nullopt,
                         .store = &call});
          else if (writes)
            nodes[target].forced.push_back(KindDemotion{
                .store = &call,
                .reason = "a byte-wise write by '" + match->entry->name + "'"});
        }
        continue;
      }
      const clang::FunctionDecl *definition = callee->getDefinition();
      if (definition != nullptr &&
          argument.index < definition->getNumParams() &&
          confinedParameter(*definition->getParamDecl(argument.index)))
        continue;
    }
    const std::string who = callee != nullptr
                                ? "'" + callee->getNameAsString() + "'"
                                : std::string("an indirect call");
    const std::string reason = argument.node
                                   ? "its address is passed to " + who
                                   : "a pointer to it may be passed to " + who;
    for (const unsigned target : targets)
      nodes[target].forced.push_back(
          KindDemotion{.store = &call, .reason = reason});
  }

  // §7.3: a byte-wise write demotes every pointer field of the object's
  // type; so does making the object from a byte buffer.
  const auto demoteFields = [&](const clang::RecordDecl *record,
                                const KindDemotion &demotion) {
    forEachPointerField(record, context, [&](const clang::FieldDecl &field) {
      if (const auto found = nodeOf.find(field.getCanonicalDecl());
          found != nodeOf.end())
        nodes[found->second].forced.push_back(demotion);
    });
  };
  for (const ByteWrite &write : byteWrites)
    if (!write.keepsPointers)
      demoteFields(write.record, write.demotion);
  for (const Conversion &conversion : conversions)
    if (conversion.fromBytes)
      demoteFields(conversion.record,
                   KindDemotion{.store = conversion.where,
                                .reason = "its object is converted from a "
                                          "byte buffer"});

  // A store to another member of a union rewrites the pointer's bytes.
  for (const auto &[record, stores] : unionStores) {
    for (const clang::FieldDecl *field : record->fields()) {
      const auto found = nodeOf.find(field->getCanonicalDecl());
      if (found == nodeOf.end())
        continue;
      for (const auto &[stored, where] : stores) {
        if (stored->getCanonicalDecl() == field->getCanonicalDecl() ||
            clang::ASTContext::hasSameUnqualifiedType(stored->getType(),
                                                      field->getType()))
          continue;
        nodes[found->second].forced.push_back(
            KindDemotion{.store = where,
                         .reason = "a store to '" + fieldName(*stored) +
                                   "', another member of the union"});
        break;
      }
    }
  }
}

const clang::ParentMap &
KindInferenceState::parentsOf(const clang::FunctionDecl &function) const {
  std::unique_ptr<clang::ParentMap> &map = parentMaps[&function];
  if (map == nullptr)
    // `ParentMap` takes a mutable root but only reads it.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    map = std::make_unique<clang::ParentMap>(
        const_cast<clang::Stmt *>(function.getBody()));
  return *map;
}

/// The expression a reference's value flows into, past parentheses and the
/// conversions that keep the pointer (`child` is set to the last one).
static const clang::Stmt *valueParent(const clang::ParentMap &parents,
                                      const clang::Stmt *&child,
                                      bool &toInteger) {
  toInteger = false;
  const clang::Stmt *parent = parents.getParent(child);
  while (parent != nullptr) {
    if (llvm::isa<clang::ParenExpr>(parent)) {
      child = parent;
      parent = parents.getParent(parent);
      continue;
    }
    const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(parent);
    if (cast == nullptr)
      break;
    const clang::CastKind kind = cast->getCastKind();
    if (kind == clang::CK_PointerToBoolean ||
        kind == clang::CK_PointerToIntegral) {
      toInteger = true;
      return parent;
    }
    if (kind != clang::CK_LValueToRValue && kind != clang::CK_NoOp &&
        kind != clang::CK_BitCast)
      break;
    child = parent;
    parent = parents.getParent(parent);
  }
  return parent;
}

/// `stmt` is inside an unevaluated operand (`sizeof`, `_Alignof`).
static bool unevaluated(const clang::Stmt *stmt,
                        const clang::ParentMap &parents) {
  for (const clang::Stmt *parent = parents.getParent(stmt); parent != nullptr;
       parent = parents.getParent(parent))
    if (const auto *trait =
            llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(parent);
        trait != nullptr && !trait->getTypeOfArgument()->isVariableArrayType())
      return true;
  return false;
}

bool KindInferenceState::confinedParameter(
    const clang::ParmVarDecl &param) const {
  if (const auto use = variableUses.find(&param);
      use != variableUses.end() &&
      (use->second.assigned || use->second.modified))
    return false;
  const auto references = parameterReferences.find(&param);
  if (references == parameterReferences.end())
    return true;
  const auto *function =
      llvm::dyn_cast<clang::FunctionDecl>(param.getDeclContext());
  if (function == nullptr || function->getBody() == nullptr)
    return false;
  const clang::ParentMap &parents = parentsOf(*function);
  for (const clang::DeclRefExpr *ref : references->second) {
    if (unevaluated(ref, parents))
      continue;
    const clang::Stmt *child = ref;
    bool toInteger = false;
    const clang::Stmt *parent = valueParent(parents, child, toInteger);
    if (toInteger)
      continue;
    if (const auto *unary =
            llvm::dyn_cast_or_null<clang::UnaryOperator>(parent);
        unary != nullptr && (unary->getOpcode() == clang::UO_Deref ||
                             unary->getOpcode() == clang::UO_LNot))
      continue;
    if (const auto *subscript =
            llvm::dyn_cast_or_null<clang::ArraySubscriptExpr>(parent);
        subscript != nullptr && subscript->getBase() == child)
      continue;
    if (const auto *binary =
            llvm::dyn_cast_or_null<clang::BinaryOperator>(parent);
        binary != nullptr &&
        (binary->isComparisonOp() || binary->isLogicalOp()))
      continue;
    return false;
  }
  return true;
}

bool KindInferenceState::reliesOnDefault(
    const clang::ParmVarDecl &param) const {
  const auto references = parameterReferences.find(&param);
  if (references == parameterReferences.end())
    return false;
  const auto *function =
      llvm::dyn_cast<clang::FunctionDecl>(param.getDeclContext());
  if (function == nullptr || function->getBody() == nullptr)
    return true;
  const clang::ParentMap &parents = parentsOf(*function);
  for (const clang::DeclRefExpr *ref : references->second) {
    if (unevaluated(ref, parents))
      continue;
    const clang::Stmt *child = ref;
    bool toInteger = false;
    const clang::Stmt *parent = valueParent(parents, child, toInteger);
    if (toInteger || parent == nullptr)
      continue;
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(parent)) {
      if (unary->getOpcode() == clang::UO_Deref ||
          unary->getOpcode() == clang::UO_Plus ||
          unary->getOpcode() == clang::UO_Extension)
        return true;
      continue; // `&p`, `!p`, `++p`: not a use of the value's extent.
    }
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(parent)) {
      if (member->isArrow() && member->getBase() == child)
        return true;
      continue;
    }
    if (const auto *subscript =
            llvm::dyn_cast<clang::ArraySubscriptExpr>(parent)) {
      // `p[0]` rests on the default; `p[i]` never does (§7.3).
      const auto index = constantOf(subscript->getIdx());
      if (subscript->getBase() == child && index == 0)
        return true;
      continue;
    }
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(parent)) {
      if (binary->isComparisonOp() || binary->isLogicalOp() ||
          binary->isAdditiveOp() || binary->isCompoundAssignmentOp() ||
          (binary->isAssignmentOp() && binary->getLHS() == child) ||
          (binary->getOpcode() == clang::BO_Comma && binary->getLHS() == child))
        continue;
      return true;
    }
    if (const auto *cast = llvm::dyn_cast<clang::ExplicitCastExpr>(parent)) {
      if (cast->getType()->isIntegerType() || cast->getType()->isVoidType())
        continue;
      return true;
    }
    if (llvm::isa<clang::UnaryExprOrTypeTraitExpr>(parent))
      continue;
    if (const auto *conditional =
            llvm::dyn_cast<clang::ConditionalOperator>(parent);
        conditional != nullptr && conditional->getCond() == child)
      continue;
    // Stored, returned, passed, or copied: the value flows on.
    return true;
  }
  return false;
}

} // namespace weavec::analysis
