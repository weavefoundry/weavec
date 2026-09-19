//===- KindInference.cpp - Inferred pointer kinds (RFC 0030) --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// §7.3: the Single-valid judgement, the greatest fixpoint over slots,
// results, static parameters and variables, and the table it fills. The
// walk that collects the stores is in KindInferenceCollect.cpp, §7.5 in
// KindInferenceMustAccess.cpp, §7.4 rule 7 and §7.6 in
// KindInferenceFields.cpp.
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/KindInference.h"

#include "KindInferenceImpl.h"
#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Analysis/SlotCollector.h"

#include "clang/AST/Attr.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/Type.h"
#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <utility>

namespace weavec::analysis {

// -- Helpers ------------------------------------------------------------------

ValueWidth meet(const ValueWidth &a, const ValueWidth &b) {
  if (a.nullOnly)
    return b.nullOnly ? a
                      : ValueWidth{.bytes = b.bytes,
                                   .nullOnly = false,
                                   .nullability = core::Nullability::Nullable,
                                   .why = b.why};
  if (b.nullOnly)
    return meet(b, a);
  ValueWidth out;
  out.nullability = core::join(a.nullability, b.nullability);
  if (!a.bytes || !b.bytes) {
    out.why = !a.bytes ? a.why : b.why;
    return out;
  }
  out.bytes = std::min(*a.bytes, *b.bytes);
  return out;
}

bool isFlexibleArrayMember(const clang::FieldDecl &field,
                           const clang::ASTContext &context) {
  const clang::RecordDecl *record = field.getParent();
  if (record == nullptr || record->isUnion())
    return false;
  const clang::FieldDecl *last = nullptr;
  for (const clang::FieldDecl *each : record->fields())
    last = each;
  if (last != &field && last != field.getCanonicalDecl())
    return false;
  const clang::ArrayType *array = context.getAsArrayType(field.getType());
  if (array == nullptr)
    return false;
  const auto *constant = llvm::dyn_cast<clang::ConstantArrayType>(array);
  if (constant == nullptr)
    return true; // `T f[]`
  const std::uint64_t size = constant->getSize().getZExtValue();
  using Level = clang::LangOptions::StrictFlexArraysLevelKind;
  switch (context.getLangOpts().getStrictFlexArraysLevel()) {
  case Level::Default:
    return true; // §7.4: every trailing array at level 0.
  case Level::OneZeroOrIncomplete:
    return size <= 1;
  case Level::ZeroOrIncomplete:
    return size == 0;
  case Level::IncompleteOnly:
    return false;
  }
  return true;
}

std::uint64_t objectWidth(clang::QualType pointee,
                          const clang::ASTContext &context) {
  if (pointee.isNull())
    return 0;
  pointee = pointee.getCanonicalType();
  if (pointee->isVoidType())
    return 1;
  if (pointee->isIncompleteType() || pointee->isFunctionType() ||
      pointee->isDependentType() || pointee->isSizelessType())
    return 0;
  if (pointee->isVariableArrayType()) {
    const clang::ArrayType *array = context.getAsArrayType(pointee);
    return array != nullptr ? objectWidth(array->getElementType(), context) : 0;
  }
  if (const auto *record = pointee->getAsRecordDecl()) {
    const clang::FieldDecl *last = nullptr;
    for (const clang::FieldDecl *field : record->fields())
      last = field;
    if (last != nullptr && isFlexibleArrayMember(*last, context)) {
      const clang::ASTRecordLayout &layout = context.getASTRecordLayout(record);
      return layout.getFieldOffset(last->getFieldIndex()) /
             context.getCharWidth();
    }
  }
  return static_cast<std::uint64_t>(
      context.getTypeSizeInChars(pointee).getQuantity());
}

bool isObjectPointer(clang::QualType type) {
  return !type.isNull() && type->isPointerType() &&
         !type->isFunctionPointerType();
}

bool isCharacter(clang::QualType type) {
  return !type.isNull() && type->isCharType();
}

const clang::Expr *stripPointerCasts(const clang::Expr *expr) {
  while (expr != nullptr) {
    expr = expr->IgnoreParens();
    const auto *cast = llvm::dyn_cast<clang::CastExpr>(expr);
    if (cast == nullptr)
      break;
    // Only conversions from one pointer type to another.
    const clang::CastKind kind = cast->getCastKind();
    if ((kind != clang::CK_BitCast && kind != clang::CK_NoOp &&
         kind != clang::CK_AddressSpaceConversion) ||
        !cast->getSubExpr()->getType()->isPointerType())
      break;
    expr = cast->getSubExpr();
  }
  return expr;
}

const clang::DeclRefExpr *calleeReference(const clang::CallExpr &call) {
  const clang::Expr *callee = call.getCallee();
  while (callee != nullptr) {
    callee = callee->IgnoreParenImpCasts();
    const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(callee);
    if (unary == nullptr || (unary->getOpcode() != clang::UO_Deref &&
                             unary->getOpcode() != clang::UO_AddrOf &&
                             unary->getOpcode() != clang::UO_Plus))
      break;
    callee = unary->getSubExpr();
  }
  const auto *ref = llvm::dyn_cast_or_null<clang::DeclRefExpr>(callee);
  return ref != nullptr && llvm::isa<clang::FunctionDecl>(ref->getDecl())
             ? ref
             : nullptr;
}

const clang::ParmVarDecl *parameterOf(const clang::Expr *expr) {
  if (expr == nullptr)
    return nullptr;
  const auto *ref =
      llvm::dyn_cast<clang::DeclRefExpr>(expr->IgnoreParenImpCasts());
  return ref != nullptr ? llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl())
                        : nullptr;
}

