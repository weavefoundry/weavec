//===- DataflowWitnesses.cpp - Spatial decisions and witnesses ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §3.3, §7.1, §7.4 and §14: the spatial decision of an access
// against the extent the engine knows, and the witness its check needs. A
// witness names C places the engine knows still hold the values the extent
// was derived from: a write to a place an extent is expressed in drops the
// extent (`SpatialTracker::dropExtentsOn`), a reassigned pointer loses its
// definite aliases, and a quantity the program computed into a snapshot
// place has no C name, so its term is never built.
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"

using namespace clang;

namespace weavec::analysis {

/// §7.4: the trailing array member of `record` that `-fstrict-flex-arrays`
/// makes flexible: at the default level every trailing array, whatever its
/// declared bound (`UpVal *upvals[1]`).
static const FieldDecl *flexibleTrailingMember(const RecordDecl &record,
                                               const ASTContext &context) {
  if (record.isUnion() || !record.isCompleteDefinition())
    return nullptr;
  const FieldDecl *last = nullptr;
  for (const FieldDecl *field : record.fields())
    last = field;
  if (last == nullptr)
    return nullptr;
  const QualType type = last->getType();
  if (type->isIncompleteArrayType())
    return last;
  const auto *constant = context.getAsConstantArrayType(type);
  if (constant == nullptr)
    return nullptr;
  using Level = LangOptions::StrictFlexArraysLevelKind;
  switch (context.getLangOpts().getStrictFlexArraysLevel()) {
  case Level::Default:
    return last;
  case Level::OneZeroOrIncomplete:
    return constant->getSize().ule(1) ? last : nullptr;
  case Level::ZeroOrIncomplete:
    return constant->getSize().isZero() ? last : nullptr;
  case Level::IncompleteOnly:
    return nullptr;
  }
  return nullptr;
}

std::optional<std::int64_t>
FunctionDataflow::objectWidthOf(QualType type) const {
  const auto size = byteSizeOf(type, context);
  if (!size)
    return std::nullopt;
  if (const RecordDecl *record = type->getAsRecordDecl())
    if (const FieldDecl *member = flexibleTrailingMember(*record, context)) {
      const std::uint64_t bits = context.getFieldOffset(member);
      if (bits % context.getCharWidth() == 0)
        return static_cast<std::int64_t>(bits / context.getCharWidth());
    }
  return size;
}

std::optional<WitnessTerm>
FunctionDataflow::extentTerm(const core::Affine &have,
                             std::optional<core::PlaceId> &readsThrough) {
  if (have.isConstant())
    return have.constant >= 0
               ? std::optional(WitnessTerm::ofConstant(have.constant))
               : std::nullopt;
  if (have.scale <= 0)
    return std::nullopt;
  std::optional<WitnessTerm> term;
  if (const auto numeric = numericExpressions.find(*have.place);
      numeric != numericExpressions.end())
    term = expressionTerm(numeric->second, readsThrough);
  else
    term = placeTerm(*have.place, readsThrough);
  if (!term)
    return std::nullopt;
  if (have.scale != 1)
    term =
        WitnessTerm::mul(std::move(*term), WitnessTerm::ofConstant(have.scale));
  if (have.constant > 0)
    term = WitnessTerm::add(std::move(*term),
                            WitnessTerm::ofConstant(have.constant));
  else if (have.constant < 0)
    term = WitnessTerm::sub(std::move(*term),
                            WitnessTerm::ofConstant(-have.constant));
  return term;
}

std::optional<WitnessTerm>
FunctionDataflow::countTerm(const core::Affine &have, std::int64_t unit,
                            std::optional<core::PlaceId> &readsThrough) {
  if (unit <= 0)
    return std::nullopt;
  if (have.isConstant())
    return have.constant >= 0
               ? std::optional(WitnessTerm::ofConstant(have.constant / unit))
               : std::nullopt;
  // `n * sizeof *p` bytes, computed without wrapping (RFC 0017 proved the
  // product, or it is in 64 bits, where a wrapped value makes the term
  // helper's product saturate and the check fail closed): `n` elements.
  if (have.scale == 1 && have.constant == 0)
    if (const auto numeric = numericExpressions.find(*have.place);
        numeric != numericExpressions.end()) {
      const auto &root = numeric->second.all().back();
      const auto operands = numeric->second.operands();
      if (root.kind == core::IntegerNodeKind::Operation &&
          root.op == core::IntegerOp::Multiply && operands.size() == 2)
        for (std::size_t i = 0; i < 2; ++i) {
          const auto k = operands[1 - i].constantValue();
          const auto factor = k ? k->signedValue() : std::nullopt;
          if (!factor || *factor <= 0 || *factor % unit != 0)
            continue;
          const bool wraps =
              root.type.width < 64 &&
              (currentState == nullptr ||
               !operationDoesNotOverflow(root.op, operands[0], operands[1],
                                         root.type, *currentState));
          if (wraps)
            break;
          std::optional<core::PlaceId> through = readsThrough;
          if (auto count = expressionTerm(operands[i], through)) {
            readsThrough = through;
            return *factor == unit ? std::move(*count)
                                   : WitnessTerm::mul(std::move(*count),
                                                      WitnessTerm::ofConstant(
                                                          *factor / unit));
          }
        }
    }
  // Whole multiples of the element: `(scale / unit) * x + constant / unit`.
  if (have.scale > 0 && have.scale % unit == 0 && have.constant % unit == 0 &&
      !numericExpressions.contains(*have.place))
    return extentTerm(core::Affine{.place = have.place,
                                   .scale = have.scale / unit,
                                   .constant = have.constant / unit},
                      readsThrough);
  // §7.4 *Arithmetic*: otherwise the byte value the program passed,
  // rounded down to whole elements (`index(i, bytes / 4)`).
  auto bytes = extentTerm(have, readsThrough);
  if (!bytes)
    return std::nullopt;
  return unit == 1 ? std::move(*bytes)
                   : WitnessTerm::div(std::move(*bytes), unit);
}

std::optional<WitnessTerm>
FunctionDataflow::objectBaseTerm(const KnownExtent &known,
                                 std::optional<core::PlaceId> &readsThrough) {
  if (currentState == nullptr || !known.pointer)
    return std::nullopt;
  const core::AnalysisState &state = *currentState;
  // A cursor into a variable (`&m[0][0]`, `&s.f`, an array's element): the
  // variable's own address is its start (§7.4: the complete object).
  for (const core::Loan &loan : state.loans.heldBy(*known.pointer)) {
    if (!loan.allPaths || places.innermostDeref(loan.place))
      continue;
    const VarDecl *var = builder.varForPlace(places.root(loan.place));
    if (var == nullptr || isa<ParmVarDecl>(var) ||
        !var->getType()->isArrayType())
      continue;
    return WitnessTerm::ofPlace(*var);
  }
  // Another pointer at the start of the same object, on every path.
  for (const auto &[alias, edge] :
       state.definiteAliases.edgesFrom(*known.pointer)) {
    (void)edge;
    const auto record = state.spatial.recordOf(alias);
    if (!record || !record->extent || *record->extent != known.have ||
        !record->offset.isZero() ||
        !record->boundsOffset.value_or(core::PointerOffset::zero()).isZero() ||
        record->location != known.origin)
      continue;
    std::optional<core::PlaceId> through = readsThrough;
    if (auto term = placeTerm(alias, through)) {
      readsThrough = through;
      return term;
    }
  }
  return std::nullopt;
}

std::optional<CheckWitness>
FunctionDataflow::accessWitness(const SiteInfo &site,
                                const KnownExtent &known) {
  // The bytes one access covers: an Index site's element; a dereference
  // needs the object width of what its pointer points to (§7.4), which for
  // a struct with a flexible trailing array is less than `sizeof`.
  std::optional<std::int64_t> width;
  if (site.kind == core::SiteKind::Deref && site.operand != nullptr &&
      site.operand->getType()->isPointerType())
    width = objectWidthOf(site.operand->getType()->getPointeeType());
  else if (const auto *expr = dyn_cast<Expr>(site.stmt))
    width = byteSizeOf(expr->getType(), context);
  if (!width || *width <= 0)
    return std::nullopt;
  const std::int64_t unit = *width;
  std::optional<core::PlaceId> readsThrough;
  const auto safe = [&] {
    return !readsThrough || (currentState != nullptr &&
                             currentState->nulls.isNonNull(*readsThrough));
  };
  WitnessTerm index = site.index != nullptr ? WitnessTerm::ofExpr(*site.index)
                                            : WitnessTerm::ofConstant(0);
  // Whether the access indexes the pointer the extent belongs to itself
  // (`p[i]`, `*(p + i)`, `p->f`), rather than a member array of what it
  // points to (`p->items[i]`), whose index counts other elements.
  const bool direct =
      known.base != nullptr && site.operand != nullptr &&
      &PlaceBuilder::stripTransparent(*site.operand) == known.base;
  // `p->items[i]` with `p` at the start of its object: the member's
  // elements up to the end of the object, `index(i, (bytes - offset) /
  // unit)` (a flexible trailing array's count, §7.4).
  if (known.offset.isZero() && !direct && known.base != nullptr &&
      site.kind == core::SiteKind::Index && site.index != nullptr &&
      site.operand != nullptr)
    if (const auto *member = dyn_cast<MemberExpr>(
            &PlaceBuilder::stripTransparent(*site.operand));
        member != nullptr && member->isArrow() &&
        &PlaceBuilder::stripTransparent(*member->getBase()) == known.base &&
        member->getType()->isArrayType())
      if (const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
          field != nullptr && !field->isBitField() &&
          field->getParent()->isCompleteDefinition()) {
        const std::uint64_t bits = context.getFieldOffset(field);
        if (bits % context.getCharWidth() == 0) {
          const auto offset =
              static_cast<std::int64_t>(bits / context.getCharWidth());
          if (const auto rest = known.have.shifted(-offset))
            if (auto count = countTerm(*rest, unit, readsThrough))
              return CheckWitness{.shape = CheckWitness::Shape::Index,
                                  .extent = std::move(count),
                                  .extentClass = known.extentClass,
                                  .offset = std::move(index),
                                  .unmodified = true,
                                  .accessesSafe = safe()};
        }
      }
  // A pointer at the start of its object: `index(i, bytes / unit)`.
  if (known.offset.isZero() && direct) {
    auto count = countTerm(known.have, unit, readsThrough);
    if (!count)
      return std::nullopt;
    return CheckWitness{.shape = CheckWitness::Shape::Index,
                        .extent = std::move(count),
                        .extentClass = known.extentClass,
                        .offset = std::move(index),
                        .unmodified = true,
                        .accessesSafe = safe()};
  }
  // A cursor into its object, where the facts may or may not say: `span(p,
  // i, base, bytes, width)` measures it at run time (and lets `p[-1]`
  // pass).
  auto bytes = extentTerm(known.have, readsThrough);
  std::optional<WitnessTerm> base;
  if (bytes && known.offset.isZero() && known.pointer) {
    // The pointer itself is at the start (`p->items[i]` checks against
    // `p`).
    std::optional<core::PlaceId> through = readsThrough;
    base = placeTerm(*known.pointer, through);
    if (base)
      readsThrough = through;
  } else if (bytes) {
    base = objectBaseTerm(known, readsThrough);
  }
  if (!base)
    return std::nullopt;
  return CheckWitness{.shape = CheckWitness::Shape::Span,
                      .extent = std::move(bytes),
                      .extentClass = known.extentClass,
                      .base = std::move(base),
                      .width = WitnessTerm::ofConstant(unit),
                      .offset = std::move(index),
                      .unmodified = true,
                      .accessesSafe = safe()};
}

std::optional<bool>
FunctionDataflow::pointsToLiteral(const Expr &pointer,
                                  const core::AnalysisState &state) {
  const ValueOrigin origin = builder.classifyValue(pointer);
  if (!origin.place)
    return std::nullopt;
  if (origin.kind == ValueOrigin::Kind::Borrow)
    return builder.isLiteralPlace(places.root(origin.place->place))
               ? std::optional(true)
               : std::nullopt;
  if (origin.kind != ValueOrigin::Kind::Copy)
    return std::nullopt;
  for (const core::Loan &loan : state.loans.heldBy(origin.place->place))
    if (builder.isLiteralPlace(places.root(loan.place)))
      return loan.allPaths;
  return std::nullopt;
}

bool FunctionDataflow::checkLiteralWrite(const Expr &pointer, const Expr &at,
                                         const SiteInfo *site,
                                         const core::AnalysisState &state) {
  const auto literal = pointsToLiteral(pointer, state);
  if (!literal)
    return false;
  if (!*literal) {
    // Writable on the other paths, but not measurable as one object.
    decide(site, core::Facet::Spatial,
           core::FacetDecision::unresolvedFor(
               core::UnresolvedReason::UnknownExtent,
               "it may point into a string literal"));
    return true;
  }
  decide(site, core::Facet::Spatial, core::FacetDecision::violation());
  const Expr &stripped = PlaceBuilder::stripTransparent(pointer);
  std::string name;
  if (const auto ref = builder.resolvePointerValue(stripped);
      ref && !isa<StringLiteral>(stripped.IgnoreParenImpCasts()))
    name = nameOf(ref->place);
  else
    name = spellIndex(&stripped, core::Affine::ofConstant(0));
  report(makeError(core::diag::OutOfBounds,
                   "write through '" + name +
                       "', which points to a string literal",
                   at),
         core::Certainty::Definite, site, core::Facet::Spatial);
  return true;
}

void FunctionDataflow::decideUnplacedAccess(const Expr &access, Role role,
                                            core::AnalysisState &state) {
  if (!publishing())
    return;
  const Expr &e = PlaceBuilder::stripTransparent(access);
  if (role == Role::Read || role == Role::Write || role == Role::ReadWrite)
    checkBounds(e, state);
  decidePathBounds(e, role == Role::Consume, state);
  const SiteInfo *site = accessSite(e, core::Facet::Temporal);
  if (site == nullptr)
    site = accessSite(e, core::Facet::Null);
  if (site == nullptr || site->operand == nullptr)
    return;
  // The pointer the operand derives from (`&ts->contents` is a step from
  // `ts`).
  std::optional<PlaceRef> pointer = builder.resolvePointerValue(*site->operand);
  if (!pointer) {
    const ValueOrigin origin = builder.classifyValue(*site->operand);
    if (origin.kind == ValueOrigin::Kind::Copy && origin.place)
      pointer = origin.place;
  }
  if (!pointer || !pointer->element.isWhole()) {
    // Nothing names the object: its lifetime is not known here.
    decide(site, core::Facet::Temporal,
           core::FacetDecision::unresolvedFor(
               core::UnresolvedReason::Unanalysed,
               "the pointer is not a place the engine follows"));
    return;
  }
  // The object the pointer derives from: its record decides, as at a
  // dereference of the pointer itself (no report: the pointer's own uses
  // are reported where they are dereferenced).
  if (const auto hit = findMoved(pointer->place, state, pointer->element))
    decide(site, core::Facet::Temporal,
           temporalDecisionFor(hit->record, core::Certainty::Possible));
  else if (state.reinterpreted.contains(pointer->place))
    decide(site, core::Facet::Temporal,
           core::FacetDecision::unresolvedFor(core::UnresolvedReason::RawCast));
  else
    decide(site, core::Facet::Temporal, core::FacetDecision::proven());
  const auto record = nullnessAt(pointer->place, state);
  decide(site, core::Facet::Null,
         record && !record->mayBeNull() ? core::FacetDecision::proven()
                                        : core::FacetDecision::checked());
}

void FunctionDataflow::decideLoopBodySite(const Expr &expr,
                                          core::AnalysisState &state) {
  if (!publishing())
    return;
  // The pointer a site's facets are about: the access's operand, or the
  // released argument.
  const auto pointerOf = [&](const SiteInfo &site) -> std::optional<PlaceRef> {
    if (site.operand == nullptr)
      return std::nullopt;
    auto ref = builder.resolvePointerValue(
        PlaceBuilder::stripTransparent(*site.operand));
    if (ref && site.kind == core::SiteKind::Release)
      return builder.resolve(PlaceBuilder::stripTransparent(*site.operand));
    return ref;
  };
  const auto decideTemporalAndNull = [&](const SiteInfo &site,
                                         const PlaceRef &pointer) {
    // What the loop's model does is applied at its exit; here only what
    // holds before each iteration's operation, with no report.
    if (const auto hit = findMoved(pointer.place, state, pointer.element))
      decide(&site, core::Facet::Temporal,
             temporalDecisionFor(hit->record, core::Certainty::Possible));
    else
      decide(&site, core::Facet::Temporal, core::FacetDecision::proven());
    const auto record = nullnessAt(pointer.place, state);
    decide(&site, core::Facet::Null,
           record && !record->mayBeNull() ? core::FacetDecision::proven()
                                          : core::FacetDecision::checked());
  };
  const SiteIndex &sites = ledger.siteIndex();
  for (const core::SiteId id : sites.sitesOf(expr)) {
    const SiteInfo *site = sites.info(id);
    if (site == nullptr)
      continue;
    if (site->kind == core::SiteKind::Release) {
      // `free(a[i])`: the element's own allocation, when this function
      // holds it, is released from its start; the release of every element
      // once is the loop model's (`releaseArrayRange`).
      const auto element = pointerOf(*site);
      if (!element)
        continue;
      const auto resource = state.resources.recordOf(element->place);
      decide(site, core::Facet::Spatial,
             resource ? core::FacetDecision::proven()
                      : core::FacetDecision::unresolvedFor(
                            core::UnresolvedReason::UnknownIndex));
      decideTemporalAndNull(*site, *element);
      continue;
    }
    if (site->kind != core::SiteKind::Deref &&
        site->kind != core::SiteKind::Index)
      continue;
    if (ledger.applies(id, core::Facet::Spatial)) {
      const bool outer = boundsDecisionOnly;
      boundsDecisionOnly = true;
      checkBounds(expr, state);
      boundsDecisionOnly = outer;
    }
    if (const auto pointer = pointerOf(*site);
        pointer && pointer->element.isWhole())
      decideTemporalAndNull(*site, *pointer);
  }
}

void FunctionDataflow::decideSpatial(const SiteInfo *site,
                                     const core::SpatialCheck &check,
                                     const KnownExtent *known) {
  if (site == nullptr || !publishing())
    return;
  const auto unresolved = [&](core::UnresolvedReason reason,
                              std::string detail = {}) {
    decide(site, core::Facet::Spatial,
           core::FacetDecision::unresolvedFor(reason, std::move(detail)));
  };
  // A check against the extent the engine knows (§14 `witness`). §7.1: a
  // lower bound is never a check operand, and an access it does not cover
  // has no extent. Without a witness the planner falls back to what the
  // declarations give, or finds the check inexpressible.
  const auto checkAgainst = [&](const KnownExtent &extent) {
    if (!core::isCheckOperand(extent.extentClass)) {
      // What the declarations give still makes a check (§2.6).
      if (site->spatialCheckable())
        decide(site, core::Facet::Spatial, core::FacetDecision::checked());
      else
        unresolved(core::UnresolvedReason::UnknownExtent);
      return;
    }
    decide(site, core::Facet::Spatial, core::FacetDecision::checked());
    if (auto witness = accessWitness(*site, extent))
      ledger.witness(*site->stmt, core::Facet::Spatial, std::move(*witness));
  };
  switch (check.outcome) {
  case core::SpatialOutcome::Proven:
    decide(site, core::Facet::Spatial, core::FacetDecision::proven());
    return;
  case core::SpatialOutcome::Violation: {
    // §3.3: a violation against an exact extent is the caller's error; a
    // boundary value that may be past the end, or a declared extent the
    // object may exceed, is checked.
    const bool definiteKind =
        check.violation &&
        (check.violation->kind == core::BoundsVerdict::Kind::OutOfBounds ||
         check.violation->kind == core::BoundsVerdict::Kind::BeforeStart ||
         check.violation->kind == core::BoundsVerdict::Kind::AtLeastPastEnd);
    if (definiteKind && known != nullptr && known->exact()) {
      decide(site, core::Facet::Spatial, core::FacetDecision::violation());
      return;
    }
    if (known != nullptr) {
      checkAgainst(*known);
      return;
    }
    decide(site, core::Facet::Spatial, core::FacetDecision::checked());
    return;
  }
  case core::SpatialOutcome::Unresolved:
    break;
  }
  // The extent is known but not where in it the access lands: a check.
  if (check.reason == core::SpatialReason::UnknownIndex && known != nullptr) {
    checkAgainst(*known);
    return;
  }
  // What the declarations give still makes a check (§2.6).
  if (site->spatialCheckable()) {
    decide(site, core::Facet::Spatial, core::FacetDecision::checked());
    return;
  }
  switch (check.reason) {
  case core::SpatialReason::UnknownExtent:
  case core::SpatialReason::InterfaceRequirement:
    unresolved(core::UnresolvedReason::UnknownExtent);
    return;
  case core::SpatialReason::UnknownOffset:
    // §7.4 item 1: a cursor whose object's start and extent have names
    // here is checked with a span; otherwise its position is unknown.
    if (known != nullptr && core::isCheckOperand(known->extentClass))
      if (auto witness = accessWitness(*site, *known);
          witness && witness->shape == CheckWitness::Shape::Span) {
        decide(site, core::Facet::Spatial, core::FacetDecision::checked());
        ledger.witness(*site->stmt, core::Facet::Spatial, std::move(*witness));
        return;
      }
    unresolved(core::UnresolvedReason::UnknownIndex);
    return;
  case core::SpatialReason::UnknownIndex:
  case core::SpatialReason::Arithmetic:
  case core::SpatialReason::UnsupportedExpression:
  case core::SpatialReason::None:
    unresolved(core::UnresolvedReason::Unanalysed,
               std::string(core::toString(check.reason)));
    return;
  }
}

} // namespace weavec::analysis
