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

#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
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

/// Steps a value's offset within its object (RFC 0011, *Derived pointers*):
/// a copy, an allocation or a borrow moved by `step` points that much
/// further into what it pointed into; the alternatives of a conditional are
/// stepped one by one; other origins are unchanged.
static ValueOrigin withOffset(ValueOrigin origin,
                              const core::PointerOffset &step) {
  if (origin.kind == ValueOrigin::Kind::Copy ||
      origin.kind == ValueOrigin::Kind::Alloc ||
      origin.kind == ValueOrigin::Kind::Borrow)
    origin.offset = origin.offset.plus(step);
  for (ValueOrigin &alternative : origin.alternatives)
    alternative = withOffset(std::move(alternative), step);
  return origin;
}

/// The size in bytes of a complete object type, else nothing (`void`, an
/// incomplete record, a function).
static std::optional<std::int64_t> sizeOfType(QualType type,
                                              const ASTContext &context) {
  if (type.isNull() || type->isIncompleteType() || type->isFunctionType())
    return std::nullopt;
  return static_cast<std::int64_t>(
      context.getTypeSizeInChars(type).getQuantity());
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
  if (chain.size() - 1 > MaxPlaceDepth)
    return std::nullopt;
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

std::optional<PlaceRef> PlaceBuilder::copyOrNull(const ValueOrigin &origin) {
  if (origin.kind == ValueOrigin::Kind::Copy)
    return origin.place;
  // `f(obj_ref(p))` where `obj_ref` returns `{copy param 0, null when[param
  // 0 null]}`: the value is `p` wherever it is anything (RFC 0010, the
  // returning-ref shape).
  if (origin.kind != ValueOrigin::Kind::Conditional)
    return std::nullopt;
  std::optional<PlaceRef> copied;
  for (const ValueOrigin &alternative : origin.alternatives) {
    if (alternative.kind == ValueOrigin::Kind::Null)
      continue;
    const auto here = copyOrNull(alternative);
    if (!here || (copied && copied->place != here->place))
      return std::nullopt;
    copied = here;
  }
  return copied;
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
    // The argument is `&x` (or `&p->f`, RFC 0011: a derived copy of `p`,
    // but `param(i)*` is still `p->f` itself): there is no place holding
    // the pointer, and `param(i)*` is `x`.
    const bool derefFirst =
        !path.steps.empty() && path.steps.front().step == core::PathStep::Deref;
    const auto addressed = addressedPlace(*argExpr);
    const ValueOrigin origin = classifyValue(*argExpr);
    if (addressed && derefFirst) {
      ref = *addressed;
      firstStep = 1;
    } else if (const auto copied = copyOrNull(origin)) {
      ref = *copied;
    } else if (origin.kind == ValueOrigin::Kind::Borrow && origin.place &&
               derefFirst) {
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
  const Expr &argExpr = *call.getArg(path.index);
  const bool derefFirst =
      !path.steps.empty() && path.steps.front().step == core::PathStep::Deref;
  if (const auto addressed = addressedPlace(argExpr); addressed && derefFirst)
    return std::pair{addressed->place, std::size_t{1}};
  const ValueOrigin origin = classifyValue(argExpr);
  if (const auto copied = copyOrNull(origin))
    return std::pair{copied->place, std::size_t{0}};
  if (origin.kind == ValueOrigin::Kind::Borrow && origin.place && derefFirst)
    return std::pair{origin.place->place, std::size_t{1}};
  return std::nullopt;
}

std::optional<PlaceRef> PlaceBuilder::addressedPlace(const Expr &expr) {
  const Expr &stripped = stripTransparent(expr);
  if (const auto *unary = dyn_cast<UnaryOperator>(&stripped);
      unary != nullptr && unary->getOpcode() == UO_AddrOf) {
    // `&p->f` names the sub-object the derived pointer points to, spelled as
    // the pointer's dereference plus the offset's fields: through a union
    // member that is `*p` itself, not `(*p).m` (RFC 0011, *Deriving a
    // pointer*), so writes through the derived pointer and through the
    // argument agree on the place. Elsewhere the two spellings coincide and
    // the lvalue's own resolution keeps the dereference expressions.
    auto resolved = resolve(*unary->getSubExpr());
    if (const auto derivation = derivationOf(*unary->getSubExpr());
        derivation &&
        (derivation->offset.isZero() ||
         derivation->offset.kind == core::PointerOffset::Kind::Field)) {
      ValueOrigin origin =
          makeOrigin(ValueOrigin::Kind::Copy, derivation->pointer);
      origin.offset = derivation->offset;
      if (auto pointee = pointeeOf(origin);
          pointee && (!resolved || pointee->place != resolved->place))
        return pointee;
    }
    return resolved;
  }
  // Array decay: `buf` passed where a pointer is expected is `&buf[0]`, and
  // `param(i)*` is the array's elements.
  if (const auto *cast = dyn_cast<CastExpr>(stripped.IgnoreParens());
      cast != nullptr && cast->getCastKind() == CK_ArrayToPointerDecay &&
      !isa<StringLiteral, PredefinedExpr>(cast->getSubExpr()->IgnoreParens())) {
    auto ref = resolve(*cast->getSubExpr());
    if (!ref)
      return std::nullopt;
    ref->place = places.index(ref->place);
    return ref;
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

std::optional<ValueOrigin>
PlaceBuilder::originFromSource(const core::ValueSource &source,
                               const CallExpr &call,
                               const core::FunctionSummary &of) {
  // The guard first: an alternative the arguments rule out is no
  // alternative (RFC 0009).
  std::optional<core::PlaceGuard> guard = translateGuard(source.when, call);
  if (!guard)
    return std::nullopt;
  ValueOrigin origin = originFromUnguardedSource(source, call, of);
  origin.guard = std::move(*guard);
  return origin;
}

ValueOrigin
PlaceBuilder::originFromUnguardedSource(const core::ValueSource &source,
                                        const CallExpr &call,
                                        const core::FunctionSummary &of) {
  const auto fresh = [&call](std::string family) {
    ValueOrigin origin =
        makeOrigin(ValueOrigin::Kind::Alloc, std::nullopt, &call);
    origin.family = std::move(family);
    return origin;
  };
  switch (source.kind) {
  case core::ValueSource::Kind::Fresh: {
    ValueOrigin origin = fresh(source.family);
    origin.offset = source.offset;
    if (source.extent)
      origin.extent = affineFromPath(*source.extent, call);
    else
      origin.extent = productExtentOf(call);
    return origin;
  }
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
      return withOffset(std::move(origin), source.offset);
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
    origin.offset = source.offset;
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

/// The mathematical value of `value`, read with its own signedness.
static std::optional<std::int64_t>
mathematicalValue(const llvm::APSInt &value) {
  if (value.isSigned()) {
    if (value.getSignificantBits() > 64)
      return std::nullopt;
    return value.getSExtValue();
  }
  if (value.getActiveBits() > 63)
    return std::nullopt;
  return static_cast<std::int64_t>(value.getZExtValue());
}

std::optional<std::int64_t> integerConstant(const Expr &expr,
                                            const ASTContext &context) {
  Expr::EvalResult result;
  if (expr.isValueDependent() || !expr.getType()->isIntegerType() ||
      !expr.EvaluateAsInt(result, context) || !result.Val.isInt())
    return std::nullopt;
  return mathematicalValue(result.Val.getInt());
}

std::optional<std::int64_t> integerConvertedTo(std::int64_t value,
                                               QualType type,
                                               const ASTContext &context) {
  if (!type->isIntegerType())
    return std::nullopt;
  llvm::APSInt converted(
      llvm::APInt(64, static_cast<std::uint64_t>(value), /*isSigned=*/true),
      /*isUnsigned=*/false);
  converted = converted.extOrTrunc(context.getIntWidth(type));
  converted.setIsUnsigned(type->isUnsignedIntegerType());
  return mathematicalValue(converted);
}

/// An integer type, looking through `_Atomic` (RFC 0010: a count may be an
/// atomic integer).
static bool isIntegerLike(QualType type) {
  if (type.isNull())
    return false;
  if (const auto *atomic = type->getAs<AtomicType>())
    type = atomic->getValueType();
  return type->isIntegerType();
}

/// A member of a union (RFC 0011: all of them start at the union's address).
static bool isUnionMember(const ValueDecl &member) {
  const auto *field = dyn_cast<FieldDecl>(&member);
  return field != nullptr && field->getParent() != nullptr &&
         field->getParent()->isUnion();
}

/// The place the pointer argument of an adjusting builtin names: `&x` is
/// `x`; any other pointer value is what it points to.
static std::optional<PlaceRef> pointeeOfArgument(PlaceBuilder &builder,
                                                 const Expr &argument) {
  const Expr &stripped = PlaceBuilder::stripTransparent(argument);
  if (const auto *addr = dyn_cast<UnaryOperator>(&stripped);
      addr != nullptr && addr->getOpcode() == UO_AddrOf) {
    const Expr &operand = PlaceBuilder::stripTransparent(*addr->getSubExpr());
    if (!PlaceBuilder::isPlaceExpr(operand))
      return std::nullopt;
    return builder.resolve(operand);
  }
  auto pointer = builder.resolvePointerValue(stripped);
  if (!pointer)
    return std::nullopt;
  pointer->addDeref(pointer->place, &stripped);
  pointer->place = builder.table().deref(pointer->place);
  return pointer;
}

std::optional<PlaceBuilder::Adjustment>
PlaceBuilder::adjustmentOf(const Expr &expr) {
  const Expr *e = expr.IgnoreParens();
  // `++x`, `x++`, `--x`, `x--`.
  if (const auto *unary = dyn_cast<UnaryOperator>(e);
      unary != nullptr && unary->isIncrementDecrementOp()) {
    const Expr &operand = stripTransparent(*unary->getSubExpr());
    if (!isIntegerLike(operand.getType()) || !isPlaceExpr(operand))
      return std::nullopt;
    auto place = resolve(operand);
    if (!place)
      return std::nullopt;
    const int delta = unary->isIncrementOp() ? 1 : -1;
    return Adjustment{.place = std::move(*place),
                      .delta = delta,
                      .valueOffset = unary->isPostfix() ? -delta : 0,
                      .operand = &operand};
  }
  // `x += 1`, `x -= 1`.
  if (const auto *compound = dyn_cast<CompoundAssignOperator>(e);
      compound != nullptr && (compound->getOpcode() == BO_AddAssign ||
                              compound->getOpcode() == BO_SubAssign)) {
    const Expr &operand = stripTransparent(*compound->getLHS());
    if (!isIntegerLike(operand.getType()) || !isPlaceExpr(operand))
      return std::nullopt;
    const auto k = integerConstant(*compound->getRHS(), context);
    if (!k || (*k != 1 && *k != -1))
      return std::nullopt;
    auto place = resolve(operand);
    if (!place)
      return std::nullopt;
    const int delta =
        static_cast<int>(compound->getOpcode() == BO_AddAssign ? *k : -*k);
    return Adjustment{.place = std::move(*place),
                      .delta = delta,
                      .valueOffset = 0,
                      .operand = &operand};
  }
  // `__atomic_*` and `__c11_atomic_*` are `AtomicExpr`s.
  if (const auto *atomic = dyn_cast<AtomicExpr>(e)) {
    bool add = false;
    bool yieldsOld = false;
    switch (atomic->getOp()) {
    case AtomicExpr::AO__atomic_fetch_add:
    case AtomicExpr::AO__c11_atomic_fetch_add:
    case AtomicExpr::AO__scoped_atomic_fetch_add:
      add = true;
      yieldsOld = true;
      break;
    case AtomicExpr::AO__atomic_add_fetch:
    case AtomicExpr::AO__scoped_atomic_add_fetch:
      add = true;
      break;
    case AtomicExpr::AO__atomic_fetch_sub:
    case AtomicExpr::AO__c11_atomic_fetch_sub:
    case AtomicExpr::AO__scoped_atomic_fetch_sub:
      yieldsOld = true;
      break;
    case AtomicExpr::AO__atomic_sub_fetch:
    case AtomicExpr::AO__scoped_atomic_sub_fetch:
      break;
    default:
      return std::nullopt;
    }
    const auto k = integerConstant(*atomic->getVal1(), context);
    if (!k || *k != 1)
      return std::nullopt;
    auto place = pointeeOfArgument(*this, *atomic->getPtr());
    if (!place || !isIntegerLike(atomic->getPtr()->getType()->getPointeeType()))
      return std::nullopt;
    const int delta = add ? 1 : -1;
    return Adjustment{.place = std::move(*place),
                      .delta = delta,
                      .valueOffset = yieldsOld ? -delta : 0,
                      .operand = atomic->getPtr()};
  }
  // `__sync_fetch_and_add(&x, 1)` and friends are calls to builtins, which
  // Sema rewrites to the sized form (`__sync_fetch_and_add_4`).
  if (const auto *call = dyn_cast<CallExpr>(e)) {
    const FunctionDecl *callee = call->getDirectCallee();
    if (callee == nullptr || callee->getBuiltinID() == 0 ||
        call->getNumArgs() < 2)
      return std::nullopt;
    llvm::StringRef name = callee->getName();
    if (!name.consume_front("__sync_"))
      return std::nullopt;
    bool add = false;
    bool yieldsOld = false;
    if (name.consume_front("fetch_and_add")) {
      add = true;
      yieldsOld = true;
    } else if (name.consume_front("add_and_fetch")) {
      add = true;
    } else if (name.consume_front("fetch_and_sub")) {
      yieldsOld = true;
    } else if (!name.consume_front("sub_and_fetch")) {
      return std::nullopt;
    }
    // Only the size suffix (`_4`) may follow.
    if (!name.empty()) {
      const bool sizeSuffix = name.consume_front("_") && !name.empty() &&
                              llvm::all_of(name, llvm::isDigit);
      if (!sizeSuffix)
        return std::nullopt;
    }
    const auto k = integerConstant(*call->getArg(1), context);
    if (!k || *k != 1)
      return std::nullopt;
    auto place = pointeeOfArgument(*this, *call->getArg(0));
    if (!place || !isIntegerLike(call->getArg(0)->getType()->getPointeeType()))
      return std::nullopt;
    const int delta = add ? 1 : -1;
    return Adjustment{.place = std::move(*place),
                      .delta = delta,
                      .valueOffset = yieldsOld ? -delta : 0,
                      .operand = call->getArg(0)};
  }
  return std::nullopt;
}

PlaceBuilder::ScalarOperand PlaceBuilder::scalarOperand(const Expr &expr) {
  ScalarOperand operand;
  if (const auto value = integerConstant(expr, context)) {
    operand.constant = core::ValueFact::ofConstant(*value);
    return operand;
  }
  const Expr *e = &expr;
  for (;;) {
    e = e->IgnoreParens();
    // RFC 0010: `--x == 0`, `x-- == 1`, `__atomic_fetch_sub(&x, 1, o) == 1`
    // read the adjusted place, at an offset for the forms that yield the
    // old value.
    if (auto adjustment = adjustmentOf(*e)) {
      operand.place = std::move(adjustment->place);
      operand.offset = adjustment->valueOffset;
      return operand;
    }
    if (const auto *cast = dyn_cast<CastExpr>(e)) {
      // A conversion keeps zero-ness and, absent overflow, sign (RFC 0009,
      // *Assumptions*); a narrowing or sign-changing one loses the exact
      // constant.
      switch (cast->getCastKind()) {
      case CK_LValueToRValue:
      case CK_NoOp:
        break;
      case CK_IntegralCast:
        if (!cast->getSubExpr()->getType()->isIntegerType())
          return operand;
        operand.scaled = true;
        break;
      default:
        return operand;
      }
      e = cast->getSubExpr();
      continue;
    }
    // `n * 8`, `8 * n`: zero exactly when `n` is, same sign as `n`.
    if (const auto *binary = dyn_cast<BinaryOperator>(e);
        binary != nullptr && binary->getOpcode() == BO_Mul) {
      const auto lhs = integerConstant(*binary->getLHS(), context);
      const auto rhs = integerConstant(*binary->getRHS(), context);
      if (rhs && *rhs > 0) {
        operand.scaled = true;
        e = binary->getLHS();
        continue;
      }
      if (lhs && *lhs > 0) {
        operand.scaled = true;
        e = binary->getRHS();
        continue;
      }
      return operand;
    }
    break;
  }
  if (!e->getType()->isIntegerType() || !isPlaceExpr(*e))
    return operand;
  operand.place = resolve(*e);
  return operand;
}

std::optional<core::PlaceGuard>
PlaceBuilder::translateGuard(const core::PathGuard &guard,
                             const CallExpr &call) {
  core::PlaceGuard translated;
  for (const auto &[path, fact] : guard.conditions) {
    if (path.isParam() && path.isRoot()) {
      if (path.index >= call.getNumArgs())
        continue;
      const Expr &arg = *call.getArg(path.index);
      if (arg.getType()->isPointerType()) {
        // A null constant or an address decides a pointer conjunct on the
        // spot.
        const ValueOrigin value = classifyValue(arg);
        if (value.kind == ValueOrigin::Kind::Null ||
            value.kind == ValueOrigin::Kind::Borrow) {
          const core::Outcome actual = value.kind == ValueOrigin::Kind::Null
                                           ? core::Outcome::Null
                                           : core::Outcome::NonNull;
          if (fact.disjointFrom(core::ValueFact::of(actual)))
            return std::nullopt;
          continue;
        }
        if (const auto ref = resolvePointerValue(arg))
          translated.require(ref->place, fact);
        continue;
      }
      if (!arg.getType()->isIntegerType())
        continue;
      const ScalarOperand operand = scalarOperand(arg);
      if (operand.constant) {
        if (operand.constant->disjointFrom(fact))
          return std::nullopt;
        // Satisfied by the constant: nothing left to check later.
        continue;
      }
      if (!operand.place)
        continue;
      core::ValueFact onPlace = fact;
      if (operand.scaled)
        onPlace.constant.reset();
      translated.require(operand.place->place, onPlace);
      continue;
    }
    if (const auto ref = resolveSummaryPath(path, call))
      translated.require(ref->place, fact);
  }
  return translated;
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

const Expr *PlaceBuilder::pointerOperandOfArithmetic(const Expr &expr) {
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
  // `o->payload` decaying is `&o->payload[0]`: the value is `o` stepped to
  // the field (RFC 0011, *Deriving a pointer*), not the array place. A
  // variable's own array (`buf`) stays its storage.
  if (stripped.getType()->isArrayType()) {
    if (auto derivation = derivationOf(stripped))
      return std::move(derivation->pointer);
  }
  return resolve(stripped);
}

std::optional<PlaceRef> PlaceBuilder::resolveConsumedValue(const Expr &expr) {
  if (auto ref = resolvePointerValue(expr))
    return ref;
  // `free(&o->in)`, `take(&p[i])`: a derived pointer names the object of
  // the pointer it derives from (RFC 0011, *Deriving a pointer*).
  const Expr &stripped = stripTransparent(expr);
  if (const auto *unary = dyn_cast<UnaryOperator>(&stripped);
      unary != nullptr && unary->getOpcode() == UO_AddrOf) {
    if (auto derivation = derivationOf(*unary->getSubExpr()))
      return std::move(derivation->pointer);
  }
  return std::nullopt;
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
        auto origin = makeOrigin(ValueOrigin::Kind::Borrow,
                                 PlaceRef{.place = literalPlace(),
                                          .derefs = {},
                                          .derefExprs = {},
                                          .derefElements = {},
                                          .element = {}},
                                 nullptr, /*constObject=*/true);
        // RFC 0011, *Extents*: a literal's extent is its length plus the
        // terminator, in elements.
        if (const auto *text =
                dyn_cast<StringLiteral>(cast->getSubExpr()->IgnoreParens()))
          origin.extent = core::Affine::ofConstant(
              static_cast<std::int64_t>(text->getLength()) + 1);
        return origin;
      }
      // `p->a` decaying is `&p->a[0]`: a copy of `p` stepped to the field
      // (RFC 0011, *Deriving a pointer*), so that a release through it
      // reaches `p`'s object and its extent bounds the elements.
      if (auto derived = derivationOf(*cast->getSubExpr())) {
        ValueOrigin origin =
            makeOrigin(ValueOrigin::Kind::Copy, std::move(derived->pointer));
        origin.offset = std::move(derived->offset);
        return origin;
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
    // The alternatives the arguments do not rule out (RFC 0009). None left
    // means the summary's guards were too specific for what is known here:
    // the callee returned something, of unknown origin.
    std::vector<ValueOrigin> alternatives;
    for (const core::ValueSource &source : summary.returns) {
      if (auto origin = originFromSource(source, *call, summary))
        alternatives.push_back(std::move(*origin));
    }
    if (alternatives.empty()) {
      ValueOrigin opaque;
      opaque.call = call;
      return opaque;
    }
    if (alternatives.size() == 1) {
      ValueOrigin origin = std::move(alternatives.front());
      if (origin.call == nullptr)
        origin.call = call;
      return origin;
    }
    ValueOrigin origin = makeOrigin(ValueOrigin::Kind::Conditional);
    origin.call = call;
    origin.alternatives = std::move(alternatives);
    return origin;
  }

  if (const auto *unary = dyn_cast<UnaryOperator>(e)) {
    switch (unary->getOpcode()) {
    case UO_AddrOf: {
      // `&p->f`, `&p[i]`, `&*p`: a copy of `p` stepped into its object (RFC
      // 0011, *Deriving a pointer*), not a borrow of what `p` points to.
      if (auto derived = derivationOf(*unary->getSubExpr())) {
        ValueOrigin origin =
            makeOrigin(ValueOrigin::Kind::Copy, std::move(derived->pointer));
        origin.offset = std::move(derived->offset);
        return origin;
      }
      auto ref = resolve(*unary->getSubExpr());
      if (!ref)
        return ValueOrigin{};
      // `&a[3]` is the storage of `a` at `+3`.
      core::PointerOffset offset;
      if (!ref->element.isWhole()) {
        offset = ref->element.kind == core::ElementWitness::Kind::Constant
                     ? core::PointerOffset::ofElements(ref->element.constant)
                     : core::PointerOffset::unknown();
      }
      ValueOrigin origin =
          makeOrigin(ValueOrigin::Kind::Borrow, std::move(ref), nullptr,
                     unary->getSubExpr()->getType().isConstQualified());
      origin.offset = std::move(offset);
      return origin;
    }
    case UO_PreInc:
    case UO_PreDec:
    case UO_PostInc:
    case UO_PostDec: {
      // `p++` as a value: the same object (RFC 0004, *Pointer identity*),
      // one element before where `p` now points (RFC 0011); `++p` is `p`.
      auto ref = resolve(*unary->getSubExpr());
      if (!ref)
        return ValueOrigin{};
      ValueOrigin origin = makeOrigin(ValueOrigin::Kind::Copy, std::move(ref));
      if (unary->getOpcode() == UO_PostInc)
        origin.offset = core::PointerOffset::ofElements(-1);
      else if (unary->getOpcode() == UO_PostDec)
        origin.offset = core::PointerOffset::ofElements(1);
      return origin;
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
      // `p += k` as a value is `p` after the step (RFC 0011).
      auto ref = resolve(*binary->getLHS());
      if (!ref)
        return ValueOrigin{};
      return makeOrigin(ValueOrigin::Kind::Copy, std::move(ref));
    }
    if (binary->getOpcode() == BO_Comma)
      return classifyValue(*binary->getRHS());
    // `p + k` refers to the same object as `p` (RFC 0004, *Pointer
    // identity*) at another offset (RFC 0011, *Derived pointers*); whether
    // the offset stays in bounds is the spatial record's business.
    if (const Expr *pointer = pointerOperandOfArithmetic(*binary))
      return withOffset(classifyValue(*pointer),
                        arithmeticStepOf(*binary, *pointer));
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

// -- Derived pointers and extents (RFC 0011) ----------------------------------

std::optional<core::Affine> PlaceBuilder::affineOf(const Expr &expr) {
  if (const auto value = integerConstant(expr, context))
    return core::Affine::ofConstant(*value);
  const Expr *e = expr.IgnoreParens();
  if (const auto *cast = dyn_cast<CastExpr>(e)) {
    switch (cast->getCastKind()) {
    case CK_LValueToRValue:
    case CK_NoOp:
    case CK_IntegralCast:
      // A conversion keeps the value absent overflow (RFC 0009,
      // *Assumptions*).
      if (!cast->getSubExpr()->getType()->isIntegerType())
        return std::nullopt;
      return affineOf(*cast->getSubExpr());
    default:
      return std::nullopt;
    }
  }
  if (const auto *binary = dyn_cast<BinaryOperator>(e)) {
    const auto lhs = integerConstant(*binary->getLHS(), context);
    const auto rhs = integerConstant(*binary->getRHS(), context);
    switch (binary->getOpcode()) {
    case BO_Add:
      if (rhs)
        if (const auto base = affineOf(*binary->getLHS()))
          return base->shifted(*rhs);
      if (lhs)
        if (const auto base = affineOf(*binary->getRHS()))
          return base->shifted(*lhs);
      return std::nullopt;
    case BO_Sub:
      if (rhs && *rhs != INT64_MIN)
        if (const auto base = affineOf(*binary->getLHS()))
          return base->shifted(-*rhs);
      return std::nullopt;
    case BO_Mul:
      if (rhs && *rhs > 0)
        if (const auto base = affineOf(*binary->getLHS()))
          return base->times(*rhs);
      if (lhs && *lhs > 0)
        if (const auto base = affineOf(*binary->getRHS()))
          return base->times(*lhs);
      return std::nullopt;
    default:
      return std::nullopt;
    }
  }
  if (!e->getType()->isIntegerType() || !isPlaceExpr(*e))
    return std::nullopt;
  const auto ref = resolve(*e);
  if (!ref)
    return std::nullopt;
  return core::Affine::ofPlace(ref->place);
}

std::string
PlaceBuilder::fieldKeyFor(QualType record,
                          llvm::ArrayRef<core::PathElem> fields) const {
  if (record.isNull() || fields.empty())
    return {};
  return countFieldKey(record, fields, context);
}

std::optional<PlaceBuilder::Derivation>
PlaceBuilder::derivationOf(const Expr &lvalue) {
  // Walk the lvalue outside-in, collecting the field path below the
  // dereference that ends it. An index step after a field, or a second
  // dereference's worth of arithmetic, makes the offset unknown.
  std::vector<core::PathElem> fields;
  bool unknown = false;
  const Expr *e = &stripTransparent(lvalue);
  const Expr *pointerExpr = nullptr;
  core::PointerOffset offset;
  QualType record;
  // The record the collected fields are looked up in when a union member
  // was crossed: the member's own type, not the pointee's.
  QualType fieldsRecord;
  for (;;) {
    if (const auto *member = dyn_cast<MemberExpr>(e)) {
      // Every member of a union starts where the union does: `&u->m` is `u`
      // (RFC 0011, *Deriving a pointer*), so the step adds nothing to the
      // offset. `(&cast_u(o))->th` is `o` itself, `&cast_u(o)->th.stack` is
      // `o` at `lua_State.stack`.
      const ValueDecl &decl = *member->getMemberDecl();
      if (isUnionMember(decl)) {
        if (!fields.empty() && fieldsRecord.isNull())
          fieldsRecord = decl.getType();
      } else if (!fieldsRecord.isNull()) {
        // Fields on both sides of a union member: two records, one key
        // cannot spell it.
        unknown = true;
      } else {
        fields.insert(fields.begin(),
                      core::PathElem{.step = core::PathStep::Field,
                                     .field = decl.getNameAsString()});
      }
      const Expr &base = stripTransparent(*member->getBase());
      if (!member->isArrow()) {
        e = &base;
        continue;
      }
      // `(&s)->f` is `s.f`: a variable's storage.
      if (const auto *addr = dyn_cast<UnaryOperator>(&base);
          addr != nullptr && addr->getOpcode() == UO_AddrOf)
        return std::nullopt;
      if (base.getType()->isArrayType())
        return std::nullopt;
      pointerExpr = &base;
      // The record is the type the member was looked up in: `(&cast_u(o))->th`
      // reads `o` through a union, so the field path belongs to the union, not
      // to the (transparent) cast's operand.
      record = member->getBase()->getType()->getPointeeType();
      break;
    }
    if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(e)) {
      const Expr &base = stripTransparent(*subscript->getBase());
      if (base.getType()->isArrayType()) {
        // `p->a[3]`: an index below a field (unknown, RFC 0011, *Deriving a
        // pointer*), except `p->a[0]`, which is the field itself; `s.a[3]`:
        // storage.
        const core::ElementWitness witness = witnessOf(*subscript->getIdx());
        if (witness.kind != core::ElementWitness::Kind::Constant ||
            witness.constant != 0)
          unknown = true;
        e = &base;
        continue;
      }
      pointerExpr = &base;
      if (fields.empty() && !unknown) {
        const core::ElementWitness witness = witnessOf(*subscript->getIdx());
        offset = witness.kind == core::ElementWitness::Kind::Constant
                     ? core::PointerOffset::ofElements(witness.constant)
                     : core::PointerOffset::unknown();
      } else {
        unknown = true;
      }
      break;
    }
    if (const auto *unary = dyn_cast<UnaryOperator>(e);
        unary != nullptr && unary->getOpcode() == UO_Deref) {
      const Expr &operand = stripTransparent(*unary->getSubExpr());
      if (const auto *addr = dyn_cast<UnaryOperator>(&operand);
          addr != nullptr && addr->getOpcode() == UO_AddrOf)
        return std::nullopt;
      if (operand.getType()->isArrayType())
        return std::nullopt;
      // `*(p + k)`: an element step, composed with the fields.
      if (const Expr *pointer = pointerOperandOfArithmetic(operand)) {
        const auto *binary = cast<BinaryOperator>(&operand);
        pointerExpr = pointer;
        if (fields.empty())
          offset = arithmeticStepOf(*binary, *pointer);
        else
          unknown = true;
      } else {
        pointerExpr = &operand;
      }
      // As above: the type the dereference produced, casts included.
      record = unary->getType();
      break;
    }
    // A variable, a call, anything else: not reached through a pointer.
    return std::nullopt;
  }
  if (pointerExpr == nullptr || !pointerExpr->getType()->isPointerType())
    return std::nullopt;
  // The pointer itself must be a place (or arithmetic on one, which
  // `resolvePointerValue` looks through: then the offset is unknown).
  const Expr &pointerStripped = stripTransparent(*pointerExpr);
  if (pointerOperandOfArithmetic(pointerStripped) != nullptr)
    unknown = true;
  auto pointer = resolvePointerValue(*pointerExpr);
  if (!pointer)
    return std::nullopt;
  if (!unknown && !fields.empty()) {
    const std::string key =
        fieldKeyFor(fieldsRecord.isNull() ? record : fieldsRecord, fields);
    if (key.empty())
      unknown = true;
    else
      offset = core::PointerOffset::ofField(key);
  }
  // A sub-object the offset rules cannot spell (an index below a field, two
  // field paths, a field the key cannot name): inside the object, in no
  // known relation to `*p`.
  if (unknown)
    offset = core::PointerOffset::inside();
  return Derivation{.pointer = std::move(*pointer),
                    .offset = std::move(offset)};
}

std::vector<std::string>
PlaceBuilder::fieldsOfOffset(const core::PointerOffset &offset) {
  std::vector<std::string> fields;
  if (offset.kind != core::PointerOffset::Kind::Field)
    return fields;
  // `countFieldKey`: the record's type key (which contains no `.`), then one
  // `.field` per step.
  llvm::StringRef rest(offset.field);
  const auto dot = rest.find('.');
  if (dot == llvm::StringRef::npos)
    return fields;
  rest = rest.drop_front(dot + 1);
  while (!rest.empty()) {
    const auto [head, tail] = rest.split('.');
    fields.emplace_back(head.str());
    rest = tail;
  }
  return fields;
}

std::optional<PlaceRef> PlaceBuilder::pointeeOf(const ValueOrigin &origin) {
  if (!origin.place)
    return std::nullopt;
  if (origin.kind == ValueOrigin::Kind::Borrow)
    return *origin.place;
  if (origin.kind != ValueOrigin::Kind::Copy)
    return std::nullopt;
  // `*p` stands for every element of `p`'s array, so an element offset
  // (known or not) still points into it; a sub-object at an unspellable
  // offset does not.
  if (origin.offset.isInside())
    return std::nullopt;
  PlaceRef pointee = *origin.place;
  pointee.addDeref(pointee.place, nullptr);
  pointee.place = places.deref(pointee.place);
  for (const std::string &field : fieldsOfOffset(origin.offset))
    pointee.place = places.field(pointee.place, field);
  return pointee;
}

std::optional<core::PointerOffset>
PlaceBuilder::pointerStepOf(const Expr &expr) {
  const Expr *e = expr.IgnoreParens();
  if (!e->getType()->isPointerType())
    return std::nullopt;
  if (const auto *unary = dyn_cast<UnaryOperator>(e)) {
    switch (unary->getOpcode()) {
    case UO_PreInc:
    case UO_PostInc:
      return core::PointerOffset::ofElements(1);
    case UO_PreDec:
    case UO_PostDec:
      return core::PointerOffset::ofElements(-1);
    default:
      return std::nullopt;
    }
  }
  if (const auto *binary = dyn_cast<CompoundAssignOperator>(e)) {
    if (binary->getOpcode() != BO_AddAssign &&
        binary->getOpcode() != BO_SubAssign)
      return std::nullopt;
    const auto k = integerConstant(*binary->getRHS(), context);
    if (!k || *k == INT64_MIN)
      return core::PointerOffset::unknown();
    return core::PointerOffset::ofElements(
        binary->getOpcode() == BO_AddAssign ? *k : -*k);
  }
  return std::nullopt;
}

core::PointerOffset PlaceBuilder::arithmeticStepOf(const BinaryOperator &binary,
                                                   const Expr &pointer) {
  const Expr *offsetExpr =
      &pointer == binary.getLHS() ? binary.getRHS() : binary.getLHS();
  const bool subtract = binary.getOpcode() == BO_Sub;
  const QualType pointee = pointer.getType()->getPointeeType();
  const std::optional<std::int64_t> pointeeSize = sizeOfType(pointee, context);
  // `(char *)p + k`: `k` is in bytes of `char`, not elements of what `p`
  // really points to. Compare the arithmetic's element size with the
  // underlying place's.
  const Expr &underlying = stripTransparent(pointer);
  std::optional<std::int64_t> underlyingSize;
  if (underlying.getType()->isPointerType())
    underlyingSize =
        sizeOfType(underlying.getType()->getPointeeType(), context);
  else if (const ArrayType *array =
               underlying.getType()->getAsArrayTypeUnsafe())
    // `buf + k` on an array: the decay is transparent, the element is the
    // array's.
    underlyingSize = sizeOfType(array->getElementType(), context);
  const bool sameUnits = pointeeSize && underlyingSize &&
                         *pointeeSize == *underlyingSize && *pointeeSize > 0;
  // `(char *)q - offsetof(T, f)`: the field `f` of `T`, subtracted. Before
  // the constant case: `offsetof` is an integer constant expression too, and
  // its value (0 for a first field) is not what it means (RFC 0011,
  // *Assumptions*: the field path is matched, never the number).
  if (const auto *offsetOf =
          dyn_cast<OffsetOfExpr>(offsetExpr->IgnoreParenImpCasts());
      offsetOf != nullptr && pointeeSize && *pointeeSize == 1) {
    std::vector<core::PathElem> fields;
    QualType record = offsetOf->getTypeSourceInfo()->getType();
    for (unsigned i = 0; i < offsetOf->getNumComponents(); ++i) {
      const OffsetOfNode &node = offsetOf->getComponent(i);
      if (node.getKind() != OffsetOfNode::Field)
        return core::PointerOffset::inside();
      const FieldDecl &field = *node.getField();
      // A union member adds nothing (`derivationOf` spells `&u->m` as `u`);
      // the members below it are looked up in its own type.
      if (isUnionMember(field)) {
        if (!fields.empty())
          return core::PointerOffset::inside();
        record = field.getType();
        continue;
      }
      fields.push_back(core::PathElem{.step = core::PathStep::Field,
                                      .field = field.getNameAsString()});
    }
    if (fields.empty())
      return core::PointerOffset::zero();
    const std::string key = fieldKeyFor(record, fields);
    if (key.empty())
      return core::PointerOffset::inside();
    return core::PointerOffset::ofField(key, subtract);
  }
  // Arithmetic in another unit than the object's elements (`(char *)p + k`
  // for a non-`char` `p`) lands somewhere inside; in the object's own
  // elements it is a number of them the checker may not know.
  if (const auto k = integerConstant(*offsetExpr, context)) {
    if (*k == 0)
      return core::PointerOffset::zero();
    if (!sameUnits)
      return core::PointerOffset::inside();
    if (*k == INT64_MIN)
      return core::PointerOffset::unknown();
    return core::PointerOffset::ofElements(subtract ? -*k : *k);
  }
  return sameUnits ? core::PointerOffset::unknown()
                   : core::PointerOffset::inside();
}

std::optional<core::Affine>
PlaceBuilder::affineFromPath(const core::PathAffine &affine,
                             const CallExpr &call) {
  if (!affine.path)
    return core::Affine::ofConstant(affine.constant);
  std::optional<core::Affine> base;
  if (affine.path->isParam() && affine.path->isRoot()) {
    if (affine.path->index >= call.getNumArgs())
      return std::nullopt;
    base = affineOf(*call.getArg(affine.path->index));
  } else if (const auto ref = resolveSummaryPath(*affine.path, call)) {
    base = core::Affine::ofPlace(ref->place);
  }
  if (!base)
    return std::nullopt;
  const auto scaled = base->times(affine.scale);
  if (!scaled)
    return std::nullopt;
  return scaled->shifted(affine.constant);
}

std::optional<core::Affine>
PlaceBuilder::productExtentOf(const CallExpr &call) {
  // `calloc(n, size)` and `reallocarray(p, n, size)` allocate a product the
  // summary format cannot spell; it is affine when one factor is constant.
  const FunctionDecl *callee = call.getDirectCallee();
  if (callee == nullptr || !callee->isGlobal() ||
      callee->getIdentifier() == nullptr)
    return std::nullopt;
  const llvm::StringRef name = callee->getName();
  unsigned first = 0;
  if (name == "calloc")
    first = 0;
  else if (name == "reallocarray")
    first = 1;
  else
    return std::nullopt;
  if (call.getNumArgs() < first + 2)
    return std::nullopt;
  const auto lhs = affineOf(*call.getArg(first));
  const auto rhs = affineOf(*call.getArg(first + 1));
  if (!lhs || !rhs)
    return std::nullopt;
  if (rhs->isConstant() && rhs->constant > 0)
    return lhs->times(rhs->constant);
  if (lhs->isConstant() && lhs->constant > 0)
    return rhs->times(lhs->constant);
  return std::nullopt;
}

} // namespace weavec::analysis