const clang::VarDecl *variableOf(const clang::Expr *expr) {
  if (expr == nullptr)
    return nullptr;
  const auto *ref =
      llvm::dyn_cast<clang::DeclRefExpr>(expr->IgnoreParenImpCasts());
  return ref != nullptr ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl())
                        : nullptr;
}

std::string recordName(const clang::RecordDecl &record) {
  if (!record.getName().empty())
    return record.getNameAsString();
  if (const clang::TypedefNameDecl *name = record.getTypedefNameForAnonDecl())
    return name->getNameAsString();
  return "<anonymous>";
}

std::string fieldName(const clang::FieldDecl &field) {
  return recordName(*field.getParent()) + "." + field.getNameAsString();
}

std::string recordKey(const clang::RecordDecl &record,
                      const clang::ASTContext &context) {
  std::string key =
      recordTypeKey(context.getCanonicalTagType(&record), context);
  if (!key.empty())
    return key;
  // An anonymous struct is named by its typedef, which no tag can spell.
  if (const clang::TypedefNameDecl *name = record.getTypedefNameForAnonDecl())
    return name->getNameAsString();
  return {};
}

// -- Nodes --------------------------------------------------------------------

unsigned KindInferenceState::addNode(KindNode::Kind kind,
                                     const clang::Decl *decl,
                                     clang::QualType pointee) {
  const auto index = static_cast<unsigned>(nodes.size());
  KindNode node;
  node.kind = kind;
  node.decl = decl;
  node.pointee = pointee;
  node.needed = objectWidth(pointee, context);
  nodes.push_back(std::move(node));
  if (kind != KindNode::Kind::ParamEntry)
    nodeOf[decl] = index;
  return index;
}

std::string KindInferenceState::nodeName(unsigned index) const {
  const KindNode &node = nodes[index];
  if (const auto *field = llvm::dyn_cast<clang::FieldDecl>(node.decl))
    return "'" + fieldName(*field) + "'";
  if (const auto *function = llvm::dyn_cast<clang::FunctionDecl>(node.decl))
    return "the result of '" + function->getNameAsString() + "'";
  if (const auto *named = llvm::dyn_cast<clang::NamedDecl>(node.decl))
    return "'" + named->getNameAsString() + "'";
  return "a slot";
}

std::optional<unsigned>
KindInferenceState::slotNode(const clang::Expr *lvalue) const {
  if (lvalue == nullptr)
    return std::nullopt;
  lvalue = lvalue->IgnoreParens();
  const clang::Decl *decl = nullptr;
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(lvalue)) {
    if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
      decl = variable->getCanonicalDecl();
  } else if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(lvalue)) {
    if (const auto *field =
            llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl()))
      decl = field->getCanonicalDecl();
  }
  if (decl == nullptr)
    return std::nullopt;
  const auto found = nodeOf.find(decl);
  if (found == nodeOf.end())
    return std::nullopt;
  return found->second;
}

ValueWidth KindInferenceState::loadOf(unsigned index) const {
  const KindNode &node = nodes[index];
  if (node.demoted)
    return ValueWidth::unknown("a load from " + nodeName(index) +
                               ", which is unknown");
  switch (node.kind) {
  case KindNode::Kind::Field:
  case KindNode::Kind::Global: {
    // A slot is nullable unless declared otherwise: zero-initialised
    // objects hold null in every pointer field.
    const KindEntry *entry = nullptr;
    if (const auto *field = llvm::dyn_cast<clang::FieldDecl>(node.decl))
      entry = kinds.field(*field);
    else if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(node.decl))
      entry = kinds.variable(*variable);
    return ValueWidth::of(node.needed,
                          entry != nullptr && entry->declaresNonnull()
                              ? core::Nullability::Nonnull
                              : core::Nullability::Nullable);
  }
  case KindNode::Kind::Result: {
    const auto *function = llvm::cast<clang::FunctionDecl>(node.decl);
    const KindEntry *entry = kinds.result(*function);
    core::Nullability nullability =
        node.nonnull ? core::Nullability::Nonnull : core::Nullability::Nullable;
    if (entry != nullptr && entry->nullabilityLevel)
      nullability = entry->kind.nullability;
    return ValueWidth::of(node.needed, nullability);
  }
  case KindNode::Kind::ParamEntry:
    return ValueWidth::of(node.needed, node.nonnull
                                           ? core::Nullability::Nonnull
                                           : core::Nullability::Nullable);
  case KindNode::Kind::Value:
    return ValueWidth::of(node.least, node.nonnull
                                          ? core::Nullability::Nonnull
                                          : core::Nullability::Nullable);
  }
  return ValueWidth::unknown("an unmodelled load");
}

