//===- PlaceBuilder.cpp - Clang expressions to core places ----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PlaceBuilder.h"

#include "weavec/Analysis/Allocators.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/OperationKinds.h"

#include "llvm/Support/Casting.h"

using namespace clang;

namespace weavec::analysis {

core::PlaceId PlaceBuilder::placeForVar(const VarDecl &var) {
  const VarDecl *canonical = var.getCanonicalDecl();
  if (const auto it = varPlaces.find(canonical); it != varPlaces.end())
    return it->second;
  const core::PlaceId id = places.create(var.getNameAsString());
  varPlaces.try_emplace(canonical, id);
  order.push_back(canonical);
  return id;
}

std::optional<core::PlaceId> PlaceBuilder::lookupVar(const VarDecl &var) const {
  const auto it = varPlaces.find(var.getCanonicalDecl());
  if (it == varPlaces.end())
    return std::nullopt;
  return it->second;
}

const VarDecl *PlaceBuilder::varForPlace(core::PlaceId place) const {
  for (const auto &[var, id] : varPlaces) {
    if (id == place)
      return var;
  }
  return nullptr;
}

bool PlaceBuilder::isTransparentCast(QualType from, QualType to) {
  if (!from->isPointerType() || !to->isPointerType())
    return false;
  const QualType fromPointee = from->getPointeeType().getUnqualifiedType();
  const QualType toPointee = to->getPointeeType().getUnqualifiedType();
  if (fromPointee.getCanonicalType() == toPointee.getCanonicalType())
    return true;
  const auto isGeneric = [](QualType pointee) {
    return pointee->isVoidType() || pointee->isCharType();
  };
  return isGeneric(fromPointee) || isGeneric(toPointee);
}

const Expr &PlaceBuilder::stripTransparent(const Expr &expr) {
  const Expr *current = &expr;
  while (true) {
    current = current->IgnoreParens();
    const auto *cast = dyn_cast<CastExpr>(current);
    if (cast == nullptr)
      return *current;
    switch (cast->getCastKind()) {
    case CK_LValueToRValue:
    case CK_NoOp:
    case CK_ArrayToPointerDecay:
      break;
    case CK_BitCast:
      if (!isTransparentCast(cast->getSubExpr()->getType(), cast->getType()))
        return *current;
      break;
    default:
      return *current;
    }
    current = cast->getSubExpr();
  }
}

bool PlaceBuilder::isPlaceExpr(const Expr &expr) {
  if (const auto *ref = dyn_cast<DeclRefExpr>(&expr))
    return isa<VarDecl>(ref->getDecl());
  if (isa<MemberExpr, ArraySubscriptExpr>(&expr))
    return true;
  if (const auto *unary = dyn_cast<UnaryOperator>(&expr))
    return unary->getOpcode() == UO_Deref;
  return false;
}

std::optional<PlaceRef> PlaceBuilder::resolvePointerValue(const Expr &expr) {
  const Expr &stripped = stripTransparent(expr);
  if (!isPlaceExpr(stripped))
    return std::nullopt;
  return resolve(stripped);
}

/// `*(p + k)` and `*(k + p)` denote an element of whatever `p` points to.
static const Expr *pointerOperandOfArithmetic(const Expr &expr) {
  const auto *binary = dyn_cast<BinaryOperator>(&expr);
  if (binary == nullptr)
    return nullptr;
  if (binary->getOpcode() != BO_Add && binary->getOpcode() != BO_Sub)
    return nullptr;
  if (binary->getLHS()->getType()->isPointerType())
    return binary->getLHS();
  if (binary->getOpcode() == BO_Add &&
      binary->getRHS()->getType()->isPointerType())
    return binary->getRHS();
  return nullptr;
}

std::optional<PlaceRef> PlaceBuilder::resolve(const Expr &expr) {
  const Expr &e = stripTransparent(expr);

  if (const auto *ref = dyn_cast<DeclRefExpr>(&e)) {
    const auto *var = dyn_cast<VarDecl>(ref->getDecl());
    if (var == nullptr)
      return std::nullopt;
    return PlaceRef{.place = placeForVar(*var), .derefs = {}, .derefExprs = {}};
  }

  if (const auto *member = dyn_cast<MemberExpr>(&e)) {
    const std::string field = member->getMemberDecl()->getNameAsString();
    const Expr &base = stripTransparent(*member->getBase());
    if (!member->isArrow()) {
      auto ref = resolve(base);
      if (!ref)
        return std::nullopt;
      ref->place = places.field(ref->place, field);
      return ref;
    }
    // `(&s)->f` is `s.f`.
    if (const auto *addr = dyn_cast<UnaryOperator>(&base);
        addr != nullptr && addr->getOpcode() == UO_AddrOf) {
      auto ref = resolve(*addr->getSubExpr());
      if (!ref)
        return std::nullopt;
      ref->place = places.field(ref->place, field);
      return ref;
    }
    auto pointer = resolvePointerValue(base);
    if (!pointer)
      return std::nullopt;
    pointer->addDeref(pointer->place, &base);
    pointer->place = places.field(places.deref(pointer->place), field);
    return pointer;
  }

  if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(&e)) {
    const Expr &base = stripTransparent(*subscript->getBase());
    if (base.getType()->isArrayType()) {
      auto ref = resolve(base);
      if (!ref)
        return std::nullopt;
      ref->place = places.index(ref->place);
      return ref;
    }
    auto pointer = resolvePointerValue(base);
    if (!pointer)
      return std::nullopt;
    pointer->addDeref(pointer->place, &base);
    pointer->place = places.deref(pointer->place);
    return pointer;
  }

