//===- EngineKinds.cpp - Pointer kinds in the object engine ---------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §7.2–§7.5 over the object domain: what `KindInference` found
// seeds the entry objects' extents and nullness, strengthens the decisions
// of the accesses a requirement covers, and becomes requirement records at
// the calls that must meet it.
//
//===----------------------------------------------------------------------===//

#include "Engine.h"
#include "weavec/Analysis/ClangLocation.h"
#include "weavec/Analysis/KindTable.h"

#include "clang/AST/RecordLayout.h"
#include "clang/Basic/SourceManager.h"

using namespace clang;

namespace weavec::analysis::engine {

/// §7.5: the requirement's guard holds whatever the arguments (`0 < 8`).
static bool alwaysHolds(const RequirementGuard &guard) {
  if (!guard.lhs.isConstant() || !guard.rhs.isConstant())
    return false;
  return guard.relation == RequirementGuard::Relation::Less
             ? guard.lhs.offset < guard.rhs.offset
             : guard.lhs.offset <= guard.rhs.offset;
}

/// §7.5: a guarded count that is zero or less whenever its guard fails.
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
  std::int64_t last = guard.lhs.offset;
  if (guard.relation == RequirementGuard::Relation::LessEqual)
    last -= 1;
  return last + (kind.extent.offset - guard.rhs.offset) <= 0;
}

/// §7.5: whether the null part of `requirement` is what the calls check.
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

/// The bytes of one element of a `T *` (1 for `void`).
static std::optional<std::int64_t> elementBytes(const ASTContext &context,
                                                QualType pointee) {
  if (pointee.isNull())
    return std::nullopt;
  if (pointee->isVoidType())
    return 1;
  if (pointee->isIncompleteType() || pointee->isFunctionType() ||
      !pointee->isConstantSizeType())
    return std::nullopt;
  return static_cast<std::int64_t>(
      context.getTypeSizeInChars(pointee).getQuantity());
}

/// §7.1: the bytes a Single `T *` guarantees: the object width.
static std::optional<std::int64_t> singleWidth(const ASTContext &context,
                                               QualType pointee) {
  if (pointee.isNull())
    return std::nullopt;
  if (pointee->isVoidType())
    return 1;
  auto size = elementBytes(context, pointee);
  if (!size)
    return std::nullopt;
  if (const RecordDecl *record = pointee->getAsRecordDecl();
      record != nullptr && !record->isUnion() &&
      record->isCompleteDefinition()) {
    const FieldDecl *last = nullptr;
    for (const FieldDecl *field : record->fields())
      last = field;
    if (last != nullptr &&
        (last->getType()->isIncompleteArrayType() ||
         (context.getAsConstantArrayType(last->getType()) != nullptr &&
          context.getLangOpts().getStrictFlexArraysLevel() ==
              LangOptions::StrictFlexArraysLevelKind::Default))) {
      const ASTRecordLayout &layout = context.getASTRecordLayout(record);
      return static_cast<std::int64_t>(
          layout.getFieldOffset(last->getFieldIndex()) /
          context.getCharWidth());
    }
  }
  return size;
}