std::optional<std::int64_t>
KindInferenceState::constantOf(const clang::Expr *expr) const {
  if (expr == nullptr || expr->isValueDependent() ||
      !expr->getType()->isIntegralOrEnumerationType())
    return std::nullopt;
  if (const auto value = expr->getIntegerConstantExpr(context))
    return value->tryExtValue();
  return std::nullopt;
}

ValueWidth
KindInferenceState::declaredWidth(const KindEntry &entry,
                                  clang::QualType pointee,
                                  const clang::CallExpr *call) const {
  const core::Nullability nullability = entry.kind.nullability;
  const std::uint64_t element = objectWidth(pointee, context);
  const core::ExtentTerm &term = entry.kind.extent;
  const auto evaluate =
      [&](const core::ExtentTerm &extent) -> std::optional<std::int64_t> {
    if (extent.isConstant())
      return extent.offset;
    if (call == nullptr || extent.path->root != core::ExtentPath::Root::Param ||
        extent.path->param >= call->getNumArgs())
      return std::nullopt;
    const auto value = constantOf(call->getArg(extent.path->param));
    if (!value)
      return std::nullopt;
    return (*value * extent.scale) + extent.offset;
  };
  switch (entry.kind.shape) {
  case core::PointerShape::Single:
  case core::PointerShape::NulTerminated:
    return ValueWidth::of(element, nullability);
  case core::PointerShape::Counted:
  case core::PointerShape::Sized: {
    auto value = evaluate(term);
    if (value && entry.extentFactor && call != nullptr &&
        *entry.extentFactor < call->getNumArgs()) {
      const auto factor = constantOf(call->getArg(*entry.extentFactor));
      value = factor ? std::optional(*value * *factor) : std::nullopt;
    }
    if (!value || *value <= 0)
      return ValueWidth::unknown("a declared extent that may be empty");
    const auto count = static_cast<std::uint64_t>(*value);
    return ValueWidth::of(entry.kind.shape == core::PointerShape::Counted
                              ? count * element
                              : count,
                          nullability);
  }
  case core::PointerShape::EndedBy:
    return ValueWidth::unknown("an ended-by pointer, which may be at its end");
  case core::PointerShape::Unknown:
    break;
  }
  return ValueWidth::unknown("a pointer of unknown kind");
}

ValueWidth KindInferenceState::libraryValue(const core::LibraryResult &result,
                                            const core::LibraryMatch &match,
                                            const clang::CallExpr &call,
                                            clang::QualType pointee) const {
  const core::Nullability nullability =
      result.null == core::LibraryResult::Null::Never
          ? core::Nullability::Nonnull
          : core::Nullability::Nullable;
  const std::uint64_t element = objectWidth(pointee, context);
  const std::string name = "'" + match.entry->name + "'";
  const auto argument = [&](unsigned rowArg) -> const clang::Expr * {
    const int index = match.callArgument(rowArg);
    if (index < 0 || static_cast<unsigned>(index) >= call.getNumArgs())
      return nullptr;
    return call.getArg(static_cast<unsigned>(index));
  };
  switch (result.kind) {
  case core::LibraryResult::Kind::Fresh:
  case core::LibraryResult::Kind::Static: {
    if (!result.extent)
      return ValueWidth::of(result.string ? element : 0, nullability);
    const auto bytes = result.extent->evaluate(core::LibTerm::Values{
        .argument =
            [&](unsigned rowArg) { return constantOf(argument(rowArg)); },
        .stringLength = nullptr,
        .formatLength = nullptr,
        .macro = nullptr});
    if (bytes && *bytes >= 0)
      return ValueWidth::of(static_cast<std::uint64_t>(*bytes), nullability);
    if (result.string)
      return ValueWidth::of(element, nullability);
    return ValueWidth::unknown("an allocation by " + name +
                               " of a size that is not constant");
  }
  case core::LibraryResult::Kind::Arg: {
    const clang::Expr *arg = argument(result.arg);
    if (arg == nullptr)
      return ValueWidth::unknown("the result of " + name);
    ValueWidth value = judge(arg);
    if (value.nullOnly)
      return ValueWidth::unknown("the result of " + name +
                                 " for a null argument");
    return value;
  }
  case core::LibraryResult::Kind::Interior:
  case core::LibraryResult::Kind::InteriorState:
    if (result.string)
      return ValueWidth::of(element, nullability);
    return ValueWidth::unknown("a pointer into another object, from " + name);
  case core::LibraryResult::Kind::Unknown:
  case core::LibraryResult::Kind::Void:
  case core::LibraryResult::Kind::Int:
    break;
  }
  return ValueWidth::unknown("a pointer of unknown provenance, from " + name);
}