  if (const auto *unary = dyn_cast<UnaryOperator>(&e);
      unary != nullptr && unary->getOpcode() == UO_Deref) {
    const Expr &operand = stripTransparent(*unary->getSubExpr());
    // `*&x` is `x`.
    if (const auto *addr = dyn_cast<UnaryOperator>(&operand);
        addr != nullptr && addr->getOpcode() == UO_AddrOf)
      return resolve(*addr->getSubExpr());
    const Expr *pointerExpr = pointerOperandOfArithmetic(operand);
    if (pointerExpr == nullptr)
      pointerExpr = &operand;
    auto pointer = resolvePointerValue(*pointerExpr);
    if (!pointer)
      return std::nullopt;
    pointer->addDeref(pointer->place, pointerExpr);
    pointer->place = places.deref(pointer->place);
    return pointer;
  }

  return std::nullopt;
}

static ValueOrigin makeOrigin(ValueOrigin::Kind kind,
                              std::optional<PlaceRef> place = std::nullopt,
                              const CallExpr *call = nullptr,
                              bool constObject = false) {
  ValueOrigin origin;
  origin.kind = kind;
  origin.place = std::move(place);
  origin.call = call;
  origin.constObject = constObject;
  return origin;
}

ValueOrigin PlaceBuilder::classifyValue(const Expr &expr) {
  const Expr *e = expr.IgnoreParens();

  if (const auto *cast = dyn_cast<CastExpr>(e)) {
    switch (cast->getCastKind()) {
    case CK_NullToPointer:
      return makeOrigin(ValueOrigin::Kind::Null);
    case CK_ArrayToPointerDecay: {
      auto ref = resolve(*cast->getSubExpr());
      if (!ref)
        return ValueOrigin{};
      ref->place = places.index(ref->place);
      const QualType arrayType = cast->getSubExpr()->getType();
      const ArrayType *array = arrayType->getAsArrayTypeUnsafe();
      const bool constObject =
          arrayType.isConstQualified() ||
          (array != nullptr && array->getElementType().isConstQualified());
      return makeOrigin(ValueOrigin::Kind::Borrow, std::move(ref), nullptr,
                        constObject);
    }
    case CK_LValueToRValue: {
      const Expr &sub = stripTransparent(*cast->getSubExpr());
      if (!isPlaceExpr(sub))
        return classifyValue(sub);
      auto ref = resolve(sub);
      if (!ref)
        return ValueOrigin{};
      return makeOrigin(ValueOrigin::Kind::Copy, std::move(ref));
    }
    case CK_NoOp:
      return classifyValue(*cast->getSubExpr());
    case CK_BitCast:
      if (isTransparentCast(cast->getSubExpr()->getType(), cast->getType()))
        return classifyValue(*cast->getSubExpr());
      return ValueOrigin{};
    default:
      return ValueOrigin{};
    }
  }

  if (const auto *call = dyn_cast<CallExpr>(e)) {
    const auto effects = classifyCall(*call);
    if (!effects || !effects->producesOwned)
      return ValueOrigin{};
    if (effects->isRealloc) {
      auto arg = resolvePointerValue(*call->getArg(0));
      return makeOrigin(ValueOrigin::Kind::Realloc, std::move(arg), call);
    }
    return makeOrigin(ValueOrigin::Kind::Alloc, std::nullopt, call);
  }

  if (const auto *unary = dyn_cast<UnaryOperator>(e);
      unary != nullptr && unary->getOpcode() == UO_AddrOf) {
    auto ref = resolve(*unary->getSubExpr());
    if (!ref)
      return ValueOrigin{};
    return makeOrigin(ValueOrigin::Kind::Borrow, std::move(ref), nullptr,
                      unary->getSubExpr()->getType().isConstQualified());
  }

  if (const auto *binary = dyn_cast<BinaryOperator>(e)) {
    if (binary->getOpcode() == BO_Assign) {
      auto ref = resolve(*binary->getLHS());
      if (!ref)
        return ValueOrigin{};
      return makeOrigin(ValueOrigin::Kind::Copy, std::move(ref));
    }
    if (binary->getOpcode() == BO_Comma)
      return classifyValue(*binary->getRHS());
    // Pointer arithmetic may leave the object: opaque (RFC 0002, *Places*).
    return ValueOrigin{};
  }

  if (const auto *conditional = dyn_cast<AbstractConditionalOperator>(e)) {
    ValueOrigin origin = makeOrigin(ValueOrigin::Kind::Conditional);
    origin.alternatives.push_back(classifyValue(*conditional->getTrueExpr()));
    origin.alternatives.push_back(classifyValue(*conditional->getFalseExpr()));
    return origin;
  }

  if (isPlaceExpr(*e)) {
    // An lvalue used where an rvalue was expected without an explicit load
    // (should not happen in C, but be robust).
    auto ref = resolve(*e);
    if (!ref)
      return ValueOrigin{};
    return makeOrigin(ValueOrigin::Kind::Copy, std::move(ref));
  }

  return ValueOrigin{};
}

} // namespace weavec::analysis