std::optional<core::Extent>
FunctionRun::paramExtent(unsigned index, const std::vector<core::Sym> &values,
                         bool &nonnull) const {
  const KindEntry *entry = unit.input.kinds.param(function, index);
  if (entry == nullptr)
    return std::nullopt;
  QualType pointee = function.getParamDecl(index)->getType()->getPointeeType();
  auto termBytes = [&](const core::ExtentTerm &term,
                       std::int64_t unit) -> std::optional<core::Term> {
    if (term.isConstant())
      return core::Term::of(term.offset * unit);
    if (term.path->root != core::ExtentPath::Root::Param ||
        term.path->param >= values.size() ||
        values[term.path->param] == core::ZeroSym)
      return std::nullopt;
    if (term.scale * unit <= 0)
      return std::nullopt;
    return core::Term::ofSym(values[term.path->param], term.scale * unit,
                             term.offset * unit);
  };
  auto extentOf = [&](const core::PointerKind &kind,
                      core::ExtentClass cls) -> std::optional<core::Extent> {
    std::optional<core::Term> bytes;
    switch (kind.shape) {
    case core::PointerShape::Single:
      if (auto width = singleWidth(context, pointee))
        bytes = core::Term::of(*width);
      cls = core::ExtentClass::LowerBound;
      break;
    case core::PointerShape::Counted:
      if (auto elementSize = elementBytes(context, pointee))
        bytes = termBytes(kind.extent, *elementSize);
      break;
    case core::PointerShape::Sized:
      bytes = termBytes(kind.extent, 1);
      break;
    default:
      break;
    }
    if (!bytes)
      return std::nullopt;
    return core::Extent{*bytes, cls};
  };
  std::optional<core::Extent> extent;
  nonnull = entry->kind.nullability == core::Nullability::Nonnull &&
            (entry->kind.source == core::KindSource::Inferred ||
             entry->declaresNonnull());
  if (entry->hasEnforcedRequirement() && !entry->hasDeclaredShape())
    for (const MustAccessRequirement &requirement : entry->mustAccess) {
      bool unguarded = !requirement.guard || alwaysHolds(*requirement.guard);
      if (unguarded &&
          requirement.kind.nullability == core::Nullability::Nonnull &&
          entry->enforcement == RequirementEnforcement::CallSites)
        nonnull = true;
      if (!extent && (unguarded || vacuousWhenFalse(requirement)))
        extent = extentOf(requirement.kind, core::ExtentClass::LowerBound);
    }
  if (entry->mainArgv) {
    extent = extentOf(entry->kind, core::ExtentClass::Declared);
    nonnull = true;
  }
  if (!extent && entry->hasShape() && !entry->shapeFromSystemHeader())
    extent =
        extentOf(entry->kind,
                 entry->extentClass.value_or(core::ExtentClass::LowerBound));
  // A1's Single default, unless the kind is Unknown: a static callee some
  // caller passes a cursor to relies on nothing (§7.3).
  if (!extent && (entry->hasShape() || entry->shapeFromSystemHeader()))
    if (auto width = singleWidth(context, pointee))
      extent =
          core::Extent{core::Term::of(*width), core::ExtentClass::LowerBound};
  return extent;
}

bool Transfer::isArgvElement(const Expr &pointer) const {
  const auto *element =
      dyn_cast<ArraySubscriptExpr>(pointer.IgnoreParenImpCasts());
  const auto *ref =
      element != nullptr
          ? dyn_cast<DeclRefExpr>(element->getBase()->IgnoreParenImpCasts())
          : nullptr;
  const auto *param =
      ref != nullptr ? dyn_cast<ParmVarDecl>(ref->getDecl()) : nullptr;
  if (param == nullptr || param->getDeclContext() != &run.decl())
    return false;
  const KindEntry *entry = run.unitRun().input.kinds.param(
      run.decl(), param->getFunctionScopeIndex());
  return entry != nullptr && entry->mainArgv;
}

core::FacetDecision Transfer::covered(const SiteInfo &site, core::Facet facet,
                                      core::FacetDecision decision) const {
  if ((facet != core::Facet::Spatial && facet != core::Facet::Null) ||
      decision.outcome == core::SiteOutcome::Violation ||
      decision.outcome == core::SiteOutcome::Proven ||
      decision.outcome == core::SiteOutcome::Trusted)
    return decision;
  const FunctionDecl &function = run.decl();
  const KindTable &kinds = run.unitRun().input.kinds;
  // §7.3: an element of `main`'s argv is nul-terminated.
  if (facet == core::Facet::Spatial && site.kind == core::SiteKind::Deref &&
      site.operand != nullptr && isArgvElement(*site.operand))
    return core::FacetDecision::trustedFor(core::TrustReason::SystemApi,
                                           "an element of 'argv' is a "
                                           "nul-terminated string");
  if (isa<CallExpr>(site.stmt))
    return decision;
  for (const CoveringRequirement &cover : kinds.covering(*site.stmt)) {
    if (cover.function != function.getCanonicalDecl())
      continue;
    const KindEntry *entry = kinds.param(function, cover.param);
    if (entry == nullptr || cover.requirement >= entry->mustAccess.size() ||
        !entry->enforcement)
      continue;
    const MustAccessRequirement &requirement =
        entry->mustAccess[cover.requirement];
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
    if (facet == core::Facet::Spatial &&
        decision.outcome == core::SiteOutcome::Unresolved &&
        requirement.kind.shape != core::PointerShape::Single)
      return core::FacetDecision::trustedFor(
          core::TrustReason::CallerContract,
          "'" + function.getNameAsString() + "' requires " +
              requirement.toString() + " of '" +
              function.getParamDecl(cover.param)->getNameAsString() +
              "' from its callers");
  }
  return decision;
}