// -- The Single-valid judgement (§7.3)
// -----------------------------------------

/// The bytes of a declared object of `type`: `sizeof`, one element of a VLA,
/// zero for an incomplete type.
static std::uint64_t objectSize(clang::QualType type,
                                const clang::ASTContext &context) {
  if (type.isNull() || type->isIncompleteType() || type->isFunctionType())
    return 0;
  if (type->isVariableArrayType()) {
    const clang::ArrayType *array = context.getAsArrayType(type);
    return array != nullptr ? objectSize(array->getElementType(), context) : 0;
  }
  return static_cast<std::uint64_t>(
      context.getTypeSizeInChars(type).getQuantity());
}

ValueWidth KindInferenceState::judge(const clang::Expr *expr) const {
  if (expr == nullptr)
    return ValueWidth::unknown("an unmodelled expression");
  expr = expr->IgnoreParens();
  if (!expr->isValueDependent() &&
      expr->isNullPointerConstant(context,
                                  clang::Expr::NPC_ValueDependentIsNotNull) !=
          clang::Expr::NPCK_NotNull)
    return ValueWidth::null();
  if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(expr)) {
    switch (cast->getCastKind()) {
    case clang::CK_NullToPointer:
      return ValueWidth::null();
    case clang::CK_ArrayToPointerDecay:
      return judgeArray(cast->getSubExpr());
    case clang::CK_LValueToRValue:
      return judgeLoad(cast->getSubExpr());
    case clang::CK_IntegralToPointer:
      return ValueWidth::unknown("a conversion from an integer");
    case clang::CK_BitCast:
    case clang::CK_NoOp:
    case clang::CK_AddressSpaceConversion:
      // §7.4 rule 4: a conversion keeps the width it had; whether that
      // covers the new pointee is the caller's comparison.
      return judge(cast->getSubExpr());
    default:
      return ValueWidth::unknown("an unmodelled conversion");
    }
  }
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expr)) {
    switch (unary->getOpcode()) {
    case clang::UO_AddrOf: {
      ValueWidth address = judgeAddress(unary->getSubExpr());
      if (address.bytes)
        address.nullability = core::Nullability::Nonnull;
      return address;
    }
    case clang::UO_Extension:
      return judge(unary->getSubExpr());
    default:
      return ValueWidth::unknown("pointer arithmetic");
    }
  }
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(expr)) {
    if (binary->getOpcode() == clang::BO_Assign ||
        binary->getOpcode() == clang::BO_Comma)
      return judge(binary->getRHS());
    return ValueWidth::unknown("pointer arithmetic");
  }
  if (const auto *conditional =
          llvm::dyn_cast<clang::AbstractConditionalOperator>(expr))
    return meet(judge(conditional->getTrueExpr()),
                judge(conditional->getFalseExpr()));
  if (const auto *opaque = llvm::dyn_cast<clang::OpaqueValueExpr>(expr))
    return opaque->getSourceExpr() != nullptr
               ? judge(opaque->getSourceExpr())
               : ValueWidth::unknown("an unmodelled expression");
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(expr))
    return judgeCall(*call);
  if (const auto *generic = llvm::dyn_cast<clang::GenericSelectionExpr>(expr);
      generic != nullptr && !generic->isResultDependent())
    return judge(generic->getResultExpr());
  if (const auto *choose = llvm::dyn_cast<clang::ChooseExpr>(expr))
    return judge(choose->getChosenSubExpr());
  if (llvm::isa<clang::VAArgExpr>(expr))
    return ValueWidth::unknown("a value from va_arg");
  if (expr->isGLValue())
    return judgeLoad(expr);
  return ValueWidth::unknown("an unmodelled expression");
}

ValueWidth KindInferenceState::judgeLoad(const clang::Expr *lvalue) const {
  lvalue = lvalue->IgnoreParens();
  if (const auto node = slotNode(lvalue))
    return loadOf(*node);
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(lvalue)) {
    if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
      const KindEntry *entry =
          variable->hasGlobalStorage() ? kinds.variable(*variable) : nullptr;
      if (entry != nullptr && entry->hasDeclaredShape())
        return declaredWidth(*entry, variable->getType()->getPointeeType(),
                             nullptr);
      return ValueWidth::unknown("a load from '" + variable->getNameAsString() +
                                 "'");
    }
    return ValueWidth::unknown("an unmodelled load");
  }
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(lvalue)) {
    if (const auto *field =
            llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl())) {
      if (const KindEntry *entry = kinds.field(*field);
          entry != nullptr && entry->hasDeclaredShape())
        return declaredWidth(*entry, field->getType()->getPointeeType(),
                             nullptr);
      return ValueWidth::unknown("a load from '" + fieldName(*field) + "'");
    }
    return ValueWidth::unknown("an unmodelled load");
  }
  if (const auto *subscript =
          llvm::dyn_cast<clang::ArraySubscriptExpr>(lvalue)) {
    const auto *decay = llvm::dyn_cast<clang::ImplicitCastExpr>(
        subscript->getBase()->IgnoreParens());
    if (decay != nullptr &&
        decay->getCastKind() == clang::CK_ArrayToPointerDecay)
      return ValueWidth::unknown("a load from an array of pointers");
  }
  return ValueWidth::unknown("a load through a pointer");
}

