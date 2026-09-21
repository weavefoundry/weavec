//===- KindSeeding.cpp - Pointer kinds in the engine (RFC 0030) -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §15 item 14 and §7.3–§7.5: what `FunctionDataflow` takes from the
// unit's kinds (`AnalysisOptions::kinds`, built before the engine runs):
//
//   - extents and nullness at parameter entry (the declared kind, the §7.3
//     static join or A1's Single default, and what the direct calls of a
//     static function enforce), at loads of fields and globals (slot kinds)
//     and at call results (result kinds, A3 outside the unit). Each extent
//     keeps its class: a Single or inferred kind is a lower bound, which
//     discharges the accesses it covers, while every other access through
//     it stays `unresolved(unknown-extent)` and is never checked against it
//     (§7.1). A seeded record counts as declared, so the summary still
//     records what an access needs behind a parameter (RFC 0011);
//   - at a direct call of a static function, a spatial and a null
//     requirement record per inferred requirement, decided under its guard
//     (§7.5, §10.4); and the §7.3 row of a value that may be a cursor,
//     passed where the callee relies on its Single default;
//   - the accesses a requirement covers: proven in a static function (its
//     calls enforce the requirement), and `trusted(caller-contract)` in an
//     exported or address-taken one when only an extent beyond Single
//     covers them (their null facets stay as §3.2 decides, §7.5).
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"
#include "weavec/Analysis/KindInference.h"

#include "llvm/Support/CheckedArithmetic.h"

using namespace clang;