std::optional<core::FacetDecision>
Transfer::coveredArgument(const CallExpr &call, core::Sym pointer,
                          bool string) const {
  // The argument is a parameter's own value: its object, at its start.
  const core::SymInfo &value = heap.info(state, pointer);
  if (value.type != core::SymInfo::Type::Pointer || value.top ||
      value.targets.size() != 1 ||
      !(value.targets[0].offset == core::Term::of(0)))
    return std::nullopt;
  const core::ObjectInfo &info = run.table().info(value.targets[0].object);
  if (info.key.kind != core::ObjectKind::Entry || !info.key.path.isParam() ||
      info.key.path.steps.size() != 1 ||
      info.key.path.steps.front().step != core::PathStep::Deref)
    return std::nullopt;
  const FunctionDecl &function = run.decl();
  const KindTable &kinds = run.unitRun().input.kinds;
  for (const CoveringRequirement &cover : kinds.covering(call)) {
    if (cover.function != function.getCanonicalDecl() ||
        cover.param != info.key.path.index)
      continue;
    const KindEntry *entry = kinds.param(function, cover.param);
    if (entry == nullptr || cover.requirement >= entry->mustAccess.size() ||
        !entry->enforcement || entry->hasDeclaredShape())
      continue;
    const MustAccessRequirement &requirement =
        entry->mustAccess[cover.requirement];
    bool matches =
        string ? requirement.kind.shape == core::PointerShape::NulTerminated
               : requirement.kind.shape != core::PointerShape::NulTerminated &&
                     requirement.kind.shape != core::PointerShape::Unknown;
    if (!matches)
      continue;
    // §7.5: the static callers check it, or the callers are trusted with it.
    if (*entry->enforcement == RequirementEnforcement::CallSites)
      return core::FacetDecision::proven();
    return core::FacetDecision::trustedFor(
        core::TrustReason::CallerContract,
        "'" + function.getNameAsString() + "' requires " +
            requirement.toString() + " of '" +
            function.getParamDecl(cover.param)->getNameAsString() +
            "' from its callers");
  }
  return std::nullopt;
}