/// Whether the object a member access reads is there: `x.f` on a named
/// object, or `p->f` with `p` Single-valid for the record.
static ValueWidth memberBase(const KindInferenceState &state,
                             const clang::MemberExpr &member) {
  if (!member.isArrow())
    return state.judgeAddress(member.getBase());
  ValueWidth base = state.judge(member.getBase());
  const clang::QualType record = member.getBase()->getType()->getPointeeType();
  if (!base.covers(objectWidth(record, state.context)) || base.nullOnly)
    return ValueWidth::unknown("a member of a pointer that may not reach it");
  return base;
}

ValueWidth KindInferenceState::judgeAddress(const clang::Expr *lvalue) const {
  lvalue = lvalue->IgnoreParens();
  const auto nonnull = [](std::uint64_t bytes) {
    return ValueWidth::of(bytes, core::Nullability::Nonnull);
  };
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(lvalue)) {
    if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
      return nonnull(objectSize(variable->getType(), context));
    return ValueWidth::unknown("the address of a function");
  }
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(lvalue)) {
    const auto *field =
        llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (field == nullptr)
      return ValueWidth::unknown("an unmodelled member");
    ValueWidth base = memberBase(*this, *member);
    if (!base.bytes)
      return base;
    if (isFlexibleArrayMember(*field, context))
      return nonnull(0);
    return nonnull(objectSize(field->getType(), context));
  }
  if (const auto *subscript =
          llvm::dyn_cast<clang::ArraySubscriptExpr>(lvalue)) {
    // `&a[c]` inside an array object: what is left of it.
    const auto *decay = llvm::dyn_cast<clang::ImplicitCastExpr>(
        subscript->getBase()->IgnoreParens());
    const auto index = constantOf(subscript->getIdx());
    if (decay == nullptr ||
        decay->getCastKind() != clang::CK_ArrayToPointerDecay || !index ||
        *index < 0)
      return ValueWidth::unknown("pointer arithmetic");
    const ValueWidth array = judgeArray(decay->getSubExpr());
    const std::uint64_t element = objectSize(subscript->getType(), context);
    const auto offset = static_cast<std::uint64_t>(*index) * element;
    if (!array.bytes || *array.bytes == 0 || offset >= *array.bytes)
      return ValueWidth::unknown("pointer arithmetic");
    return nonnull(*array.bytes - offset);
  }
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(lvalue);
      unary != nullptr && unary->getOpcode() == clang::UO_Deref)
    return judge(unary->getSubExpr());
  if (llvm::isa<clang::CompoundLiteralExpr>(lvalue))
    return nonnull(objectSize(lvalue->getType(), context));
  if (llvm::isa<clang::StringLiteral>(lvalue) ||
      llvm::isa<clang::PredefinedExpr>(lvalue))
    return judgeArray(lvalue);
  return ValueWidth::unknown("the address of an unmodelled object");
}

ValueWidth KindInferenceState::judgeArray(const clang::Expr *array) const {
  array = array->IgnoreParens();
  const auto nonnull = [](std::uint64_t bytes) {
    return ValueWidth::of(bytes, core::Nullability::Nonnull);
  };
  if (const auto *literal = llvm::dyn_cast<clang::StringLiteral>(array))
    return nonnull(literal->getByteLength() + literal->getCharByteWidth());
  if (const auto *predefined = llvm::dyn_cast<clang::PredefinedExpr>(array)) {
    if (const clang::StringLiteral *name = predefined->getFunctionName())
      return nonnull(name->getByteLength() + name->getCharByteWidth());
    return nonnull(1);
  }
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(array)) {
    if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
      return nonnull(objectSize(variable->getType(), context));
    return ValueWidth::unknown("an unmodelled array");
  }
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(array)) {
    const auto *field =
        llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (field == nullptr)
      return ValueWidth::unknown("an unmodelled member");
    ValueWidth base = memberBase(*this, *member);
    if (!base.bytes)
      return base;
    // §7.4: a flexible array's extent never comes from its declared bound.
    if (isFlexibleArrayMember(*field, context))
      return nonnull(0);
    return nonnull(objectSize(field->getType(), context));
  }
  if (llvm::isa<clang::CompoundLiteralExpr>(array))
    return nonnull(objectSize(array->getType(), context));
  if (llvm::isa<clang::ArraySubscriptExpr>(array))
    return judgeAddress(array); // a row of a multi-dimensional array
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(array);
      unary != nullptr && unary->getOpcode() == clang::UO_Deref)
    return judge(unary->getSubExpr());
  return ValueWidth::unknown("an array the analysis cannot name");
}

