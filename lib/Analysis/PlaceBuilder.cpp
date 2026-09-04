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

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Casting.h"

using namespace clang;

namespace weavec::analysis {

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

/// Marks a copy as pointing *into* its source's object rather than at the
/// same address (RFC 0006, *Alias exactness*); other origins are unchanged.
static ValueOrigin asInterior(ValueOrigin origin) {
  if (origin.kind == ValueOrigin::Kind::Copy)
    origin.interior = true;
  for (ValueOrigin &alternative : origin.alternatives)
    alternative = asInterior(std::move(alternative));
  return origin;
}

/// Records the subscript an access was spelled with. A second subscript on
/// the same path (`m[i][j]`) names an element no witness can identify.
static void setWitness(PlaceRef &ref, core::ElementWitness witness) {
  ref.element =
      ref.element.isWhole() ? witness : core::ElementWitness::unknown();
}

static ValueOrigin makeRaw(core::RawReason reason,
                           const CallExpr *call = nullptr,
                           const Expr *source = nullptr) {
  ValueOrigin origin = makeOrigin(ValueOrigin::Kind::Raw, std::nullopt, call);
  origin.rawReason = reason;
  origin.source = source;
  return origin;
}

core::PlaceId PlaceBuilder::placeForVar(const VarDecl &var) {
  const VarDecl *canonical = var.getCanonicalDecl();
  if (const auto it = varPlaces.find(canonical); it != varPlaces.end())
    return it->second;
  const core::PlaceId id = places.create(var.getNameAsString());
  varPlaces.try_emplace(canonical, id);
  placeVars.try_emplace(id.value, canonical);
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
  const auto it = placeVars.find(place.value);
  return it == placeVars.end() ? nullptr : it->second;
}

core::PlaceId PlaceBuilder::literalPlace() {
  if (!literal)
    literal = places.create("<string literal>");
  return *literal;
}

core::PlaceId PlaceBuilder::fieldPlace(core::PlaceId parent,
                                       const ValueDecl &member) {
  const core::PlaceId id = places.field(parent, member.getNameAsString());
  if (const auto *field = dyn_cast<FieldDecl>(&member))
    placeFields.try_emplace(id.value, field);
  return id;
}

const NamedDecl *PlaceBuilder::declFor(core::PlaceId place) const {
  if (const auto it = placeFields.find(place.value); it != placeFields.end())
    return it->second;
  return varForPlace(place);
}

bool PlaceBuilder::isDeclaredRaw(core::PlaceId place) const {
  const NamedDecl *decl = declFor(place);
  return decl != nullptr && getAnnotations(*decl).raw;
}

std::optional<core::SummaryPath>
PlaceBuilder::summaryPathOf(core::PlaceId place) {
  const core::PlaceId root = places.root(place);
  const VarDecl *var = varForPlace(root);
  if (var == nullptr)
    return std::nullopt;

  core::SummaryPath path;
  if (const auto *param = dyn_cast<ParmVarDecl>(var)) {
    path = core::SummaryPath::param(param->getFunctionScopeIndex());
  } else if (var->hasGlobalStorage()) {
    path = core::SummaryPath::global(summaries.globals().idFor(*var));
  } else {
    return std::nullopt;
  }

  // Ancestors come nearest-first; the path is spelled root-first.
  std::vector<core::PlaceId> chain{place};
  llvm::append_range(chain, places.ancestors(place));
  for (const core::PlaceId node : llvm::reverse(chain)) {
    if (node == root)
      continue;
    switch (places.step(node)) {
    case core::PathStep::Deref:
      path = path.deref();
      break;
    case core::PathStep::Field:
      path = path.field(places.fieldName(node));
      break;
    case core::PathStep::Index:
      path = path.indexed();
      break;
    }
  }
  return path;
}

std::optional<PlaceRef>
PlaceBuilder::resolveSummaryPath(const core::SummaryPath &path,
                                 const CallExpr &call) {
  PlaceRef ref;
  std::size_t firstStep = 0;
  const Expr *argExpr = nullptr;

  if (path.isParam()) {
    if (path.index >= call.getNumArgs())
      return std::nullopt;
    argExpr = call.getArg(path.index);
    const ValueOrigin origin = classifyValue(*argExpr);
    if (origin.kind == ValueOrigin::Kind::Copy && origin.place) {
      ref = *origin.place;
    } else if (origin.kind == ValueOrigin::Kind::Borrow && origin.place) {
      // The argument is `&x`: there is no place holding the pointer, and
      // `param(i)*` is `x` itself.
      if (path.steps.empty() ||
          path.steps.front().step != core::PathStep::Deref)
        return std::nullopt;
      ref = *origin.place;
      firstStep = 1;
    } else {
      return std::nullopt;
    }
  } else if (path.isGlobal()) {
    const VarDecl *global = summaries.globals().declFor(path.index);
    if (global == nullptr)
      return std::nullopt;
    ref = PlaceRef{.place = placeForVar(*global),
                   .derefs = {},
                   .derefExprs = {},
                   .derefElements = {},
                   .element = {}};
  } else {
    // A `result` root names the returned record (RFC 0008, *Struct-by-value
    // results*): no caller place until it is assigned; see `resolveBelow`.
    return std::nullopt;
  }

  for (std::size_t i = firstStep; i < path.steps.size(); ++i) {
    switch (path.steps[i].step) {
    case core::PathStep::Deref:
      // The first dereference is of the argument as written; deeper ones
      // are synthesised and report at the call.
      ref.addDeref(ref.place, i == firstStep ? argExpr : nullptr);
      ref.place = places.deref(ref.place);
      break;
    case core::PathStep::Field:
      ref.place = places.field(ref.place, path.steps[i].field);
      break;
    case core::PathStep::Index:
      // The callee's subscript is not visible here: the effect applies to
      // every element (RFC 0006, *Element witnesses*), and to an unknown
      // one if the argument itself was an element.
      ref.place = places.index(ref.place);
      setWitness(ref, core::ElementWitness::whole());
      break;
    }
  }
  return ref;
}

std::optional<core::PlaceId>
PlaceBuilder::resolveBelow(core::PlaceId base, const core::SummaryPath &path) {
  core::PlaceId place = base;
  for (const core::PathElem &elem : path.steps) {
    switch (elem.step) {
    case core::PathStep::Deref:
      return std::nullopt;
    case core::PathStep::Field:
      place = places.field(place, elem.field);
      break;
    case core::PathStep::Index:
      place = places.index(place);
      break;
    }
  }
  return place;
}

/// The caller-side place a summary path's root denotes at `call`, and the
/// index of the first step still to apply: `1` when the argument is `&x`
/// and the path's first step is the dereference `x` already is.
std::optional<std::pair<core::PlaceId, std::size_t>>
PlaceBuilder::lookupSummaryRoot(const core::SummaryPath &path,
                                const CallExpr &call) {
  if (path.isResult())
    return std::nullopt;
  if (path.isGlobal()) {
    const VarDecl *global = summaries.globals().declFor(path.index);
    if (global == nullptr)
      return std::nullopt;
    return std::pair{placeForVar(*global), std::size_t{0}};
  }
  if (path.index >= call.getNumArgs())
    return std::nullopt;
  const ValueOrigin origin = classifyValue(*call.getArg(path.index));
  if (origin.kind == ValueOrigin::Kind::Copy && origin.place)
    return std::pair{origin.place->place, std::size_t{0}};
  if (origin.kind == ValueOrigin::Kind::Borrow && origin.place) {
    if (path.steps.empty() || path.steps.front().step != core::PathStep::Deref)
      return std::nullopt;
    return std::pair{origin.place->place, std::size_t{1}};
  }
  return std::nullopt;
}

std::optional<core::PlaceId>
PlaceBuilder::lookupSummaryPath(const core::SummaryPath &path,
                                const CallExpr &call) {
  const auto root = lookupSummaryRoot(path, call);
  if (!root)
    return std::nullopt;
  core::PlaceId place = root->first;
  for (std::size_t i = root->second; i < path.steps.size(); ++i) {
    const auto child =
        places.child(place, path.steps[i].step, path.steps[i].field);
    if (!child)
      return std::nullopt;
    place = *child;
  }
  return place;
}

std::optional<core::PlaceId>
PlaceBuilder::lookupSummaryPath(const core::SummaryPath &path,
                                const CallExpr &call, PathLookupCache &cache) {
  // A `&x` argument makes `firstStep` depend on the path's first step, so a
  // root is shared only between paths that agree on it; a root that found
  // nothing is looked up again (the failure may have been the path's shape).
  const bool sameRoot = cache.rootKnown && cache.last &&
                        cache.last->root == path.root &&
                        cache.last->index == path.index &&
                        (cache.firstStep == 0 ||
                         (!path.steps.empty() &&
                          path.steps.front().step == core::PathStep::Deref));
  if (!sameRoot) {
    cache.chain.clear();
    const auto root = lookupSummaryRoot(path, call);
    cache.rootKnown = root.has_value();
    if (root) {
      cache.chain.push_back(root->first);
      cache.firstStep = root->second;
    }
  }
  if (!cache.rootKnown) {
    cache.last = path;
    return std::nullopt;
  }
  // Keep the prefix shared with the previous path, then extend.
  std::size_t common = 0;
  if (sameRoot) {
    const auto &prev = cache.last->steps;
    while (cache.firstStep + common < prev.size() &&
           cache.firstStep + common < path.steps.size() &&
           prev[cache.firstStep + common] ==
               path.steps[cache.firstStep + common])
      ++common;
  }
  const std::size_t wanted = path.steps.size() - cache.firstStep;
  cache.last = path;
  if (common + 1 <= cache.chain.size()) {
    cache.chain.resize(common + 1);
  } else {
    // The previous path already failed inside the shared prefix.
    return std::nullopt;
  }
  for (std::size_t k = cache.chain.size() - 1; k < wanted; ++k) {
    const core::PathElem &elem = path.steps[cache.firstStep + k];
    const auto child = places.child(cache.chain.back(), elem.step, elem.field);
    if (!child)
      return std::nullopt;
    cache.chain.push_back(*child);
  }
  return cache.chain.back();
}

ValueOrigin PlaceBuilder::originFromSource(const core::ValueSource &source,
                                           const CallExpr &call,
                                           const core::FunctionSummary &of) {
  const auto fresh = [&call](std::string family) {
    ValueOrigin origin =
        makeOrigin(ValueOrigin::Kind::Alloc, std::nullopt, &call);
    origin.family = std::move(family);
    return origin;
  };
  switch (source.kind) {
  case core::ValueSource::Kind::Fresh:
    return fresh(source.family);
  case core::ValueSource::Kind::Copy: {
    if (!source.path)
      return ValueOrigin{};
    // `T *f(T *WEAVEC_OWNED p) { ...; return p; }`: the caller's pointer is
    // dead and the result is the same resource, now owned by the result.
    if (source.path->isParam() && source.path->isRoot()) {
      if (source.path->index >= call.getNumArgs())
        return ValueOrigin{};
      if (of.consumes(source.path->index))
        return fresh(of.effectOf(*source.path).family);
      // The argument value itself, whatever it was: `&x` stays a borrow of
      // `x`, `malloc(n)` stays an allocation, `p` is a copy of `p`.
      ValueOrigin origin = classifyValue(*call.getArg(source.path->index));
      return source.interior ? asInterior(std::move(origin)) : origin;
    }
    // The same for a path below an argument or a global (`t->array =
    // resizearray(L, t, ...)` in Lua): a copy of a resource the callee
    // consumed is that resource, not a dangling pointer to it (RFC 0006,
    // *Interactions*). A copy of a path *freed* on some class only
    // (`state->x.next = state->out` in a body whose error path frees
    // `state->out`) is nobody's new resource, and a test of the result
    // (`== -1`) cannot retract the class: the value is not tracked (RFC
    // 0007, *Applying a summary: deepest paths first*). A path the callee
    // consumed *and replaced* (`p->buffer = realloc(p->buffer, n); return
    // p->buffer + p->offset;`, cJSON's `ensure`) holds the new value, and
    // the copy is of that: an ordinary copy of the caller's place (RFC
    // 0008, *Replaced values*).
    if (const core::PlaceEffect effect = of.effectOf(*source.path);
        effect.consumed() && !effect.replaced) {
      if (effect.moved || of.consumesUnconditionally(*source.path))
        return fresh(effect.family);
      return ValueOrigin{};
    }
    auto ref = resolveSummaryPath(*source.path, call);
    if (!ref)
      return ValueOrigin{};
    ValueOrigin origin = makeOrigin(ValueOrigin::Kind::Copy, std::move(ref));
    origin.interior = source.interior;
    return origin;
  }
  case core::ValueSource::Kind::Borrow: {
    if (!source.path)
      return ValueOrigin{};
    auto ref = resolveSummaryPath(*source.path, call);
    if (!ref)
      return ValueOrigin{};
    return makeOrigin(ValueOrigin::Kind::Borrow, std::move(ref));
  }
  case core::ValueSource::Kind::Null:
    return makeOrigin(ValueOrigin::Kind::Null);
  case core::ValueSource::Kind::Raw:
    return makeRaw(core::RawReason::Callee, &call);
  case core::ValueSource::Kind::Unknown:
    return ValueOrigin{};
  }
  return ValueOrigin{};
}

bool PlaceBuilder::isTransparentCast(QualType from, QualType to) {
  // A pointer cast changes the type an object is viewed through, never the
  // object; only integer round-trips lose provenance (RFC 0004).
  return from->isPointerType() && to->isPointerType();
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

std::optional<PlaceRef> PlaceBuilder::resolvePointerValue(const Expr &expr) {
  const Expr &stripped = stripTransparent(expr);
  // `free(s - header)` releases the object `s` points into (RFC 0004,
  // *Pointer identity*): the argument names `s`'s object, at another offset.
  if (const Expr *pointer = pointerOperandOfArithmetic(stripped))
    return resolvePointerValue(*pointer);
  if (!isPlaceExpr(stripped))
    return std::nullopt;
  return resolve(stripped);
}

std::optional<PlaceRef> PlaceBuilder::resolve(const Expr &expr) {
  const Expr &e = stripTransparent(expr);

  if (const auto *ref = dyn_cast<DeclRefExpr>(&e)) {
    const auto *var = dyn_cast<VarDecl>(ref->getDecl());
    if (var == nullptr)
      return std::nullopt;
    return PlaceRef{.place = placeForVar(*var),
                    .derefs = {},
                    .derefExprs = {},
                    .derefElements = {},
                    .element = {}};
  }

  if (const auto *member = dyn_cast<MemberExpr>(&e)) {
    const ValueDecl &field = *member->getMemberDecl();
    const Expr &base = stripTransparent(*member->getBase());
    if (!member->isArrow()) {
      auto ref = resolve(base);
      if (!ref)
        return std::nullopt;
      ref->place = fieldPlace(ref->place, field);
      return ref;
    }
    // `(&s)->f` is `s.f`.
    if (const auto *addr = dyn_cast<UnaryOperator>(&base);
        addr != nullptr && addr->getOpcode() == UO_AddrOf) {
      auto ref = resolve(*addr->getSubExpr());
      if (!ref)
        return std::nullopt;
      ref->place = fieldPlace(ref->place, field);
      return ref;
    }
    // `a->f` on an array is `a[0].f`, like `*a` below.
    if (base.getType()->isArrayType() && isPlaceExpr(base)) {
      auto ref = resolve(base);
      if (!ref)
        return std::nullopt;
      ref->place = places.index(ref->place);
      setWitness(*ref, core::ElementWitness::ofConstant(0));
      ref->place = fieldPlace(ref->place, field);
      return ref;
    }
    auto pointer = resolvePointerValue(base);
    if (!pointer)
      return std::nullopt;
    pointer->addDeref(pointer->place, &base);
    pointer->place = fieldPlace(places.deref(pointer->place), field);
    return pointer;
  }

  if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(&e)) {
    const Expr &base = stripTransparent(*subscript->getBase());
    const core::ElementWitness witness = witnessOf(*subscript->getIdx());
    if (base.getType()->isArrayType()) {
      auto ref = resolve(base);
      if (!ref)
        return std::nullopt;
      ref->place = places.index(ref->place);
      setWitness(*ref, witness);
      return ref;
    }
    auto pointer = resolvePointerValue(base);
    if (!pointer)
      return std::nullopt;
    pointer->addDeref(pointer->place, &base);
    pointer->place = places.deref(pointer->place);
    setWitness(*pointer, witness);
    return pointer;
  }

  if (const auto *unary = dyn_cast<UnaryOperator>(&e);
      unary != nullptr && unary->getOpcode() == UO_Deref) {
    const Expr &operand = stripTransparent(*unary->getSubExpr());
    // `*&x` is `x`.
    if (const auto *addr = dyn_cast<UnaryOperator>(&operand);
        addr != nullptr && addr->getOpcode() == UO_AddrOf)
      return resolve(*addr->getSubExpr());
    // `*a` on an array is its first element, `a[0]`.
    if (operand.getType()->isArrayType() && isPlaceExpr(operand)) {
      auto ref = resolve(operand);
      if (!ref)
        return std::nullopt;
      ref->place = places.index(ref->place);
      setWitness(*ref, core::ElementWitness::ofConstant(0));
      return ref;
    }
    const Expr *pointerExpr = pointerOperandOfArithmetic(operand);
    const Expr *indexExpr = nullptr;
    if (pointerExpr != nullptr) {
      const auto *binary = cast<BinaryOperator>(&operand);
      indexExpr =
          pointerExpr == binary->getLHS() ? binary->getRHS() : binary->getLHS();
    } else {
      pointerExpr = &operand;
    }
    auto pointer = resolvePointerValue(*pointerExpr);
    if (!pointer)
      return std::nullopt;
    pointer->addDeref(pointer->place, pointerExpr);
    pointer->place = places.deref(pointer->place);
    if (indexExpr != nullptr)
      setWitness(*pointer, witnessOf(*indexExpr));
    return pointer;
  }

  return std::nullopt;
}

core::ElementWitness PlaceBuilder::witnessOf(const Expr &index) {
  const Expr *e = index.IgnoreParenImpCasts();
  if (Expr::EvalResult result;
      !e->isValueDependent() && e->EvaluateAsInt(result, context) &&
      result.Val.isInt() && result.Val.getInt().getSignificantBits() <= 64)
    return core::ElementWitness::ofConstant(result.Val.getInt().getSExtValue());
  if (const auto *ref = dyn_cast<DeclRefExpr>(e)) {
    if (const auto *var = dyn_cast<VarDecl>(ref->getDecl()))
      return core::ElementWitness::ofVariable(placeForVar(*var));
  }
  return core::ElementWitness::unknown();
}

std::optional<ValueOrigin> PlaceBuilder::rawBaseOf(const Expr &placeExpr) {
  const Expr &e = stripTransparent(placeExpr);
  const Expr *base = nullptr;
  if (const auto *member = dyn_cast<MemberExpr>(&e)) {
    base = member->isArrow() ? member->getBase() : nullptr;
    if (base == nullptr)
      return rawBaseOf(*member->getBase());
  } else if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(&e)) {
    const Expr &sub = stripTransparent(*subscript->getBase());
    if (sub.getType()->isArrayType())
      return rawBaseOf(sub);
    base = subscript->getBase();
  } else if (const auto *unary = dyn_cast<UnaryOperator>(&e);
             unary != nullptr && unary->getOpcode() == UO_Deref) {
    const Expr &operand = stripTransparent(*unary->getSubExpr());
    const Expr *pointerExpr = pointerOperandOfArithmetic(operand);
    base = pointerExpr != nullptr ? pointerExpr : &operand;
  } else {
    return std::nullopt;
  }
  const Expr &stripped = stripTransparent(*base);
  if (isPlaceExpr(stripped))
    return rawBaseOf(stripped);
  ValueOrigin origin = classifyValue(stripped);
  if (origin.kind == ValueOrigin::Kind::Raw)
    return origin;
  return std::nullopt;
}