void Transfer::decideCallKinds(const CallExpr &call, const SiteInfo &site,
                               const std::vector<core::Sym> &args) {
  const FunctionDecl *callee = call.getDirectCallee();
  if (callee == nullptr || site.kind != core::SiteKind::Call ||
      !run.isPublishing())
    return;
  const KindTable &kinds = run.unitRun().input.kinds;
  std::vector<ArgRequirement> requirements;
  // An extent term over the callee's parameters, as this call passes them.
  auto termAt = [&](const core::ExtentTerm &term) -> core::Term {
    if (term.isConstant())
      return core::Term::of(term.offset);
    if (term.path->root != core::ExtentPath::Root::Param ||
        term.path->param >= args.size() ||
        args[term.path->param] == core::ZeroSym)
      return core::Term::unknown();
    core::Term value = termOf(args[term.path->param]);
    if (!value.known)
      return value;
    value.scale *= term.scale;
    value.constant = value.constant * term.scale + term.offset;
    if (value.isConstant())
      value.scale = 0;
    return value;
  };
  // The bytes from where argument `from` points to where `to` points.
  auto between = [&](unsigned from,
                     unsigned to) -> std::optional<std::int64_t> {
    if (from >= args.size() || to >= args.size())
      return std::nullopt;
    const core::SymInfo &a = heap.info(state, args[from]);
    const core::SymInfo &b = heap.info(state, args[to]);
    if (a.targets.size() != 1 || b.targets.size() != 1 ||
        a.targets[0].object != b.targets[0].object)
      return std::nullopt;
    auto diff = b.targets[0].offset.plus(
        core::Term{.var = a.targets[0].offset.var,
                   .scale = -a.targets[0].offset.scale,
                   .constant = -a.targets[0].offset.constant,
                   .known = a.targets[0].offset.known});
    if (!diff || !diff->isConstant())
      return std::nullopt;
    return diff->constant;
  };
  auto holds = [&](const RequirementGuard &guard) -> std::optional<bool> {
    if (alwaysHolds(guard))
      return true;
    if (!guard.lhs.isConstant() && !guard.rhs.isConstant() &&
        guard.lhs.path->root == core::ExtentPath::Root::Param &&
        guard.rhs.path->root == core::ExtentPath::Root::Param &&
        guard.lhs.path->param < callee->getNumParams() &&
        callee->getParamDecl(guard.lhs.path->param)
            ->getType()
            ->isPointerType()) {
      auto span = between(guard.lhs.path->param, guard.rhs.path->param);
      if (!span)
        return std::nullopt;
      return guard.relation == RequirementGuard::Relation::Less ? *span > 0
                                                                : *span >= 0;
    }
    core::Term lhs = termAt(guard.lhs);
    core::Term rhs = termAt(guard.rhs);
    if (!lhs.known || !rhs.known)
      return std::nullopt;
    if (guard.relation == RequirementGuard::Relation::Less)
      lhs = lhs.plusConstant(1);
    return heap.lessEqual(state, lhs, rhs);
  };
  for (unsigned i = 0;
       i < call.getNumArgs() && i < callee->getNumParams() && i < args.size();
       ++i) {
    const KindEntry *param = kinds.param(*callee, i);
    if (param == nullptr)
      continue;
    QualType pointee = callee->getParamDecl(i)->getType()->getPointeeType();
    auto unit = elementBytes(context, pointee);
    // §7.3: a callee that relies on Single, passed a possible cursor.
    if (std::find(site.reliance.begin(), site.reliance.end(), i) !=
        site.reliance.end())
      if (auto bytes = singleWidth(context, pointee)) {
        ArgRequirement row;
        row.argument = i;
        row.need = core::Term::of(*bytes);
        row.rowOnly = true;
        requirements.push_back(std::move(row));
      }
    // §7.2: a declared `ended-by(q)`.
    if (param->hasDeclaredShape() && !param->shapeFromSystemHeader() &&
        param->kind.shape == core::PointerShape::EndedBy) {
      ArgRequirement out;
      out.argument = i;
      out.enforced = true;
      if (unit && !param->kind.extent.isConstant() &&
          param->kind.extent.path->root == core::ExtentPath::Root::Param)
        if (auto span = between(i, param->kind.extent.path->param)) {
          std::int64_t bytes = *span + param->kind.extent.offset * *unit;
          out.need = core::Term::of(bytes);
          if (bytes >= 0)
            out.needTerm = WitnessTerm::ofConstant(bytes);
        }
      requirements.push_back(std::move(out));
      continue;
    }
    bool enforced =
        param->hasEnforcedRequirement() && !param->hasDeclaredShape();
    bool contract =
        param->enforcement == RequirementEnforcement::CallerContract &&
        !param->hasDeclaredShape();
    if (!enforced && !contract)
      continue;
    for (const MustAccessRequirement &requirement : param->mustAccess) {
      if (contract && requirement.kind.shape == core::PointerShape::Single)
        continue;
      ArgRequirement out;
      out.argument = i;
      out.enforced = true;
      std::optional<bool> guarded =
          requirement.guard ? holds(*requirement.guard) : std::optional(true);
      if (guarded == false) {
        out.need = core::Term::of(0);
        requirements.push_back(std::move(out));
        continue;
      }
      if (!guarded) {
        out.guard = guardTerm(*requirement.guard, call);
        if (!out.guard) {
          requirements.push_back(std::move(out));
          continue;
        }
      }
      const core::PointerKind &kind = requirement.kind;
      switch (kind.shape) {
      case core::PointerShape::Single:
        if (auto bytes = singleWidth(context, pointee)) {
          out.need = core::Term::of(*bytes);
          out.needTerm = WitnessTerm::ofConstant(*bytes);
        }
        break;
      case core::PointerShape::Counted:
      case core::PointerShape::Sized: {
        std::int64_t size =
            kind.shape == core::PointerShape::Sized ? 1 : unit.value_or(0);
        if (size <= 0)
          continue;
        core::Term count = termAt(kind.extent);
        if (count.known)
          out.need = count.isConstant()
                         ? core::Term::of(count.constant * size)
                         : core::Term::ofSym(count.var, count.scale * size,
                                             count.constant * size);
        if (auto term = argumentTerm(kind.extent, call))
          out.needTerm = size == 1
                             ? std::move(*term)
                             : WitnessTerm::mul(std::move(*term),
                                                WitnessTerm::sizeOf(pointee));
        break;
      }
      case core::PointerShape::EndedBy:
        if (unit && !kind.extent.isConstant() &&
            kind.extent.path->root == core::ExtentPath::Root::Param)
          if (auto span = between(i, kind.extent.path->param)) {
            std::int64_t bytes = *span + kind.extent.offset * *unit;
            out.need = core::Term::of(bytes);
            if (bytes >= 0)
              out.needTerm = WitnessTerm::ofConstant(bytes);
          }
        break;
      case core::PointerShape::NulTerminated:
        out.kind = ArgRequirement::Kind::String;
        out.argvElement = isArgvElement(*call.getArg(i));
        break;
      case core::PointerShape::Unknown:
        continue;
      }
      requirements.push_back(std::move(out));
    }
    if (!enforced)
      continue;
    // The null part.
    const ArgumentNeed *need = nullptr;
    for (const ArgumentNeed &each : site.arguments)
      if (each.argument == i && each.inferred)
        need = &each;
    if (need == nullptr)
      continue;
    auto publishNull = [&](const core::FacetDecision &decision) {
      if (!run.applies(site.id, core::Facet::Null))
        return;
      core::Requirement record;
      record.argument = i;
      record.decision = decision;
      run.ledger().requirement(call, core::Facet::Null, std::move(record));
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
    const core::SymInfo &value = heap.info(state, args[i]);
    if (value.null == core::PointerNull::NonNull) {
      publishNull(core::FacetDecision::proven());
      continue;
    }
    if (value.null != core::PointerNull::Null || value.allocatorSource ||
        guardHolds != true) {
      publishNull(core::FacetDecision::checked());
      continue;
    }
    publishNull(core::FacetDecision::violation());
    run.report(nullArgument(*call.getArg(i), value,
                            "'" + callee->getNameAsString() + "'", callee),
               core::Certainty::Definite, &call, core::Facet::Null);
  }
  // §7.2: declared shapes are checked at every call.
  for (const SiteInfo::DeclaredShape &shape : site.declaredShapes) {
    if (shape.argument >= call.getNumArgs())
      continue;
    ArgRequirement requirement;
    requirement.argument = shape.argument;
    requirement.enforced = true;
    const core::PointerKind &kind = shape.kind;
    if (kind.shape == core::PointerShape::NulTerminated) {
      requirement.kind = ArgRequirement::Kind::String;
      requirement.argvElement = isArgvElement(*call.getArg(shape.argument));
      requirements.push_back(std::move(requirement));
      continue;
    }
    core::Term count = core::Term::unknown();
    std::optional<WitnessTerm> countTerm;
    if (kind.extent.isConstant()) {
      count = core::Term::of(kind.extent.offset);
      countTerm = WitnessTerm::ofConstant(kind.extent.offset);
    } else if (kind.extent.path->root == core::ExtentPath::Root::Param &&
               kind.extent.path->param < call.getNumArgs()) {
      count = termAt(kind.extent);
      const Expr &arg = *call.getArg(kind.extent.path->param);
      countTerm = WitnessTerm::ofExpr(arg);
      if (kind.extent.scale != 1)
        countTerm = WitnessTerm::mul(
            std::move(*countTerm), WitnessTerm::ofConstant(kind.extent.scale));
      if (kind.extent.offset != 0)
        countTerm = WitnessTerm::add(
            std::move(*countTerm), WitnessTerm::ofConstant(kind.extent.offset));
    }
    auto element = elementBytes(context, shape.pointee);
    switch (kind.shape) {
    case core::PointerShape::Counted:
      if (!element)
        continue;
      if (count.known)
        requirement.need =
            count.isConstant()
                ? core::Term::of(count.constant * *element)
                : core::Term::ofSym(count.var, count.scale * *element,
                                    count.constant * *element);
      if (countTerm)
        requirement.needTerm = WitnessTerm::mul(
            std::move(*countTerm), WitnessTerm::sizeOf(shape.pointee));
      break;
    case core::PointerShape::Sized:
      requirement.need = count;
      requirement.needTerm = std::move(countTerm);
      break;
    case core::PointerShape::Single:
      if (auto width = singleWidth(context, shape.pointee)) {
        requirement.need = core::Term::of(*width);
        requirement.needTerm = WitnessTerm::ofConstant(*width);
      } else {
        continue;
      }
      break;
    default:
      continue;
    }
    requirements.push_back(std::move(requirement));
  }
  if (!requirements.empty())
    decideArguments(call, site, requirements, args, /*library=*/false);
}

} // namespace weavec::analysis::engine