ValueWidth KindInferenceState::judgeCall(const clang::CallExpr &call) const {
  const clang::QualType pointee = call.getType()->getPointeeType();
  const clang::FunctionDecl *callee = call.getDirectCallee();
  if (callee == nullptr) {
    // §9.3: through a closed slot, the join of the targets' results; through
    // an open one, the A3 default of a function outside the unit.
    if (options.slots == nullptr || options.slotSolution == nullptr)
      return ValueWidth::unknown("the result of an indirect call");
    const auto slot = options.slots->calleeSlot(call);
    if (!slot)
      return ValueWidth::unknown("the result of an indirect call");
    const core::CallResolution resolution =
        options.slotSolution->resolveCall(*slot);
    switch (resolution.kind) {
    case core::IndirectCallKind::OpenKnown:
    case core::IndirectCallKind::OpenUnknown:
      return ValueWidth::of(objectWidth(pointee, context));
    case core::IndirectCallKind::ClosedEmpty:
      return ValueWidth::unknown("a call through a pointer that is null");
    case core::IndirectCallKind::ClosedSingle:
    case core::IndirectCallKind::ClosedJoin:
      break;
    }
    std::optional<ValueWidth> joined;
    for (const std::string &target : resolution.targets) {
      const clang::FunctionDecl *function = options.slots->function(target);
      ValueWidth value = ValueWidth::unknown("the result of an indirect call");
      if (function != nullptr) {
        const auto found = nodeOf.find(function->getCanonicalDecl());
        value = found != nodeOf.end()
                    ? loadOf(found->second)
                    : ValueWidth::of(objectWidth(pointee, context));
      }
      joined = joined ? meet(*joined, value) : value;
    }
    return joined ? *joined
                  : ValueWidth::unknown("the result of an indirect call");
  }
  if (const auto match = governingLibraryEntry(*callee, library))
    return libraryValue(match->entry->result, *match, call, pointee);
  const KindEntry *entry = kinds.result(*callee);
  if (entry != nullptr && entry->hasDeclaredShape())
    return declaredWidth(*entry, pointee, &call);
  if (const auto found = nodeOf.find(callee->getCanonicalDecl());
      found != nodeOf.end() &&
      nodes[found->second].kind == KindNode::Kind::Result)
    return loadOf(found->second);
  // A3: the result of a function outside the unit is Single-or-nullable.
  return ValueWidth::of(objectWidth(pointee, context),
                        entry != nullptr && entry->declaresNonnull()
                            ? core::Nullability::Nonnull
                            : core::Nullability::Nullable);
}

// -- The fixpoint (§7.3)
// -------------------------------------------------------

void KindInferenceState::demote(unsigned index, const clang::Stmt *store,
                                std::string reason) {
  KindNode &node = nodes[index];
  if (node.demoted)
    return;
  node.demoted = true;
  // What demoted the node is not judged for nullness: nullable.
  node.nonnull = false;
  node.why = std::move(reason);
  (void)store;
}

/// The value one input brings to its node in the current round.
static ValueWidth valueOf(const KindInferenceState &state,
                          const Incoming &input) {
  if (input.fixed)
    return *input.fixed;
  if (input.node)
    return state.loadOf(*input.node);
  return state.judge(input.value);
}

void KindInferenceState::solve() {
  // A greatest fixpoint: every node starts `single` (and nonnull), and each
  // round demotes what an input no longer supports. Demotion, the least
  // width and the nullability only ever move down.
  bool changed = true;
  while (changed) {
    changed = false;
    for (unsigned index = 0; index < nodes.size(); ++index) {
      KindNode &node = nodes[index];
      if (node.demoted)
        continue;
      if (!node.forced.empty()) {
        demote(index, node.forced.front().store, node.forced.front().reason);
        changed = true;
        continue;
      }
      std::uint64_t least = KindNode::Top;
      bool nonnull = true;
      std::optional<std::string> why;
      for (const Incoming &input : node.incoming) {
        const ValueWidth value = valueOf(*this, input);
        if (value.nullOnly) {
          nonnull = false;
          continue;
        }
        if (value.nullability != core::Nullability::Nonnull)
          nonnull = false;
        if (!value.bytes) {
          why = value.why.empty()
                    ? std::string("a value that is not Single-valid")
                    : value.why;
          break;
        }
        least = std::min(least, *value.bytes);
      }
      if (!why && node.kind != KindNode::Kind::Value && least < node.needed)
        why = "a value with " + std::to_string(least) +
              " bytes, fewer than the " + std::to_string(node.needed) +
              " of '" + node.pointee.getAsString() + "'";
      if (why) {
        demote(index, nullptr, std::move(*why));
        changed = true;
        continue;
      }
      if (node.kind == KindNode::Kind::Value && least != node.least) {
        node.least = least;
        changed = true;
      }
      if (nonnull != node.nonnull) {
        node.nonnull = nonnull;
        changed = true;
      }
    }
  }
}