ValueOrigin PlaceBuilder::classifyValue(const Expr &expr) {
  const Expr *e = expr.IgnoreParens();

  if (const auto *cast = dyn_cast<CastExpr>(e)) {
    switch (cast->getCastKind()) {
    case CK_NullToPointer:
      return makeOrigin(ValueOrigin::Kind::Null);
    case CK_IntegralToPointer:
      // Provenance is lost on the way through the integer (RFC 0004).
      return makeRaw(core::RawReason::IntegerCast, nullptr, cast);
    case CK_ArrayToPointerDecay: {
      // A string literal (or `__func__`) is a borrow of static storage that
      // is not a heap object (RFC 0008, *Invalid releases*).
      if (isa<StringLiteral, PredefinedExpr>(
              cast->getSubExpr()->IgnoreParens())) {
        return makeOrigin(ValueOrigin::Kind::Borrow,
                          PlaceRef{.place = literalPlace(),
                                   .derefs = {},
                                   .derefExprs = {},
                                   .derefElements = {},
                                   .element = {}},
                          nullptr, /*constObject=*/true);
      }
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
    const auto effects = classifyCall(*call, summaries);
    if (!effects) {
      // Unchecked code: under strict mode its result is raw (RFC 0004,
      // *Boundaries*); by default nothing is known about it beyond what its
      // declaration says about nullness (RFC 0008), hence the call.
      if (strictExterns)
        return makeRaw(core::RawReason::UnknownCallee, call);
      ValueOrigin opaque;
      opaque.call = call;
      return opaque;
    }
    const core::FunctionSummary &summary = *effects->summary;
    if (summary.returns.empty())
      return ValueOrigin{};
    if (summary.returns.size() == 1) {
      ValueOrigin origin =
          originFromSource(*summary.returns.begin(), *call, summary);
      if (origin.call == nullptr)
        origin.call = call;
      return origin;
    }
    ValueOrigin origin = makeOrigin(ValueOrigin::Kind::Conditional);
    origin.call = call;
    for (const core::ValueSource &source : summary.returns)
      origin.alternatives.push_back(originFromSource(source, *call, summary));
    return origin;
  }

  if (const auto *unary = dyn_cast<UnaryOperator>(e)) {
    switch (unary->getOpcode()) {
    case UO_AddrOf: {
      auto ref = resolve(*unary->getSubExpr());
      if (!ref)
        return ValueOrigin{};
      return makeOrigin(ValueOrigin::Kind::Borrow, std::move(ref), nullptr,
                        unary->getSubExpr()->getType().isConstQualified());
    }
    case UO_PreInc:
    case UO_PreDec:
    case UO_PostInc:
    case UO_PostDec: {
      // `p++` as a value: an interior pointer into the same object (RFC
      // 0004, *Pointer identity*).
      auto ref = resolve(*unary->getSubExpr());
      if (!ref)
        return ValueOrigin{};
      return asInterior(makeOrigin(ValueOrigin::Kind::Copy, std::move(ref)));
    }
    default:
      return ValueOrigin{};
    }
  }

  if (const auto *binary = dyn_cast<BinaryOperator>(e)) {
    if (binary->getOpcode() == BO_Assign) {
      auto ref = resolve(*binary->getLHS());
      if (!ref)
        return ValueOrigin{};
      return makeOrigin(ValueOrigin::Kind::Copy, std::move(ref));
    }
    if (binary->isCompoundAssignmentOp()) {
      auto ref = resolve(*binary->getLHS());
      if (!ref)
        return ValueOrigin{};
      // `p += k` as a value is interior like `p + k`.
      return asInterior(makeOrigin(ValueOrigin::Kind::Copy, std::move(ref)));
    }
    if (binary->getOpcode() == BO_Comma)
      return classifyValue(*binary->getRHS());
    // `p + k` refers to the same object as `p` (RFC 0004, *Pointer
    // identity*) but not to the same address (RFC 0006, *Alias exactness*);
    // whether it stays in bounds is outside the model. `p + 0` is `p`.
    if (const Expr *pointer = pointerOperandOfArithmetic(*binary)) {
      const Expr *offset =
          pointer == binary->getLHS() ? binary->getRHS() : binary->getLHS();
      if (const auto value = offset->getIntegerConstantExpr(context);
          value && value->isZero())
        return classifyValue(*pointer);
      return asInterior(classifyValue(*pointer));
    }
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
