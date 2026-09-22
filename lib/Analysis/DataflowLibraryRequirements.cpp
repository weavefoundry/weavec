//===- DataflowLibraryRequirements.cpp - Library call requirements --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §2.5, §3.3, §8 and §15 item 4: the spatial facet of a call a
// `LibrarySpec` row governs is the merge of one record per requirement of
// the row: the bytes (or elements) behind each pointer argument, the
// terminator a `str` argument must hold within its object, and each
// `disjoint` clause. Each record is decided from the facts before the call
// and carries the witness its own check needs (§14), so a checked
// requirement is planned even when another one of the call is unresolved.
//
// A definite shortfall is reported, and the facet decided a violation,
// where the engine's library checks report it (`checkRequiredExtents`,
// `checkStringArguments`); a record here is never a violation without that
// diagnostic, so it says `checked` instead, which is sound: its check traps.
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"
#include "IntegerSupport.h"

using namespace clang;

namespace weavec::analysis {

/// The pointee of an argument before its implicit conversions (`char` for a
/// `char *` passed as `void *`), when it is a complete object type.
static std::optional<QualType> accessedElement(const Expr &argument) {
  const QualType type = argument.IgnoreParenImpCasts()->getType();
  QualType pointee;
  if (type->isPointerType())
    pointee = type->getPointeeType();
  else if (const auto *array = type->getAsArrayTypeUnsafe())
    pointee = array->getElementType();
  else
    return std::nullopt;
  if (pointee->isVoidType() || pointee->isIncompleteType() ||
      pointee->isFunctionType() || !pointee->isConstantSizeType())
    return std::nullopt;
  return pointee;
}

/// The call argument that carries row argument `rowArg`, or null.
static const Expr *rowArgument(const CallExpr &call,
                               const core::LibraryMatch &match,
                               unsigned rowArg) {
  const int index = match.callArgument(rowArg);
  if (index < 0 || static_cast<unsigned>(index) >= call.getNumArgs())
    return nullptr;
  return call.getArg(static_cast<unsigned>(index));
}

/// The row term `term` as a C term at `call`: the arguments as written,
/// which the planner examines (§10.3).
static std::optional<WitnessTerm> libraryTerm(const core::LibTerm &term,
                                              const CallExpr &call,
                                              const core::LibraryMatch &match) {
  const auto operand = [&](std::size_t i) -> std::optional<WitnessTerm> {
    if (i >= term.operands.size())
      return std::nullopt;
    return libraryTerm(term.operands[i], call, match);
  };
  switch (term.kind) {
  case core::LibTerm::Kind::Constant:
    return WitnessTerm::ofConstant(term.value);
  case core::LibTerm::Kind::Argument:
    if (const Expr *arg = rowArgument(call, match, term.arg))
      return WitnessTerm::ofExpr(*arg);
    return std::nullopt;
  case core::LibTerm::Kind::StringLength:
    if (const Expr *arg = rowArgument(call, match, term.arg))
      return WitnessTerm::strLen(WitnessTerm::ofExpr(*arg));
    return std::nullopt;
  case core::LibTerm::Kind::Product:
  case core::LibTerm::Kind::Sum: {
    auto lhs = operand(0);
    auto rhs = operand(1);
    if (!lhs || !rhs)
      return std::nullopt;
    return term.kind == core::LibTerm::Kind::Product
               ? WitnessTerm::mul(std::move(*lhs), std::move(*rhs))
               : WitnessTerm::add(std::move(*lhs), std::move(*rhs));
  }
  case core::LibTerm::Kind::Difference: {
    auto lhs = operand(0);
    if (!lhs)
      return std::nullopt;
    return WitnessTerm::sub(std::move(*lhs),
                            WitnessTerm::ofConstant(term.value));
  }
  // `fmtlen` is never a check term (§8.1); a macro's value and `min` have
  // no C spelling at the call.
  case core::LibTerm::Kind::FormatLength:
  case core::LibTerm::Kind::Macro:
  case core::LibTerm::Kind::Min:
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<core::Affine>
FunctionDataflow::libraryValue(const core::LibTerm &term, const CallExpr &call,
                               const core::LibraryMatch &match,
                               const core::AnalysisState &state) {
  const auto operand = [&](std::size_t i) -> std::optional<core::Affine> {
    if (i >= term.operands.size())
      return std::nullopt;
    return libraryValue(term.operands[i], call, match, state);
  };
  switch (term.kind) {
  case core::LibTerm::Kind::Constant:
    return core::Affine::ofConstant(term.value);
  case core::LibTerm::Kind::Argument: {
    const Expr *arg = rowArgument(call, match, term.arg);
    const auto affine = arg != nullptr ? builder.affineOf(*arg) : std::nullopt;
    return affine ? std::optional(foldAffine(*affine, state)) : std::nullopt;
  }
  case core::LibTerm::Kind::StringLength: {
    const Expr *arg = rowArgument(call, match, term.arg);
    return arg != nullptr ? stringLengthOf(*arg, state) : std::nullopt;
  }
  case core::LibTerm::Kind::Product: {
    const auto lhs = operand(0);
    const auto rhs = operand(1);
    if (!lhs || !rhs)
      return std::nullopt;
    if (lhs->isConstant())
      return rhs->times(lhs->constant);
    if (rhs->isConstant())
      return lhs->times(rhs->constant);
    return std::nullopt;
  }
  case core::LibTerm::Kind::Sum: {
    const auto lhs = operand(0);
    const auto rhs = operand(1);
    return lhs && rhs ? sumOf(*lhs, *rhs) : std::nullopt;
  }
  case core::LibTerm::Kind::Difference: {
    const auto lhs = operand(0);
    return lhs ? lhs->shifted(-term.value) : std::nullopt;
  }
  case core::LibTerm::Kind::FormatLength:
  case core::LibTerm::Kind::Macro:
  case core::LibTerm::Kind::Min:
    return std::nullopt;
  }
  return std::nullopt;
}

/// §8: rows of the `str` family, whose destination is bounded by its
/// member's own size (§7.4, as `_FORTIFY_SOURCE=2` does).
static bool isStringFamily(llvm::StringRef name) {
  return name.starts_with("str") || name.starts_with("stp") ||
         name.starts_with("wcs");
}

/// `s->name` or `s.name` for a member of array type: the destination whose
/// own bound a `str` row uses.
static const MemberExpr *memberArray(const Expr &argument) {
  const auto *member = dyn_cast<MemberExpr>(argument.IgnoreParenImpCasts());
  if (member == nullptr || !isa_and_nonnull<ConstantArrayType>(
                               member->getType()->getAsArrayTypeUnsafe()))
    return nullptr;
  return member;
}

void FunctionDataflow::decideArgumentRequirements(
    const CallExpr &call, const SiteInfo &site,
    llvm::ArrayRef<ArgumentRequirement> requirements,
    const core::AnalysisState &state) {
  const auto spell = [this](const core::Affine &amount) {
    std::string text = amount.isConstant() ? std::to_string(amount.constant)
                                           : nameOf(*amount.place);
    if (!amount.isConstant() && amount.scale != 1)
      text += "*" + std::to_string(amount.scale);
    if (!amount.isConstant() && amount.constant != 0)
      text += (amount.constant > 0 ? "+" : "-") +
              std::to_string(unsignedMagnitude(amount.constant));
    return text;
  };
  const auto publish = [&](unsigned argument, core::FacetDecision decision,
                           std::optional<std::string> need,
                           std::optional<std::string> have,
                           std::optional<CheckWitness> witness) {
    core::Requirement record;
    record.argument = argument;
    record.need = std::move(need);
    record.have = std::move(have);
    record.decision = coveredDecision(site, core::Facet::Spatial,
                                      std::move(decision), argument);
    if (witness)
      witness->argument = static_cast<std::uint8_t>(argument);
    ledger.requirement(*site.stmt, core::Facet::Spatial, std::move(record),
                       std::move(witness));
  };

  // What an argument points into, with its extent in bytes from the
  // object's start and where the argument points (bytes from that start).
  struct Target {
    KnownExtent known;
    core::Affine start;
    std::string name;
  };
  const auto targetOf = [&](const Expr &argument,
                            bool memberBound) -> std::optional<Target> {
    // §7.4: a `str` destination that is a member array is bounded by the
    // member.
    if (memberBound)
      if (const MemberExpr *member = memberArray(argument))
        if (const auto size = byteSizeOf(member->getType(), context))
          return Target{
              .known = KnownExtent{.have = core::Affine::ofConstant(*size),
                                   .origin = locate(
                                       member->getMemberDecl()->getLocation()),
                                   .pointer = std::nullopt,
                                   .offset = {},
                                   .unit = std::nullopt,
                                   .declared = true},
              .start = core::Affine::ofConstant(0),
              .name = member->getMemberDecl()->getNameAsString()};
    const auto pointed = argumentAccessOf(argument);
    if (!pointed)
      return std::nullopt;
    auto known = knownExtentOf(*pointed, state);
    if (!known)
      return std::nullopt;
    known->unit = byteSizeOf(pointed->base != nullptr
                                 ? pointed->base->getType()->getPointeeType()
                                 : QualType(),
                             context);
    // Where the argument points: the pointer's own offset and the
    // argument's arithmetic.
    core::Affine start = pointed->start;
    if (known->offset.isElements()) {
      std::int64_t shift = 0;
      if (!known->unit ||
          __builtin_mul_overflow(known->offset.elements, *known->unit, &shift))
        return std::nullopt;
      const auto shifted = start.shifted(shift);
      if (!shifted)
        return std::nullopt;
      start = *shifted;
    } else if (!known->offset.isZero()) {
      return std::nullopt;
    }
    std::string name;
    if (known->pointer)
      name = nameOf(*known->pointer);
    else if (pointed->storage != nullptr)
      name = pointed->storage->getNameAsString();
    return Target{.known = std::move(*known),
                  .start = foldAffine(start, state),
                  .name = name};
  };
  // The bytes behind the argument, `have - start`, as a term.
  const auto haveTerm = [&](const Target &target,
                            std::optional<core::PlaceId> &readsThrough)
      -> std::optional<WitnessTerm> {
    auto extent = extentTerm(target.known.have, readsThrough);
    if (!extent)
      return std::nullopt;
    if (target.start.isConstant() && target.start.constant == 0)
      return extent;
    auto start = extentTerm(target.start, readsThrough);
    if (!start)
      return std::nullopt;
    return WitnessTerm::sub(std::move(*extent), std::move(*start));
  };
  // A quantity the facts give as a constant needs no C spelling of its own
  // (`strlen("hello") + 1` is 6).
  const auto constantOr = [&](const std::optional<core::Affine> &value,
                              std::optional<WitnessTerm> term) {
    if (value) {
      const core::Affine folded = foldAffine(*value, state);
      if (folded.isConstant() && folded.constant >= 0)
        return std::optional(WitnessTerm::ofConstant(folded.constant));
    }
    return term;
  };
  // §10.4 (RFC 0030 S5): a writer of the `printf` family has no need term
  // (`fmtlen` never is one); its destination is checked through the result
  // of its bounded writer, which the planner lowers it to.
  const bool formatWriter = site.library && site.library->entry != nullptr &&
                            site.library->entry->format.has_value();
  const auto lengthWitness =
      [&](const Target &target, std::optional<WitnessTerm> need,
          std::optional<WitnessTerm> guard) -> std::optional<CheckWitness> {
    if (!need && !formatWriter)
      return std::nullopt;
    std::optional<core::PlaceId> readsThrough;
    auto have = haveTerm(target, readsThrough);
    if (!have)
      return std::nullopt;
    return CheckWitness{.shape = CheckWitness::Shape::Length,
                        .extent = std::move(have),
                        .extentClass = target.known.extentClass,
                        .need = std::move(need),
                        .unmodified = true,
                        .accessesSafe = !readsThrough ||
                                        state.nulls.isNonNull(*readsThrough),
                        .guard = std::move(guard)};
  };
  // §3.3 for one requirement against what the argument points into.
  const auto decideAgainst = [&](const ArgumentRequirement &requirement,
                                 const Target &target,
                                 const std::optional<core::Affine> &need,
                                 std::optional<WitnessTerm> needTerm) {
    const unsigned argument = requirement.argument;
    const std::string have = spell(target.known.have);
    const std::optional<std::string> needText =
        need ? std::optional(spell(*need)) : std::nullopt;
    needTerm = constantOr(need, std::move(needTerm));
    const auto checkedOr = [&](core::UnresolvedReason otherwise) {
      if (requirement.rowOnly ||
          !core::isCheckOperand(target.known.extentClass)) {
        publish(argument,
                core::FacetDecision::unresolvedFor(
                    core::UnresolvedReason::UnknownExtent),
                needText, have, std::nullopt);
        return;
      }
      auto witness =
          lengthWitness(target, std::move(needTerm), requirement.guard);
      publish(argument,
              witness
                  ? core::FacetDecision::checked()
                  : core::FacetDecision::unresolvedFor(
                        otherwise, "the requirement has no C spelling here"),
              needText, have, std::move(witness));
    };
    if (!need) {
      checkedOr(core::UnresolvedReason::Inexpressible);
      return;
    }
    const auto total = byteSum(target.start, *need, state);
    const auto evaluation =
        total ? evaluateBounds(*total, target.known, *call.getArg(argument),
                               &call, state, target.start)
              : std::nullopt;
    if (!evaluation) {
      publish(argument,
              core::FacetDecision::unresolvedFor(
                  core::UnresolvedReason::UnknownIndex),
              needText, have, std::nullopt);
      return;
    }
    if (evaluation->check.outcome == core::SpatialOutcome::Proven) {
      publish(argument, core::FacetDecision::proven(), needText, have,
              std::nullopt);
      return;
    }
    // §3.3, §7.2, §7.5: every value the facts allow falls short of an exact
    // extent, where the callee's kinds require it: the call's violation.
    const auto &verdict = evaluation->verdict;
    if (requirement.enforced && !requirement.guard && verdict &&
        target.known.exact() &&
        (verdict->kind == core::BoundsVerdict::Kind::OutOfBounds ||
         verdict->kind == core::BoundsVerdict::Kind::BeforeStart ||
         verdict->kind == core::BoundsVerdict::Kind::AtLeastPastEnd)) {
      reportBounds(*total, target.known, *call.getArg(argument), {},
                   target.name, nullptr, &call, state, false, target.start);
      publish(argument, core::FacetDecision::violation(), needText, have,
              std::nullopt);
      return;
    }
    checkedOr(core::UnresolvedReason::Inexpressible);
  };

  for (const ArgumentRequirement &requirement : requirements) {
    const unsigned i = requirement.argument;
    if (i >= call.getNumArgs())
      continue;
    const Expr &argument = *call.getArg(i);
    // A string literal has no writable byte (`strtok("a,b", ",")`).
    if (requirement.writes)
      if (const auto literal = pointsToLiteral(argument, state)) {
        checkLiteralWrite(argument, argument, &site, state);
        publish(i,
                *literal ? core::FacetDecision::violation()
                         : core::FacetDecision::unresolvedFor(
                               core::UnresolvedReason::UnknownExtent,
                               "it may point into a string literal"),
                std::nullopt, std::string("0"), std::nullopt);
        continue;
      }
    const std::optional<Target> target =
        targetOf(argument, requirement.memberBound);
    if (requirement.kind == ArgumentRequirement::Kind::String &&
        isArgvElement(argument)) {
      // §7.3: an element of `main`'s argv is nul-terminated.
      publish(i, core::FacetDecision::trustedFor(core::TrustReason::SystemApi),
              std::nullopt, std::nullopt, std::nullopt);
      continue;
    }
    if (requirement.kind == ArgumentRequirement::Kind::String) {
      // §10.3 rule 2: a terminator within the object, checked as
      // `strnlen(p, have) < have`.
      const auto length = stringLengthOf(argument, state);
      const auto fact = stringFactOf(argument, state);
      if (length && !(fact && fact->unterminated)) {
        publish(i, core::FacetDecision::proven(),
                spell(length->shifted(1).value_or(*length)), std::nullopt,
                std::nullopt);
      } else if (!target) {
        publish(i,
                core::FacetDecision::unresolvedFor(
                    core::UnresolvedReason::UnknownExtent),
                std::nullopt, std::nullopt, std::nullopt);
      } else {
        decideAgainst(
            requirement, *target, std::nullopt,
            WitnessTerm::add(WitnessTerm::strLen(WitnessTerm::ofExpr(argument)),
                             WitnessTerm::ofConstant(1)));
      }
      continue;
    }
    const auto &need = requirement.need;
    if (requirement.libraryObject) {
      // An object only the library makes and reads (`FILE`): the program
      // cannot size or form one, and what it passes is what the library
      // handed out (A3: at least one object).
      publish(i, core::FacetDecision::proven(), std::nullopt, std::nullopt,
              std::nullopt);
    } else if (need && foldAffine(*need, state).isConstant() &&
               foldAffine(*need, state).constant <= 0) {
      // Nothing is accessed (`memcpy(d, s, 0)`).
      publish(i, core::FacetDecision::proven(), spell(*need), std::nullopt,
              std::nullopt);
    } else if (!target) {
      publish(i,
              core::FacetDecision::unresolvedFor(
                  core::UnresolvedReason::UnknownExtent),
              need ? std::optional(spell(*need)) : std::nullopt, std::nullopt,
              std::nullopt);
    } else {
      decideAgainst(requirement, *target, need, requirement.needTerm);
    }
  }
}

void FunctionDataflow::decideLibraryRequirements(
    const CallExpr &call, const core::AnalysisState &state) {
  const SiteInfo *site = accessSite(call, core::Facet::Spatial);
  if (site == nullptr || site->kind != core::SiteKind::LibCall ||
      !site->library)
    return;
  // RFC 0030 §9.3: the row of a call through an *open* slot decides the
  // temporal facts only. Code outside the solved program may have stored
  // another function there, so what it needs behind its arguments is not
  // this row's business: the needs are `unresolved(callback)`.
  if (const auto resolved = callResolutions.find(&call);
      resolved != callResolutions.end() &&
      (resolved->second.kind == core::IndirectCallKind::OpenKnown ||
       resolved->second.kind == core::IndirectCallKind::OpenUnknown)) {
    std::string detail =
        resolved->second.open ? resolved->second.open->detail : std::string{};
    for (unsigned i = 0; i < call.getNumArgs(); ++i) {
      if (const SiteInfo *argument =
              siteFor(*call.getArg(i), core::Facet::Spatial, /*operand=*/true))
        decide(argument, core::Facet::Spatial,
               core::FacetDecision::unresolvedFor(
                   core::UnresolvedReason::Callback, detail));
    }
    decide(site, core::Facet::Spatial,
           core::FacetDecision::unresolvedFor(core::UnresolvedReason::Callback,
                                              std::move(detail)));
    return;
  }
  const core::LibraryMatch &match = *site->library;
  const bool stringRow = isStringFamily(match.entry->name);
  std::vector<ArgumentRequirement> requirements;
  for (unsigned i = 0; i < call.getNumArgs(); ++i) {
    const core::LibraryParam *param = match.param(i);
    if (param == nullptr || param->type != core::LibraryParam::Type::Pointer)
      continue;
    const bool sized = param->bytes || param->count;
    if (param->access == core::LibraryParam::Access::None && !sized &&
        !param->string)
      continue;
    const Expr &argument = *call.getArg(i);
    const bool writes = param->access == core::LibraryParam::Access::Write ||
                        param->access == core::LibraryParam::Access::ReadWrite;
    // The bytes (or elements) the row needs behind the argument; one
    // element of the pointee when the row names no size.
    if (sized || !param->string) {
      ArgumentRequirement requirement{
          .argument = i, .writes = writes, .memberBound = stringRow && writes};
      const auto element = accessedElement(argument);
      if (param->bytes) {
        requirement.need = libraryValue(*param->bytes, call, match, state);
        requirement.needTerm = libraryTerm(*param->bytes, call, match);
      } else if (param->count && element) {
        const auto size = byteSizeOf(*element, context);
        const auto count = libraryValue(*param->count, call, match, state);
        if (size && count)
          requirement.need = count->times(*size);
        if (auto countTerm = libraryTerm(*param->count, call, match))
          requirement.needTerm = WitnessTerm::mul(
              std::move(*countTerm), WitnessTerm::sizeOf(*element));
      } else if (!param->count && element) {
        if (const auto size = byteSizeOf(*element, context)) {
          requirement.need = core::Affine::ofConstant(*size);
          requirement.needTerm = WitnessTerm::sizeOf(*element);
        }
      }
      requirement.libraryObject = !param->bytes && !element;
      requirements.push_back(std::move(requirement));
    }
    if (param->string)
      requirements.push_back(
          ArgumentRequirement{.argument = i,
                              .kind = ArgumentRequirement::Kind::String,
                              .writes = writes,
                              .memberBound = stringRow && writes});
  }
  decideArgumentRequirements(call, *site, requirements, state);

  const auto spell = [this](const core::Affine &amount) {
    std::string text = amount.isConstant() ? std::to_string(amount.constant)
                                           : nameOf(*amount.place);
    if (!amount.isConstant() && amount.scale != 1)
      text += "*" + std::to_string(amount.scale);
    if (!amount.isConstant() && amount.constant != 0)
      text += (amount.constant > 0 ? "+" : "-") +
              std::to_string(unsignedMagnitude(amount.constant));
    return text;
  };
  const auto publish = [&](unsigned argument, core::FacetDecision decision,
                           std::optional<std::string> need,
                           std::optional<CheckWitness> witness) {
    core::Requirement record;
    record.argument = argument;
    record.need = std::move(need);
    record.decision = std::move(decision);
    if (witness)
      witness->argument = static_cast<std::uint8_t>(argument);
    ledger.requirement(*site->stmt, core::Facet::Spatial, std::move(record),
                       std::move(witness));
  };
  const auto constantOr = [&](const std::optional<core::Affine> &value,
                              std::optional<WitnessTerm> term) {
    if (value) {
      const core::Affine folded = foldAffine(*value, state);
      if (folded.isConstant() && folded.constant >= 0)
        return std::optional(WitnessTerm::ofConstant(folded.constant));
    }
    return term;
  };
  // `disjoint(d, s, n)`: nothing overlaps when nothing is copied or the two
  // arguments are different variables' storage; otherwise the overlap check
  // compares the two pointers.
  for (const core::LibDisjoint &disjoint : match.entry->disjoint) {
    const int first = match.callArgument(disjoint.first);
    const int second = match.callArgument(disjoint.second);
    if (first < 0 || second < 0 ||
        static_cast<unsigned>(std::max(first, second)) >= call.getNumArgs())
      continue;
    const auto argument = static_cast<unsigned>(first);
    const Expr &other = *call.getArg(static_cast<unsigned>(second));
    const auto length = libraryValue(disjoint.length, call, match, state);
    const std::optional<std::string> needText =
        length ? std::optional(spell(*length)) : std::nullopt;
    const auto storageRoot =
        [&](const Expr &arg) -> std::optional<core::PlaceId> {
      const ValueOrigin origin = builder.classifyValue(arg);
      if (origin.kind != ValueOrigin::Kind::Borrow || !origin.place ||
          !isStorageOfVariable(origin.place->place))
        return std::nullopt;
      return places.root(origin.place->place);
    };
    const auto a = storageRoot(*call.getArg(argument));
    const auto b = storageRoot(other);
    // A string literal is an object of its own, which nothing writes.
    const auto literal = [&](const Expr &arg) {
      const ValueOrigin origin = builder.classifyValue(arg);
      return origin.kind == ValueOrigin::Kind::Borrow && origin.place &&
             builder.isLiteralPlace(places.root(origin.place->place));
    };
    // §3.3: ranges of one object at known offsets that overlap for a known
    // length: a definite out-of-bounds (probe 21).
    const auto byteOffset =
        [&](const Expr &arg) -> std::optional<std::int64_t> {
      const ValueOrigin origin = builder.classifyValue(arg);
      const QualType type = arg.IgnoreParenImpCasts()->getType();
      const auto unit = type->isPointerType()
                            ? byteSizeOf(type->getPointeeType(), context)
                            : std::nullopt;
      if (origin.offset.isZero())
        return 0;
      if (!unit || !origin.offset.isElements())
        return std::nullopt;
      return origin.offset.elements * *unit;
    };
    const auto n =
        length ? std::optional(foldAffine(*length, state)) : std::nullopt;
    const auto from =
        a && b && *a == *b ? byteOffset(*call.getArg(argument)) : std::nullopt;
    const auto to = from ? byteOffset(other) : std::nullopt;
    if (n && n->isConstant() && to &&
        std::max(*from, *to) - std::min(*from, *to) < n->constant) {
      publish(argument, core::FacetDecision::violation(), needText,
              std::nullopt);
      core::Diagnostic diagnostic = makeError(
          core::diag::OutOfBounds,
          calleeName(call) + " copies " + std::to_string(n->constant) +
              " bytes between overlapping ranges of '" + nameOf(*a) + "'",
          call);
      if (const VarDecl *var = builder.varForPlace(*a))
        diagnostic.addNote("'" + nameOf(*a) + "' is declared here",
                           locate(var->getLocation()));
      decide(site, core::Facet::Spatial, core::FacetDecision::violation());
      report(std::move(diagnostic), core::Certainty::Definite, site,
             core::Facet::Spatial);
      continue;
    }
    if ((length && foldAffine(*length, state).isConstant() &&
         foldAffine(*length, state).constant <= 0) ||
        (a && b && *a != *b) || literal(*call.getArg(argument)) ||
        literal(other)) {
      publish(argument, core::FacetDecision::proven(), needText, std::nullopt);
      continue;
    }
    auto lengthTerm =
        constantOr(length, libraryTerm(disjoint.length, call, match));
    if (!lengthTerm) {
      publish(argument,
              core::FacetDecision::unresolvedFor(
                  core::UnresolvedReason::Inexpressible,
                  "the length has no C spelling here"),
              needText, std::nullopt);
      continue;
    }
    publish(
        argument, core::FacetDecision::checked(), needText,
        CheckWitness{.shape = CheckWitness::Shape::Disjoint,
                     .extentClass = core::ExtentClass::Exact,
                     .need = std::move(lengthTerm),
                     .other = WitnessTerm::ofExpr(*other.IgnoreParenImpCasts()),
                     .unmodified = true,
                     .accessesSafe = true});
  }
}

void FunctionDataflow::decideDeclaredRequirements(
    const CallExpr &call, const core::AnalysisState &state) {
  const SiteInfo *site = accessSite(call, core::Facet::Spatial);
  if (site == nullptr || site->kind != core::SiteKind::Call ||
      site->declaredShapes.empty())
    return;
  std::vector<ArgumentRequirement> requirements;
  for (const SiteInfo::DeclaredShape &shape : site->declaredShapes) {
    if (shape.argument >= call.getNumArgs())
      continue;
    ArgumentRequirement requirement{.argument = shape.argument};
    requirement.enforced = true;
    const core::PointerKind &kind = shape.kind;
    if (kind.shape == core::PointerShape::NulTerminated) {
      requirement.kind = ArgumentRequirement::Kind::String;
      requirements.push_back(std::move(requirement));
      continue;
    }
    // The kind's extent over the callee's parameters, as this call passes
    // them.
    std::optional<core::Affine> count;
    std::optional<WitnessTerm> countTerm;
    if (kind.extent.isConstant()) {
      count = core::Affine::ofConstant(kind.extent.offset);
      countTerm = WitnessTerm::ofConstant(kind.extent.offset);
    } else if (kind.extent.path->root == core::ExtentPath::Root::Param &&
               kind.extent.path->param < call.getNumArgs()) {
      const Expr &arg = *call.getArg(kind.extent.path->param);
      if (const auto value = builder.affineOf(arg))
        if (const auto scaled = value->times(kind.extent.scale))
          count = scaled->shifted(kind.extent.offset);
      countTerm = WitnessTerm::ofExpr(arg);
      if (kind.extent.scale != 1)
        countTerm = WitnessTerm::mul(
            std::move(*countTerm), WitnessTerm::ofConstant(kind.extent.scale));
      if (kind.extent.offset != 0)
        countTerm = WitnessTerm::add(
            std::move(*countTerm), WitnessTerm::ofConstant(kind.extent.offset));
    }
    const auto element = byteSizeOf(shape.pointee, context);
    switch (kind.shape) {
    case core::PointerShape::Counted:
      if (!element)
        continue;
      if (count)
        requirement.need = count->times(*element);
      if (countTerm)
        requirement.needTerm = WitnessTerm::mul(
            std::move(*countTerm), WitnessTerm::sizeOf(shape.pointee));
      break;
    case core::PointerShape::Sized:
      requirement.need = count;
      requirement.needTerm = std::move(countTerm);
      break;
    case core::PointerShape::Single:
      if (const auto width = objectWidthOf(shape.pointee)) {
        requirement.need = core::Affine::ofConstant(*width);
        requirement.needTerm = WitnessTerm::ofConstant(*width);
      } else {
        continue;
      }
      break;
    case core::PointerShape::EndedBy:
    case core::PointerShape::NulTerminated:
    case core::PointerShape::Unknown:
      continue;
    }
    requirements.push_back(std::move(requirement));
  }
  decideArgumentRequirements(call, *site, requirements, state);
}

} // namespace weavec::analysis