/// §13.1 `slotKinds.demotedBy`: the hidden stores, then every input that is
/// not Single-valid under the final kinds, in source order, once each.
static std::vector<KindDemotion> demotionsOf(const KindInferenceState &state,
                                             const KindNode &node) {
  std::vector<KindDemotion> out;
  const auto add = [&](KindDemotion demotion) {
    const bool seen = llvm::any_of(out, [&](const KindDemotion &each) {
      return each.store == demotion.store && each.reason == demotion.reason;
    });
    if (!seen)
      out.push_back(std::move(demotion));
  };
  for (const KindDemotion &forced : node.forced)
    add(forced);
  for (const Incoming &input : node.incoming) {
    const ValueWidth value = valueOf(state, input);
    if (value.covers(node.needed))
      continue;
    add(KindDemotion{
        .store = input.store,
        .reason = value.bytes ? "a value with " + std::to_string(*value.bytes) +
                                    " bytes, fewer than the " +
                                    std::to_string(node.needed) + " of '" +
                                    node.pointee.getAsString() + "'"
                              : value.why});
  }
  const clang::SourceManager &sm = state.context.getSourceManager();
  std::ranges::stable_sort(
      out, [&sm](const KindDemotion &a, const KindDemotion &b) {
        if (a.store == nullptr || b.store == nullptr)
          return a.store != nullptr && b.store == nullptr;
        const clang::SourceLocation left = a.store->getBeginLoc();
        const clang::SourceLocation right = b.store->getBeginLoc();
        return left != right && sm.isBeforeInTranslationUnit(left, right);
      });
  return out;
}

// -- The table
// ------------------------------------------------------------------

bool KindInferenceState::joinsArguments(
    const clang::FunctionDecl &function) const {
  const auto found = functions.find(function.getCanonicalDecl());
  if (found == functions.end() || found->second.definition == nullptr)
    return false;
  const FunctionFacts &facts = found->second;
  return !function.isExternallyVisible() && !function.isMain() &&
         !facts.addressTaken && !facts.calls.empty();
}

/// The shape of a slot, result or static parameter the fixpoint decided,
/// over what `entry` declares (its nullability).
static void inferShape(KindEntry &entry, bool single,
                       core::Nullability nullability) {
  entry.kind.shape =
      single ? core::PointerShape::Single : core::PointerShape::Unknown;
  entry.kind.extent = core::ExtentTerm{};
  entry.kind.source = core::KindSource::Inferred;
  if (!entry.nullabilityLevel)
    entry.kind.nullability = nullability;
  entry.extentClass = core::extentClassOf(entry.kind);
}

void KindInferenceState::fillTable() {
  // Slots.
  for (const KindNode &node : nodes) {
    if (node.kind != KindNode::Kind::Field &&
        node.kind != KindNode::Kind::Global)
      continue;
    const auto *field = llvm::dyn_cast<clang::FieldDecl>(node.decl);
    const auto *variable = llvm::dyn_cast<clang::VarDecl>(node.decl);
    const KindEntry *declared =
        field != nullptr ? kinds.field(*field) : kinds.variable(*variable);
    KindEntry entry = declared != nullptr ? *declared : KindEntry{};
    if (entry.shapeLevel)
      continue;
    inferShape(entry, !node.demoted, core::Nullability::Nullable);
    if (node.demoted)
      entry.demotedBy = demotionsOf(*this, node);
    if (field != nullptr)
      kinds.setField(*field, std::move(entry));
    else
      kinds.setVariable(*variable, std::move(entry));
  }

  // Functions: results, then parameters.
  std::vector<const clang::FunctionDecl *> all = referenced;
  for (const clang::FunctionDecl *definition : definitions)
    all.push_back(definition->getCanonicalDecl());
  llvm::DenseSet<const clang::FunctionDecl *> done;
  for (const clang::FunctionDecl *canonical : all) {
    if (!done.insert(canonical).second || kinds.governedByLibrary(*canonical))
      continue;
    const auto facts = functions.find(canonical);
    const clang::FunctionDecl *definition =
        facts != functions.end() ? facts->second.definition : nullptr;

    if (isObjectPointer(canonical->getReturnType())) {
      const KindEntry *declared = kinds.result(*canonical);
      KindEntry entry = declared != nullptr ? *declared : KindEntry{};
      if (!entry.shapeLevel) {
        const auto node = nodeOf.find(canonical);
        if (definition != nullptr && node != nodeOf.end()) {
          const KindNode &result = nodes[node->second];
          inferShape(entry, !result.demoted,
                     result.nonnull && !result.incoming.empty()
                         ? core::Nullability::Nonnull
                         : core::Nullability::Nullable);
          if (result.demoted)
            entry.demotedBy = demotionsOf(*this, result);
        } else {
          // A3: a result from outside the unit is Single-or-nullable.
          inferShape(entry, true, core::Nullability::Nullable);
          entry.kind.source = core::KindSource::Default;
        }
        kinds.setResult(*canonical, std::move(entry));
      }
    }

    if (definition == nullptr)
      continue;
    const bool join = joinsArguments(*definition);
    for (unsigned i = 0; i < definition->getNumParams(); ++i) {
      const clang::ParmVarDecl *param = definition->getParamDecl(i);
      if (!isObjectPointer(param->getType()))
        continue;
      const KindEntry *declared = kinds.param(*definition, i);
      KindEntry entry = declared != nullptr ? *declared : KindEntry{};
      if (!entry.shapeLevel) {
        if (i == 1 && isMainArgv(*definition)) {
          // §7.3: `counted(argc + 1) nonnull`, trusted as system-api.
          entry.kind = core::PointerKind::counted(
              core::ExtentTerm::of(core::ExtentPath::ofParam(0), 1, 1),
              core::Nullability::Nonnull, core::KindSource::Declared);
          entry.shapeLevel = KindLevel::SystemHeader;
          entry.nullabilityLevel = KindLevel::SystemHeader;
          entry.extentClass = core::ExtentClass::Declared;
          entry.mainArgv = true;
        } else if (const auto found = entryOf.find(param);
                   join && found != entryOf.end()) {
          const KindNode &start = nodes[found->second];
          inferShape(entry, !start.demoted,
                     start.nonnull ? core::Nullability::Nonnull
                                   : core::Nullability::Nullable);
          if (start.demoted)
            entry.demotedBy = demotionsOf(*this, start);
        } else {
          // A1: Single-or-nullable, and whether the body leans on it.
          inferShape(entry, true, core::Nullability::Nullable);
          entry.kind.source = core::KindSource::Default;
          entry.reliesOnSingle = reliesOnDefault(*param);
        }
      }
      kinds.setParam(*definition, i, std::move(entry));
    }
  }
}