namespace weavec::analysis {

/// §7.1: the bytes a Single `T *` guarantees (1 for `void`).
static std::optional<std::int64_t>
singleWidth(QualType pointee,
            const std::function<std::optional<std::int64_t>(QualType)> &width) {
  if (pointee.isNull())
    return std::nullopt;
  if (pointee->isVoidType())
    return 1;
  return width(pointee);
}

/// The bytes of one element of a `T *` (1 for `void`, as `counted` counts
/// bytes there).
static std::optional<std::int64_t> elementBytes(QualType pointee,
                                                const ASTContext &context) {
  if (pointee.isNull())
    return std::nullopt;
  if (pointee->isVoidType())
    return 1;
  return byteSizeOf(pointee, context);
}

/// `term` elements of `unit` bytes, as bytes over the place `path` resolves
/// its path to.
static std::optional<core::Affine> termBytes(
    const core::ExtentTerm &term, std::int64_t unit,
    const std::function<std::optional<core::PlaceId>(const core::ExtentPath &)>
        &placeOf) {
  const auto offset = llvm::checkedMul(term.offset, unit);
  if (!offset)
    return std::nullopt;
  if (term.isConstant())
    return core::Affine::ofConstant(*offset);
  const auto scale = llvm::checkedMul(term.scale, unit);
  const auto place = placeOf(*term.path);
  if (!scale || !place || *scale <= 0)
    return std::nullopt;
  return core::Affine::ofPlace(*place, *scale, *offset);
}

/// §7.5: the requirement's guard holds whatever the arguments (`0 < 8`).
static bool alwaysHolds(const RequirementGuard &guard) {
  if (!guard.lhs.isConstant() || !guard.rhs.isConstant())
    return false;
  return guard.relation == RequirementGuard::Relation::Less
             ? guard.lhs.offset < guard.rhs.offset
             : guard.lhs.offset <= guard.rhs.offset;
}

/// §7.5: a guarded count that is zero or less whenever its guard fails
/// (`0 < n -> counted(n)`), so it holds at entry unconditionally.
static bool vacuousWhenFalse(const MustAccessRequirement &requirement) {
  if (!requirement.guard)
    return true;
  const RequirementGuard &guard = *requirement.guard;
  const core::PointerKind &kind = requirement.kind;
  if (kind.shape != core::PointerShape::Counted &&
      kind.shape != core::PointerShape::Sized)
    return false;
  if (!guard.lhs.isConstant() || guard.rhs.isConstant() ||
      kind.extent.isConstant() || kind.extent.path != guard.rhs.path ||
      kind.extent.scale != guard.rhs.scale || guard.rhs.scale <= 0)
    return false;
  // Failing, the guard leaves `rhs <= last`; the count is `rhs` shifted.
  std::int64_t last = guard.lhs.offset;
  if (guard.relation == RequirementGuard::Relation::LessEqual)
    last -= 1;
  const auto shift = llvm::checkedSub(kind.extent.offset, guard.rhs.offset);
  const auto most = shift ? llvm::checkedAdd(last, *shift) : std::nullopt;
  return most && *most <= 0;
}

/// §7.5: the requirements of a parameter whose null part the direct calls
/// check (`SiteCollector`'s `inferredNullNeed`): every one when some
/// non-null requirement is unguarded, else those under the first guard.
static bool nullEnforced(const KindEntry &entry,
                         const MustAccessRequirement &requirement) {
  if (requirement.kind.nullability != core::Nullability::Nonnull)
    return false;
  const MustAccessRequirement *first = nullptr;
  for (const MustAccessRequirement &each : entry.mustAccess) {
    if (each.kind.nullability != core::Nullability::Nonnull)
      continue;
    if (!each.guard)
      return true;
    if (first == nullptr)
      first = &each;
  }
  return first != nullptr && first->guard == requirement.guard;
}

void FunctionDataflow::seedParameter(const ParmVarDecl &param,
                                     core::PlaceId place,
                                     core::AnalysisState &state) {
  const unsigned index = param.getFunctionScopeIndex();
  const core::SourceLocation at = locate(param.getLocation());
  if (options.kinds == nullptr) {
    // Without kinds (a bare `FunctionAnalyzer`), RFC 0011's annotation:
    // the caller passes `n` elements.
    if (const auto sized = sizedByOf(function, index))
      state.spatial.set(
          place, core::SpatialRecord{
                     .extent = core::Affine::ofPlace(
                         builder.placeForVar(*sized->count), sized->unit),
                     .offset = core::PointerOffset::zero(),
                     .location = at,
                     .declared = true,
                     .extentClass = core::ExtentClass::Declared});
    return;
  }
  const KindEntry *entry = options.kinds->param(function, index);
  if (entry == nullptr || !param.getType()->isPointerType())
    return;
  const QualType pointee = param.getType()->getPointeeType();
  const auto placeOf =
      [&](const core::ExtentPath &path) -> std::optional<core::PlaceId> {
    if (path.root != core::ExtentPath::Root::Param ||
        path.param >= function.getNumParams())
      return std::nullopt;
    return builder.placeForVar(*function.getParamDecl(path.param));
  };
  const auto width = [this](QualType type) { return objectWidthOf(type); };
  const auto recordOf =
      [&](const core::PointerKind &kind,
          core::ExtentClass extentClass) -> std::optional<core::SpatialRecord> {
    std::optional<core::Affine> extent;
    if (kind.shape == core::PointerShape::Single) {
      if (const auto bytes = singleWidth(pointee, width))
        extent = core::Affine::ofConstant(*bytes);
      extentClass = core::ExtentClass::LowerBound;
    } else if (kind.shape == core::PointerShape::Counted) {
      if (const auto unit = elementBytes(pointee, context))
        extent = termBytes(kind.extent, *unit, placeOf);
    } else if (kind.shape == core::PointerShape::Sized) {
      extent = termBytes(kind.extent, 1, placeOf);
    }
    if (!extent)
      return std::nullopt;
    return core::SpatialRecord{.extent = extent,
                               .offset = core::PointerOffset::zero(),
                               .location = at,
                               .declared = true,
                               .extentClass = extentClass};
  };
  std::optional<core::SpatialRecord> record;
  // §7.3: the static join, or a Nonnull §7.5 requirement the calls check.
  bool nonnull = entry->kind.nullability == core::Nullability::Nonnull &&
                 entry->kind.source == core::KindSource::Inferred;
  if (entry->hasEnforcedRequirement() && !entry->hasDeclaredShape()) {
    for (const MustAccessRequirement &requirement : entry->mustAccess) {
      const bool unguarded =
          !requirement.guard || alwaysHolds(*requirement.guard);
      if (unguarded &&
          requirement.kind.nullability == core::Nullability::Nonnull)
        nonnull = true;
      if (!record && (unguarded || vacuousWhenFalse(requirement)))
        record = recordOf(requirement.kind, core::ExtentClass::LowerBound);
    }
  }
  // §7.3: `argv` of `main`, `counted(argc + 1) nonnull`.
  if (entry->mainArgv) {
    record = recordOf(entry->kind, core::ExtentClass::Declared);
    nonnull = true;
  }
  if (!record && entry->hasShape() && !entry->shapeFromSystemHeader())
    record =
        recordOf(entry->kind,
                 entry->extentClass.value_or(core::ExtentClass::LowerBound));
  if (record)
    state.spatial.set(place, std::move(*record));
  if (nonnull)
    state.nulls.set(place,
                    core::NullRecord{.state = core::Nullness::NonNull,
                                     .location = at,
                                     .reason = core::NullReason::Declared,
                                     .detail = {}});
}

std::optional<core::SpatialRecord>
FunctionDataflow::slotRecordAt(core::PlaceId place) {
  if (options.kinds == nullptr)
    return std::nullopt;
  const NamedDecl *decl = builder.declFor(place);
  const KindEntry *entry = nullptr;
  QualType type;
  std::optional<core::PlaceId> object;
  if (const auto *field = dyn_cast_if_present<FieldDecl>(decl)) {
    if (places.isBase(place) || places.step(place) != core::PathStep::Field)
      return std::nullopt;
    object = places.parent(place);
    entry = options.kinds->field(*field);
    type = field->getType();
  } else if (const auto *var = dyn_cast_if_present<VarDecl>(decl);
             var != nullptr && var->hasGlobalStorage() &&
             places.isBase(place)) {
    entry = options.kinds->variable(*var);
    type = var->getType();
  }
  if (entry == nullptr || !type->isPointerType() ||
      entry->shapeFromSystemHeader())
    return std::nullopt;
  const QualType pointee = type->getPointeeType();
  const auto *record = decl->getDeclContext() != nullptr
                           ? dyn_cast<RecordDecl>(decl->getDeclContext())
                           : nullptr;
  // A sibling field of the object (`.cap` for `b->data`).
  const auto placeOf =
      [&](const core::ExtentPath &path) -> std::optional<core::PlaceId> {
    if (path.root != core::ExtentPath::Root::Field || !object ||
        record == nullptr)
      return std::nullopt;
    for (const FieldDecl *sibling : record->fields())
      if (sibling->getName() == path.field)
        return builder.fieldPlace(*object, *sibling);
    return std::nullopt;
  };
  std::optional<core::Affine> extent;
  core::ExtentClass extentClass =
      entry->extentClass.value_or(core::ExtentClass::LowerBound);
  switch (entry->kind.shape) {
  case core::PointerShape::Single:
    if (const auto bytes = singleWidth(
            pointee, [this](QualType t) { return objectWidthOf(t); }))
      extent = core::Affine::ofConstant(*bytes);
    extentClass = core::ExtentClass::LowerBound;
    break;
  case core::PointerShape::Counted:
    if (const auto unit = elementBytes(pointee, context))
      extent = termBytes(entry->kind.extent, *unit, placeOf);
    break;
  case core::PointerShape::Sized:
    extent = termBytes(entry->kind.extent, 1, placeOf);
    break;
  case core::PointerShape::EndedBy:
  case core::PointerShape::NulTerminated:
  case core::PointerShape::Unknown:
    break;
  }
  if (!extent)
    return std::nullopt;
  return core::SpatialRecord{.extent = extent,
                             .offset = core::PointerOffset::zero(),
                             .location = locate(decl->getLocation()),
                             .declared = true,
                             .extentClass = extentClass};
}

std::optional<FunctionDataflow::KnownExtent>
FunctionDataflow::resultExtentOf(const Expr &base) {
  const auto *call = dyn_cast<CallExpr>(base.IgnoreParenCasts());
  const FunctionDecl *callee =
      call != nullptr ? call->getDirectCallee() : nullptr;
  if (options.kinds == nullptr || callee == nullptr ||
      !callee->getReturnType()->isPointerType())
    return std::nullopt;
  const KindEntry *entry = options.kinds->result(*callee);
  if (entry == nullptr || entry->shapeFromSystemHeader())
    return std::nullopt;
  // What the callee's summary says of the value (a copy of an argument, a
  // fresh allocation) is the engine's own fact, never the result kind: the
  // kind speaks only for a value nothing else describes.
  if (builder.classifyValue(*call).kind != ValueOrigin::Kind::Opaque)
    return std::nullopt;
  const QualType pointee = callee->getReturnType()->getPointeeType();
  std::optional<core::Affine> extent;
  core::ExtentClass extentClass =
      entry->extentClass.value_or(core::ExtentClass::LowerBound);
  if (entry->kind.shape == core::PointerShape::Single) {
    if (const auto bytes = singleWidth(
            pointee, [this](QualType t) { return objectWidthOf(t); }))
      extent = core::Affine::ofConstant(*bytes);
    extentClass = core::ExtentClass::LowerBound;
  } else if (entry->kind.shape == core::PointerShape::Sized &&
             !entry->kind.extent.isConstant() &&
             entry->kind.extent.path->root == core::ExtentPath::Root::Param &&
             entry->kind.extent.path->param < call->getNumArgs()) {
    // §7.2 `alloc_size(i[, j])`: `param i [* param j]` bytes, as passed.
    const core::ExtentTerm &term = entry->kind.extent;
    auto bytes = builder.affineOf(*call->getArg(term.path->param));
    if (bytes && entry->extentFactor) {
      const auto factor =
          *entry->extentFactor < call->getNumArgs()
              ? builder.affineOf(*call->getArg(*entry->extentFactor))
              : std::nullopt;
      bytes = factor && factor->isConstant() ? bytes->times(factor->constant)
                                             : std::nullopt;
    }
    if (bytes && term.scale != 1)
      bytes = bytes->times(term.scale);
    if (bytes && term.offset != 0)
      bytes = bytes->shifted(term.offset);
    extent = bytes;
  }
  if (!extent)
    return std::nullopt;
  return KnownExtent{.have = *extent,
                     .origin = locate(*call),
                     .pointer = std::nullopt,
                     .offset = core::PointerOffset::zero(),
                     .unit = std::nullopt,
                     .declared = true,
                     .extentClass = extentClass,
                     .base = &base};
}

void FunctionDataflow::seedCallResult(core::PlaceId dest, const Expr &value,
                                      core::AnalysisState &state) {
  if (options.kinds == nullptr || state.spatial.has(dest))
    return;
  if (const auto known = resultExtentOf(value))
    state.spatial.set(dest,
                      core::SpatialRecord{.extent = known->have,
                                          .offset = known->offset,
                                          .location = known->origin,
                                          .declared = true,
                                          .extentClass = known->extentClass});
}

std::optional<core::Nullness>
FunctionDataflow::kindNullness(const NamedDecl &decl) const {
  if (options.kinds == nullptr)
    return std::nullopt;
  const KindEntry *entry = nullptr;
  if (const auto *param = dyn_cast<ParmVarDecl>(&decl)) {
    const auto *owner = dyn_cast<FunctionDecl>(param->getDeclContext());
    if (owner != nullptr &&
        owner->getCanonicalDecl() == function.getCanonicalDecl())
      entry = options.kinds->param(function, param->getFunctionScopeIndex());
  } else if (const auto *field = dyn_cast<FieldDecl>(&decl)) {
    entry = options.kinds->field(*field);
  } else if (const auto *var = dyn_cast<VarDecl>(&decl);
             var != nullptr && var->hasGlobalStorage()) {
    entry = options.kinds->variable(*var);
  }
  // Declared nullability only (§7.2): what callers and stores must meet.
  if (entry == nullptr || !entry->nullabilityLevel ||
      entry->nullabilityFromSystemHeader())
    return std::nullopt;
  return entry->kind.nullability == core::Nullability::Nonnull
             ? core::Nullness::NonNull
             : core::Nullness::MaybeNull;
}

bool FunctionDataflow::isArgvElement(const Expr &pointer) const {
  const auto *element =
      dyn_cast<ArraySubscriptExpr>(pointer.IgnoreParenImpCasts());
  const auto *ref =
      element != nullptr
          ? dyn_cast<DeclRefExpr>(element->getBase()->IgnoreParenImpCasts())
          : nullptr;
  const auto *param =
      ref != nullptr ? dyn_cast<ParmVarDecl>(ref->getDecl()) : nullptr;
  if (options.kinds == nullptr || param == nullptr ||
      param->getDeclContext() != &function)
    return false;
  const KindEntry *entry =
      options.kinds->param(function, param->getFunctionScopeIndex());
  return entry != nullptr && entry->mainArgv;
}

core::FacetDecision
FunctionDataflow::coveredDecision(const SiteInfo &site, core::Facet facet,
                                  core::FacetDecision decision,
                                  unsigned argument) {
  if (options.kinds == nullptr ||
      (facet != core::Facet::Spatial && facet != core::Facet::Null) ||
      decision.outcome == core::SiteOutcome::Violation ||
      decision.outcome == core::SiteOutcome::Proven ||
      decision.outcome == core::SiteOutcome::Trusted)
    return decision;
  // §7.3: an element of `main`'s argv is nul-terminated, so it has the one
  // byte `*argv[i]` reads (the system's contract).
  if (facet == core::Facet::Spatial && argument == ~0U &&
      site.kind == core::SiteKind::Deref && site.operand != nullptr &&
      isArgvElement(*site.operand))
    return core::FacetDecision::trustedFor(core::TrustReason::SystemApi,
                                           "an element of 'argv' is a "
                                           "nul-terminated string");
  const auto *call = dyn_cast<CallExpr>(site.stmt);
  // A call's own decision is about all of it; R4 covers one argument's
  // requirement record.
  if (call != nullptr && argument == ~0U)
    return decision;
  for (const CoveringRequirement &covered :
       options.kinds->covering(*site.stmt)) {
    if (covered.function != function.getCanonicalDecl())
      continue;
    const KindEntry *entry = options.kinds->param(function, covered.param);
    if (entry == nullptr || covered.requirement >= entry->mustAccess.size() ||
        !entry->enforcement)
      continue;
    // A requirement record of a call (R4) is about the parameter's own
    // argument.
    if (argument != ~0U) {
      const auto *ref = call != nullptr && argument < call->getNumArgs()
                            ? dyn_cast<DeclRefExpr>(
                                  call->getArg(argument)->IgnoreParenImpCasts())
                            : nullptr;
      if (ref == nullptr ||
          ref->getDecl() != function.getParamDecl(covered.param))
        continue;
    }
    const MustAccessRequirement &requirement =
        entry->mustAccess[covered.requirement];
    // §7.2: a declared kind rules its parameter: it holds inside under A1
    // and every call checks it, while the inferred requirements are not
    // checked at the calls. What a requirement equal to it covers is
    // proven; a loop to `m` over `counted(n)` stays the body's check.
    if (entry->hasDeclaredShape()) {
      if (facet == core::Facet::Spatial && !entry->shapeFromSystemHeader() &&
          entry->kind.sameShape(requirement.kind))
        return core::FacetDecision::proven();
      continue;
    }
    if (*entry->enforcement == RequirementEnforcement::CallSites) {
      if (facet == core::Facet::Spatial || nullEnforced(*entry, requirement))
        return core::FacetDecision::proven();
      continue;
    }
    // An exported function: its callers' contract, for extents beyond
    // Single only; the null facet is never trusted (§7.5).
    if (facet == core::Facet::Spatial &&
        decision.outcome == core::SiteOutcome::Unresolved &&
        requirement.kind.shape != core::PointerShape::Single)
      return core::FacetDecision::trustedFor(
          core::TrustReason::CallerContract,
          "'" + function.getNameAsString() + "' requires " +
              requirement.toString() + " of '" +
              function.getParamDecl(covered.param)->getNameAsString() +
              "' from its callers");
  }
  return decision;
}

void FunctionDataflow::decideCallKinds(const CallExpr &call,
                                       const core::AnalysisState &state) {
  const SiteInfo *site = accessSite(call, core::Facet::Spatial);
  const FunctionDecl *callee = call.getDirectCallee();
  if (options.kinds == nullptr || site == nullptr || callee == nullptr ||
      site->kind != core::SiteKind::Call)
    return;
  std::vector<ArgumentRequirement> requirements;
  const auto affineAt =
      [&](const core::ExtentTerm &term) -> std::optional<core::Affine> {
    if (term.isConstant())
      return core::Affine::ofConstant(term.offset);
    if (term.path->root != core::ExtentPath::Root::Param ||
        term.path->param >= call.getNumArgs())
      return std::nullopt;
    auto value = builder.affineOf(*call.getArg(term.path->param));
    if (value)
      value = value->times(term.scale);
    return value ? value->shifted(term.offset) : std::nullopt;
  };
  // The bytes from where argument `from` points to where `to` points, when
  // both point into one object at known places.
  const auto between = [&](unsigned from,
                           unsigned to) -> std::optional<std::int64_t> {
    if (from >= call.getNumArgs() || to >= call.getNumArgs())
      return std::nullopt;
    const auto first = argumentAccessOf(*call.getArg(from));
    const auto second = argumentAccessOf(*call.getArg(to));
    if (!first || !second || !first->start.isConstant() ||
        !second->start.isConstant())
      return std::nullopt;
    // The storage of an array the argument decays from, else the place of
    // the pointer it is computed from.
    const auto storageOf = [](const Access &access) -> const VarDecl * {
      if (access.storage != nullptr)
        return access.storage;
      const auto *ref =
          access.base != nullptr ? dyn_cast<DeclRefExpr>(access.base) : nullptr;
      const auto *var =
          ref != nullptr ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
      return var != nullptr && var->getType()->isArrayType() ? var : nullptr;
    };
    const VarDecl *firstStorage = storageOf(*first);
    const bool sameStorage =
        firstStorage != nullptr && firstStorage == storageOf(*second);
    const auto firstRef = first->base != nullptr && firstStorage == nullptr
                              ? builder.resolvePointerValue(*first->base)
                              : std::nullopt;
    const auto secondRef = second->base != nullptr && !sameStorage
                               ? builder.resolvePointerValue(*second->base)
                               : std::nullopt;
    if (!sameStorage &&
        !(firstRef && secondRef && firstRef->place == secondRef->place))
      return std::nullopt;
    return llvm::checkedSub(second->start.constant, first->start.constant);
  };
  // §7.5: whether the guard holds here (true, false, or not known).
  const auto holds = [&](const RequirementGuard &guard) -> std::optional<bool> {
    if (alwaysHolds(guard))
      return true;
    // R3's `p < q` over two pointer parameters.
    if (!guard.lhs.isConstant() && !guard.rhs.isConstant() &&
        guard.lhs.path->root == core::ExtentPath::Root::Param &&
        guard.rhs.path->root == core::ExtentPath::Root::Param &&
        guard.lhs.path->param < callee->getNumParams() &&
        callee->getParamDecl(guard.lhs.path->param)
            ->getType()
            ->isPointerType()) {
      const auto span = between(guard.lhs.path->param, guard.rhs.path->param);
      if (!span)
        return std::nullopt;
      return guard.relation == RequirementGuard::Relation::Less ? *span > 0
                                                                : *span >= 0;
    }
    const auto lhs = affineAt(guard.lhs);
    const auto rhs = affineAt(guard.rhs);
    const auto bound = lhs && guard.relation == RequirementGuard::Relation::Less
                           ? lhs->shifted(1)
                           : lhs;
    if (!rhs || !bound)
      return std::nullopt;
    return decideAtLeast(foldAffine(*rhs, state), foldAffine(*bound, state),
                         state);
  };
  // `(q + k) - p` bytes for an `ended-by(q + k)` requirement on `p`.
  const auto endedBytes =
      [&](unsigned argument, const core::ExtentTerm &end,
          std::int64_t unit) -> std::optional<core::Affine> {
    if (end.isConstant() || end.path->root != core::ExtentPath::Root::Param)
      return std::nullopt;
    const auto span = between(argument, end.path->param);
    const auto extra = llvm::checkedMul(end.offset, unit);
    const auto bytes =
        span && extra ? llvm::checkedAdd(*span, *extra) : std::nullopt;
    return bytes ? std::optional(core::Affine::ofConstant(*bytes))
                 : std::nullopt;
  };
  for (unsigned i = 0; i < call.getNumArgs() && i < callee->getNumParams();
       ++i) {
    const KindEntry *param = options.kinds->param(*callee, i);
    if (param == nullptr)
      continue;
    const QualType pointee =
        callee->getParamDecl(i)->getType()->getPointeeType();
    const auto unit = elementBytes(pointee, context);
    // §7.3: a value that may be a cursor, where the callee relies on Single:
    // proven when it has an element here, else `unresolved(unknown-extent)`.
    if (llvm::is_contained(site->reliance, i)) {
      if (const auto bytes = singleWidth(
              pointee, [this](QualType t) { return objectWidthOf(t); })) {
        ArgumentRequirement row{.argument = i};
        row.need = core::Affine::ofConstant(*bytes);
        row.rowOnly = true;
        requirements.push_back(std::move(row));
      }
    }
    // §7.2: a declared `ended-by(q)`: `[p, q)` in one object, decided when
    // both point into one object at known places.
    if (param->hasDeclaredShape() && !param->shapeFromSystemHeader() &&
        param->kind.shape == core::PointerShape::EndedBy) {
      ArgumentRequirement out{.argument = i};
      out.enforced = true;
      if (unit)
        if (const auto bytes = endedBytes(i, param->kind.extent, *unit)) {
          out.need = *bytes;
          if (bytes->isConstant() && bytes->constant >= 0)
            out.needTerm = WitnessTerm::ofConstant(bytes->constant);
        }
      requirements.push_back(std::move(out));
      continue;
    }
    // §7.5: a static callee's requirements, and an exported one's beyond
    // Single (its nullability stays the body's).
    const bool enforced =
        param->hasEnforcedRequirement() && !param->hasDeclaredShape();
    const bool contract =
        param->enforcement == RequirementEnforcement::CallerContract &&
        !param->hasDeclaredShape();
    if (!enforced && !contract)
      continue;
    for (const MustAccessRequirement &requirement : param->mustAccess) {
      if (contract && requirement.kind.shape == core::PointerShape::Single)
        continue;
      ArgumentRequirement out{.argument = i};
      out.enforced = true;
      const std::optional<bool> guarded =
          requirement.guard ? holds(*requirement.guard) : std::optional(true);
      if (guarded == false) {
        // The loop runs zero times here: nothing is needed.
        out.need = core::Affine::ofConstant(0);
        requirements.push_back(std::move(out));
        continue;
      }
      if (!guarded) {
        out.guard = guardTerm(*requirement.guard, call);
        // A guard with no C spelling leaves the requirement unchecked.
        if (!out.guard) {
          requirements.push_back(std::move(out));
          continue;
        }
      }
      const core::PointerKind &kind = requirement.kind;
      switch (kind.shape) {
      case core::PointerShape::Single:
        if (const auto bytes = singleWidth(
                pointee, [this](QualType t) { return objectWidthOf(t); })) {
          out.need = core::Affine::ofConstant(*bytes);
          out.needTerm = WitnessTerm::ofConstant(*bytes);
        }
        break;
      case core::PointerShape::Counted:
      case core::PointerShape::Sized: {
        const std::int64_t size =
            kind.shape == core::PointerShape::Sized ? 1 : unit.value_or(0);
        if (size <= 0)
          continue;
        if (const auto count = affineAt(kind.extent))
          out.need = count->times(size);
        if (auto term = argumentTerm(kind.extent, call))
          out.needTerm = size == 1
                             ? std::move(*term)
                             : WitnessTerm::mul(std::move(*term),
                                                WitnessTerm::sizeOf(pointee));
        break;
      }
      case core::PointerShape::EndedBy:
        if (unit)
          if (const auto bytes = endedBytes(i, kind.extent, *unit)) {
            out.need = *bytes;
            if (bytes->isConstant() && bytes->constant >= 0)
              out.needTerm = WitnessTerm::ofConstant(bytes->constant);
          }
        break;
      case core::PointerShape::NulTerminated:
        out.kind = ArgumentRequirement::Kind::String;
        break;
      case core::PointerShape::Unknown:
        continue;
      }
      requirements.push_back(std::move(out));
    }
    if (!enforced)
      continue;
    // The null part (`SiteCollector` wrapped the argument when the facts
    // leave it open): proven, checked, or the call's violation.
    const ArgumentNeed *need = nullptr;
    for (const ArgumentNeed &each : site->arguments)
      if (each.argument == i && each.inferred)
        need = &each;
    if (need == nullptr)
      continue;
    const SiteInfo *nullSite = accessSite(call, core::Facet::Null);
    const auto publishNull = [&](const core::FacetDecision &decision) {
      if (nullSite == nullptr)
        return;
      core::Requirement record;
      record.argument = i;
      record.decision = decision;
      ledger.requirement(*nullSite->stmt, core::Facet::Null, std::move(record));
    };
    std::optional<bool> guardHolds = true;
    for (const MustAccessRequirement &requirement : param->mustAccess)
      if (requirement.kind.nullability == core::Nullability::Nonnull &&
          nullEnforced(*param, requirement)) {
        guardHolds =
            requirement.guard ? holds(*requirement.guard) : std::optional(true);
        break;
      }
    if (guardHolds == false) {
      publishNull(core::FacetDecision::proven());
      continue;
    }
    const Expr &arg = *call.getArg(i);
    const ValueOrigin origin = builder.classifyValue(arg);
    std::optional<core::PlaceId> place;
    if (origin.kind == ValueOrigin::Kind::Copy && origin.place)
      place = origin.place->place;
    const bool derived = place && !origin.offset.isZero();
    const auto record =
        derived ? nullnessAt(*place, state) : nullnessOf(origin, arg, state);
    if (record && !record->mayBeNull()) {
      publishNull(core::FacetDecision::proven());
      continue;
    }
    if (!record || record->state != core::Nullness::Null ||
        record->allocatorSource || derived || guardHolds != true) {
      publishNull(core::FacetDecision::checked());
      continue;
    }
    // §3.2, §7.5: null on every path, into a must-access.
    publishNull(core::FacetDecision::violation());
    std::string message = place ? "'" + nameOf(*place) + "', which is null, "
                                : std::string("a null pointer ");
    message += "is passed to " + calleeName(call) + ", which dereferences it";
    core::Diagnostic diagnostic =
        makeError(core::diag::NullDereference, message, arg);
    if (place && record->location.isValid())
      diagnostic.addNote(nullNote(*record, nameOf(*place)), record->location);
    diagnostic.addNote(calleeName(call) + " is declared here",
                       locate(callee->getLocation()));
    report(std::move(diagnostic), core::Certainty::Definite, nullSite,
           core::Facet::Null);
  }
  if (!requirements.empty())
    decideArgumentRequirements(call, *site, requirements, state);
}

void FunctionDataflow::snapshotExtentsBelow(core::PlaceId place,
                                            core::AnalysisState &state) {
  llvm::SmallVector<core::PlaceId, 2> counts;
  for (const auto &[holder, record] : state.spatial.all()) {
    const std::optional<core::PlaceId> count =
        record.extent && record.extent->place    ? record.extent->place
        : record.string && record.string->length ? record.string->length->place
                                                 : std::nullopt;
    if (count && places.isDescendantOf(*count, place) &&
        !llvm::is_contained(counts, *count))
      counts.push_back(*count);
  }
  for (const core::PlaceId count : counts)
    snapshotScalar(count, nullptr, state);
}

void FunctionDataflow::decideSlotStores(const Stmt &stmt,
                                        const core::AnalysisState &state) {
  if (options.kinds == nullptr || !publishing())
    return;
  // §7.4 rule 7: a store into a slot with a declared kind meets it once the
  // stores of its group are done (a pointer and its count); a store in no
  // group, at once.
  std::vector<const Stmt *> stores;
  const auto groups = options.inferred != nullptr
                          ? options.inferred->groupsOf(stmt)
                          : std::vector<const StoreGroup *>{};
  for (const StoreGroup *group : groups)
    if (group->last() == &stmt)
      stores.insert(stores.end(), group->stores.begin(), group->stores.end());
  if (groups.empty())
    stores.push_back(&stmt);
  const SiteIndex &sites = ledger.siteIndex();
  for (const Stmt *store : stores) {
    const auto *assign = dyn_cast<BinaryOperator>(store);
    for (const core::SiteId id : sites.sitesOf(*store)) {
      const SiteInfo *site = sites.info(id);
      if (site == nullptr || site->kind != core::SiteKind::Cast ||
          !site->required || assign == nullptr || !assign->isAssignmentOp())
        continue;
      const core::PointerKind &kind = site->required->kind;
      const auto slot = builder.resolvePointerValue(*assign->getLHS());
      const auto record = slot && slot->element.isWhole()
                              ? state.spatial.recordOf(slot->place)
                              : std::nullopt;
      const QualType pointee = assign->getLHS()->getType()->getPointeeType();
      // What the kind needs, in bytes, over the object's other fields.
      std::optional<core::Affine> need;
      if (kind.shape == core::PointerShape::Single) {
        if (const auto bytes = singleWidth(
                pointee, [this](QualType t) { return objectWidthOf(t); }))
          need = core::Affine::ofConstant(*bytes);
      } else if ((kind.shape == core::PointerShape::Counted ||
                  kind.shape == core::PointerShape::Sized) &&
                 slot && !places.isBase(slot->place)) {
        const auto object = places.parent(slot->place);
        const auto *field =
            dyn_cast_if_present<FieldDecl>(builder.declFor(slot->place));
        const std::int64_t unit =
            kind.shape == core::PointerShape::Sized
                ? 1
                : elementBytes(pointee, context).value_or(0);
        const auto placeOf =
            [&](const core::ExtentPath &path) -> std::optional<core::PlaceId> {
          if (path.root != core::ExtentPath::Root::Field || !object ||
              field == nullptr)
            return std::nullopt;
          for (const FieldDecl *sibling : field->getParent()->fields())
            if (sibling->getName() == path.field)
              return builder.fieldPlace(*object, *sibling);
          return std::nullopt;
        };
        if (unit > 0)
          need = termBytes(kind.extent, unit, placeOf);
      }
      core::FacetDecision decision = core::FacetDecision::unresolvedFor(
          core::UnresolvedReason::UnknownExtent);
      if (need && record && record->extent && record->offset.isZero()) {
        const auto enough = decideAtLeast(foldAffine(*record->extent, state),
                                          foldAffine(*need, state), state);
        const auto *field =
            dyn_cast_if_present<FieldDecl>(builder.declFor(slot->place));
        if (enough == true)
          decision = core::FacetDecision::proven();
        else if (enough == false && record->exact() && field != nullptr &&
                 !getAnnotations(*field).sizedBy.empty())
          // RFC 0012's `annotation-mismatch` reported the shortfall.
          decision = core::FacetDecision::violation();
        else if (record->exact())
          decision = core::FacetDecision::unresolvedFor(
              core::UnresolvedReason::Inexpressible,
              "the check after the stores has no placement");
      } else if (slot && state.resources.isNull(slot->place)) {
        // A null pointer meets any shape: nothing is accessed through it.
        decision = core::FacetDecision::proven();
      }
      decide(site, core::Facet::Spatial, decision);
    }
  }
}

} // namespace weavec::analysis