void KindInferenceState::run() {
  collect();
  addHiddenStores();
  solve();
  fillTable();
  if (options.mustAccess)
    inferMustAccess();
  if (options.fieldCandidates)
    inferFieldCandidates();
  collectStoreGroups();
}

// -- The result
// -----------------------------------------------------------------

KindInferenceResult::KindInferenceResult() = default;
KindInferenceResult::~KindInferenceResult() = default;
KindInferenceResult::KindInferenceResult(KindInferenceResult &&) noexcept =
    default;
KindInferenceResult &
KindInferenceResult::operator=(KindInferenceResult &&) noexcept = default;

std::vector<FieldCandidate> KindInferenceResult::fieldCandidates() const {
  std::vector<FieldCandidate> out;
  if (state == nullptr)
    return out;
  for (const ResolvedCandidate &resolved : state->candidates)
    out.push_back(resolved.candidate);
  std::ranges::sort(out);
  return out;
}

const std::vector<ResolvedCandidate> &
KindInferenceResult::resolvedCandidates() const noexcept {
  static const std::vector<ResolvedCandidate> None;
  if (state == nullptr)
    return None;
  return state->candidates;
}

const std::vector<DisqualifiedRecord> &
KindInferenceResult::disqualified() const noexcept {
  static const std::vector<DisqualifiedRecord> None;
  if (state == nullptr)
    return None;
  return state->disqualified;
}

const std::vector<StoreGroup> &
KindInferenceResult::storeGroups() const noexcept {
  static const std::vector<StoreGroup> None;
  if (state == nullptr)
    return None;
  return state->groups;
}

bool KindInferenceResult::isSingleValid(const clang::Expr &value,
                                        clang::QualType pointee) const {
  return state != nullptr &&
         state->judge(&value).covers(objectWidth(pointee, state->context));
}

bool KindInferenceResult::argumentIsSingleValid(const clang::CallExpr &call,
                                                unsigned index) const {
  if (state == nullptr || index >= call.getNumArgs())
    return false;
  clang::QualType parameter;
  if (const clang::FunctionDecl *callee = call.getDirectCallee();
      callee != nullptr && index < callee->getNumParams()) {
    parameter = callee->getParamDecl(index)->getType();
  } else if (const auto *prototype = call.getCallee()
                                         ->getType()
                                         ->getPointeeOrArrayElementType()
                                         ->getAs<clang::FunctionProtoType>();
             prototype != nullptr && index < prototype->getNumParams()) {
    parameter = prototype->getParamType(index);
  } else {
    parameter = call.getArg(index)->getType();
  }
  if (!isObjectPointer(parameter))
    return false;
  return isSingleValid(*call.getArg(index), parameter->getPointeeType());
}

bool KindInferenceResult::alwaysReturns(
    const clang::FunctionDecl &function) const {
  return state != nullptr && state->alwaysReturns(function);
}

bool KindInferenceResult::knownToReturn(const clang::CallExpr &call) const {
  return state != nullptr && state->knownToReturn(call);
}

KindInferenceResult KindInference::infer(KindTable &kinds) {
  KindInferenceResult result;
  result.state =
      std::make_unique<KindInferenceState>(context, library, options, kinds);
  result.state->run();
  return result;
}

} // namespace weavec::analysis
