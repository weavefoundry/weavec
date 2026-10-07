//===- EngineLibrary.cpp - Library-call requirements (object engine) ------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §8 and §3.3, over the object domain (RFC 0031 §5.4): each buffer
// argument of a `LibrarySpec` row is a requirement record on the call's
// spatial facet — its need in bytes against what the argument points into —
// proven, a violation against an exact extent, or unresolved; `disjoint`
// rows compare the two ranges.
//
//===----------------------------------------------------------------------===//

#include "Engine.h"
#include "weavec/Analysis/ClangLocation.h"

#include "clang/Basic/SourceManager.h"

using namespace clang;

namespace weavec::analysis::engine {

const Expr &pointedObject(const Expr &argument) {
  // (`buf` for `&buf[3]` and `buf + 2`.)
  const Expr *base = argument.IgnoreParenImpCasts();
  for (int depth = 0; depth < 8; ++depth) {
    if (const auto *unary = dyn_cast<UnaryOperator>(base);
        unary != nullptr && unary->getOpcode() == UO_AddrOf) {
      base = unary->getSubExpr()->IgnoreParenImpCasts();
      continue;
    }
    if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(base)) {
      base = subscript->getBase()->IgnoreParenImpCasts();
      continue;
    }
    if (const auto *binary = dyn_cast<BinaryOperator>(base);
        binary != nullptr && binary->isAdditiveOp() &&
        binary->getLHS()->getType()->isPointerType()) {
      base = binary->getLHS()->IgnoreParenImpCasts();
      continue;
    }
    break;
  }
  return *base;
}

/// The pointee of an argument before its implicit conversions.
static std::optional<QualType> accessedElement(const Expr &argument) {
  QualType type = argument.IgnoreParenImpCasts()->getType();
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

namespace {
/// How a term's value relates to the true one; `None` when it is neither.
enum class TermBound : std::uint8_t { Exact, AtMost, AtLeast, None };
} // namespace

static TermBound combineBounds(TermBound a, TermBound b) {
  if (a == b || b == TermBound::Exact)
    return a;
  if (a == TermBound::Exact)
    return b;
  return TermBound::None;
}

/// A row term over the call's argument values, with how it relates to the
/// true value (every row term is monotone in its operands).
static core::Term valueTerm(Transfer &transfer, const core::LibTerm &term,
                            const CallExpr &call,
                            const core::LibraryMatch &match,
                            const std::vector<core::Sym> &args,
                            TermBound &bound) {
  using Kind = core::LibTerm::Kind;
  auto operand = [&](std::size_t i) {
    return i < term.operands.size()
               ? valueTerm(transfer, term.operands[i], call, match, args, bound)
               : core::Term::unknown();
  };
  switch (term.kind) {
  case Kind::Constant:
    return core::Term::of(term.value);
  case Kind::Argument: {
    int index = match.callArgument(term.arg);
    if (index < 0 || static_cast<unsigned>(index) >= args.size() ||
        args[static_cast<unsigned>(index)] == core::ZeroSym)
      return core::Term::unknown();
    return transfer.termOf(args[static_cast<unsigned>(index)]);
  }
  case Kind::StringLength: {
    // RFC 0012: the length the string facts give, exactly or at most.
    int index = match.callArgument(term.arg);
    if (index < 0 || static_cast<unsigned>(index) >= args.size())
      return core::Term::unknown();
    Transfer::StringFacts facts =
        transfer.stringFacts(args[static_cast<unsigned>(index)]);
    if (facts.length == Transfer::StringFacts::Length::Unknown)
      return core::Term::unknown();
    if (facts.length == Transfer::StringFacts::Length::AtMost)
      bound = combineBounds(bound, TermBound::AtMost);
    return facts.term;
  }
  case Kind::Product: {
    core::Term a = operand(0);
    core::Term b = operand(1);
    if (!a.known || !b.known)
      return core::Term::unknown();
    if (a.isConstant() && b.isConstant()) {
      if (a.constant < 0 || b.constant < 0)
        bound = combineBounds(bound, TermBound::None);
      return core::Term::of(a.constant * b.constant);
    }
    const core::Term &k = a.isConstant() ? a : b;
    const core::Term &x = a.isConstant() ? b : a;
    if (!k.isConstant())
      return core::Term::unknown();
    if (k.constant < 0)
      bound = combineBounds(bound, TermBound::None);
    return core::Term::ofSym(x.var, x.scale * k.constant,
                             x.constant * k.constant);
  }
  case Kind::Sum: {
    auto sum = operand(0).plus(operand(1));
    return sum ? *sum : core::Term::unknown();
  }
  case Kind::Difference:
    return operand(0).plusConstant(-term.value);
  case Kind::Quotient: {
    // (Not linear: known only for a constant.)
    core::Term a = operand(0);
    if (!a.isConstant() || term.value <= 0 || a.constant < 0)
      return core::Term::unknown();
    return core::Term::of(a.constant / term.value);
  }
  case Kind::FormatLength: {
    // A literal format's least output; exact when nothing in it varies.
    Transfer::FormatFacts format = transfer.formatFacts(call, match);
    if (!format.literal)
      return core::Term::unknown();
    if (!format.exact)
      bound = combineBounds(bound, TermBound::AtLeast);
    return core::Term::of(format.lower);
  }
  case Kind::Min: {
    core::Term a = operand(0);
    core::Term b = operand(1);
    if (!a.known || !b.known)
      return core::Term::unknown();
    if (a.isConstant() && b.isConstant())
      return core::Term::of(std::min(a.constant, b.constant));
    if (transfer.domain().lessEqual(transfer.heapState(), a, b).value_or(false))
      return a;
    if (transfer.domain().lessEqual(transfer.heapState(), b, a).value_or(false))
      return b;
    return core::Term::unknown();
  }
  case Kind::Macro:
    return core::Term::unknown();
  }
  return core::Term::unknown();
}

/// §8: the `str` family, whose destination is bounded by its member.
static bool isStringFamily(llvm::StringRef name) {
  return name.starts_with("str") || name.starts_with("stp") ||
         name.starts_with("wcs");
}

void Transfer::decideArguments(const CallExpr &call, const SiteInfo &site,
                               const std::vector<ArgRequirement> &requirements,
                               const std::vector<core::Sym> &args,
                               bool library) {
  if (!run.isPublishing())
    return;
  LedgerAdapter &ledger = run.ledger();
  // The requirement being decided, for §7.5's cover of an unresolved one.
  const ArgRequirement *current = nullptr;
  auto publish = [&](unsigned argument, core::FacetDecision decision) {
    if (decision.outcome == core::SiteOutcome::Unresolved &&
        current != nullptr && argument < args.size())
      if (auto covered =
              coveredArgument(call, args[argument],
                              current->kind == ArgRequirement::Kind::String))
        decision = *covered;
    ledger.requirement(call, core::Facet::Spatial, decision);
  };
  // An amount for messages: `6 bytes`, `'n' bytes`, `'strlen(s)' + 1 bytes`.
  auto nameFor = [&](core::Sym sym, bool own) -> std::string {
    const std::string &made = heap.info(state, sym).name;
    if (own && !made.empty())
      return made;
    if (auto place = nameOf(sym, /*extent=*/true))
      return messageSpelling(*place);
    return made;
  };
  auto spellBytes = [&](const core::Term &term, bool own = false) {
    return spellAmount(term, own);
  };
  // Where the object the argument points into came from.
  auto addObjectNote = [&](core::Diagnostic &diagnostic, const Expr &argument,
                           const core::SymInfo &pointer) {
    if (pointer.targets.size() != 1)
      return;
    const core::ObjectInfo &info = run.table().info(pointer.targets[0].object);
    if (!info.created.isValid())
      return;
    std::string spelled = spell(argument);
    switch (info.key.kind) {
    case core::ObjectKind::Local:
    case core::ObjectKind::Global:
      diagnostic.addNote(spelled == info.name
                             ? "'" + spelled + "' is declared here"
                             : "the object behind '" + spelled +
                                   "' is declared here",
                         info.created);
      break;
    case core::ObjectKind::HeapRecent:
    case core::ObjectKind::HeapOld:
      diagnostic.addNote("'" + spelled + "' is allocated here", info.created);
      break;
    default:
      break;
    }
  };
  std::string callee;
  if (site.library)
    callee = "'" + site.library->entry->name + "'";
  else if (const FunctionDecl *direct = call.getDirectCallee())
    callee = "'" + direct->getNameAsString() + "'";
  else
    callee = "a function pointer";
  for (const ArgRequirement &requirement : requirements) {
    current = &requirement;
    unsigned i = requirement.argument;
    if (i >= call.getNumArgs() || i >= args.size())
      continue;
    const Expr &argument = *call.getArg(i);
    core::Sym pointer = args[i];
    const core::SymInfo value = heap.info(state, pointer);
    const core::Term &need = requirement.need;
    // A zero-length argument may be null (§5.4).
    if (value.null == core::PointerNull::Null) {
      publish(i, core::FacetDecision::proven());
      continue;
    }
    // Writing into a string literal.
    if (requirement.writes && value.targets.size() == 1 &&
        run.table().info(value.targets[0].object).key.kind ==
            core::ObjectKind::Literal) {
      publish(i, core::FacetDecision::violation());
      core::Diagnostic diagnostic;
      diagnostic.id = core::diag::OutOfBounds;
      diagnostic.message = "write through '" + spell(argument) +
                           "', which points to a string literal";
      diagnostic.location =
          toCoreLocation(context.getSourceManager(), argument.getBeginLoc());
      run.report(std::move(diagnostic), core::Certainty::Definite, &call,
                 core::Facet::Spatial);
      continue;
    }
    if (requirement.kind == ArgRequirement::Kind::Bytes && need.isConstant() &&
        need.constant <= 0) {
      publish(i, core::FacetDecision::proven());
      continue;
    }
    // What the argument points into.
    std::optional<core::Extent> extent;
    core::Term start = core::Term::unknown();
    if (requirement.memberBound)
      if (const auto *member =
              dyn_cast<MemberExpr>(argument.IgnoreParenImpCasts());
          member != nullptr && isa_and_nonnull<ConstantArrayType>(
                                   member->getType()->getAsArrayTypeUnsafe()))
        if (auto size = sizeOf(member->getType())) {
          extent = core::Extent{.bytes = core::Term::of(*size),
                                .cls = core::ExtentClass::Declared};
          start = core::Term::of(0);
        }
    if (!extent && value.targets.size() == 1 && !value.top) {
      const core::ObjectState *object =
          heap.findObject(state, value.targets[0].object);
      if (object != nullptr && object->extent && object->extent->bytes.known) {
        extent = object->extent;
        start = value.targets[0].offset;
      }
    }
    if (requirement.kind == ArgRequirement::Kind::String) {
      if (requirement.argvElement) {
        if (!requirement.formatArgument)
          publish(
              i, core::FacetDecision::trustedFor(core::TrustReason::SystemApi));
        continue;
      }
      // RFC 0012: a NUL known inside the object ends the string there; no
      // NUL to the end of an exact extent is a read past it.
      Transfer::StringFacts facts = stringFacts(pointer);
      if (facts.length != Transfer::StringFacts::Length::Unknown && extent &&
          heap.lessEqual(state, facts.nulAt.plusConstant(1), extent->bytes)
              .value_or(false)) {
        publish(i, core::FacetDecision::proven());
        continue;
      }
      if (facts.unterminated && extent &&
          extent->cls == core::ExtentClass::Exact) {
        publish(i, core::FacetDecision::violation());
        core::Diagnostic diagnostic;
        diagnostic.id = core::diag::OutOfBounds;
        diagnostic.severity = core::Severity::Error;
        diagnostic.message = callee + " reads past the end of '" +
                             spell(argument) + "', which is not NUL-terminated";
        diagnostic.location =
            toCoreLocation(context.getSourceManager(), argument.getBeginLoc());
        // Where the bytes that have no terminator were written.
        if (value.targets.size() == 1)
          if (auto written = run.byteWrites.find(value.targets[0].object);
              written != run.byteWrites.end())
            diagnostic.addNote("'" + spell(argument) +
                                   "' is left without a terminator here",
                               written->second);
        run.report(std::move(diagnostic), core::Certainty::Definite, &call,
                   core::Facet::Spatial);
        continue;
      }
      if (!extent) {
        publish(i, core::FacetDecision::unresolvedFor(
                       core::UnresolvedReason::UnknownExtent));
        continue;
      }
    } else if (!extent) {
      publish(i, core::FacetDecision::unresolvedFor(
                     core::UnresolvedReason::UnknownExtent));
      continue;
    } else if (need.known) {
      auto end = start.plus(need);
      std::optional<bool> fits =
          end ? heap.lessEqual(state, *end, extent->bytes) : std::nullopt;
      std::optional<bool> nonNegative =
          start.known ? heap.lessEqual(state, core::Term::of(0), start)
                      : std::nullopt;
      // A need known only as a bound proves one way and violates the other.
      bool atMost = requirement.bound != ArgRequirement::Bound::AtLeast;
      bool atLeast = requirement.bound != ArgRequirement::Bound::AtMost;
      if (fits.value_or(false) && nonNegative.value_or(false) && atMost) {
        publish(i, core::FacetDecision::proven());
        continue;
      }
      if (fits && !*fits && atLeast &&
          extent->cls == core::ExtentClass::Exact && requirement.enforced &&
          !requirement.guarded) {
        publish(i, core::FacetDecision::violation());
        run.requirementViolated.insert(&call);
        std::string object = "'" + spell(pointedObject(argument)) + "'";
        std::string least = requirement.bound == ArgRequirement::Bound::AtLeast
                                ? "at least "
                                : "";
        // Measured from the object's start, as its extent is.
        core::Term reach = end ? *end : need;
        std::string message = callee;
        message += library ? " accesses " : " requires ";
        message += least;
        message += spellBytes(reach, true).value_or("?");
        message += library ? " of " : " behind ";
        message += object;
        message += ", which has ";
        message += spellBytes(extent->bytes).value_or("?");
        // One value under two names: `('strlen(s)' equals 'n')`.
        if (!need.isConstant() && !extent->bytes.isConstant() &&
            need.var == extent->bytes.var) {
          std::string made = nameFor(need.var, true);
          std::string held = nameFor(need.var, false);
          if (!made.empty() && !held.empty() && made != held) {
            message += " ('";
            message += made;
            message += "' equals '";
            message += held;
            message += "')";
          }
        }
        core::Diagnostic diagnostic;
        diagnostic.id = core::diag::OutOfBounds;
        diagnostic.message = std::move(message);
        diagnostic.location =
            toCoreLocation(context.getSourceManager(), argument.getBeginLoc());
        addObjectNote(diagnostic, argument, value);
        run.report(std::move(diagnostic), core::Certainty::Definite, &call,
                   core::Facet::Spatial);
        continue;
      }
    }
    if (requirement.rowOnly || !core::isCheckOperand(extent->cls)) {
      publish(i, core::FacetDecision::unresolvedFor(
                     core::UnresolvedReason::UnknownExtent));
      continue;
    }
    publish(i, core::FacetDecision::unresolvedFor(
                   core::UnresolvedReason::Undecided));
  }
}

void Transfer::decideLibraryCall(const CallExpr &call, const SiteInfo &site,
                                 const std::vector<core::Sym> &args) {
  if (!site.library || !run.isPublishing())
    return;
  const core::LibraryMatch &match = *site.library;
  LedgerAdapter &ledger = run.ledger();
  // RFC 0030 §9.3: through an open slot the row decides temporal facts
  // only; what the function really stored there needs is unknown.
  if (call.getDirectCallee() == nullptr)
    if (auto resolution = slotResolution(call);
        resolution &&
        (resolution->kind == core::IndirectCallKind::OpenKnown ||
         resolution->kind == core::IndirectCallKind::OpenUnknown)) {
      if (ledger.applies(site.id, core::Facet::Spatial))
        ledger.decideAs(
            call, site.kind, site.boundary, core::Facet::Spatial,
            core::FacetDecision::unresolvedFor(
                core::UnresolvedReason::Callback,
                resolution->open ? resolution->open->detail : std::string()));
      return;
    }
  const bool stringRow = isStringFamily(match.entry->name);
  auto publish = [&](unsigned /*argument*/,
                     const core::FacetDecision &decision) {
    ledger.requirement(call, core::Facet::Spatial, decision);
  };
  std::string callee = "'" + match.entry->name + "'";
  std::vector<ArgRequirement> requirements;
  for (unsigned i = 0; i < call.getNumArgs() && i < args.size(); ++i) {
    const core::LibraryParam *param = match.param(i);
    if (param == nullptr || param->type != core::LibraryParam::Type::Pointer)
      continue;
    bool sized = param->bytes || param->count;
    if (param->access == core::LibraryParam::Access::None && !sized &&
        !param->string)
      continue;
    const Expr &argument = *call.getArg(i);
    bool writes = param->access == core::LibraryParam::Access::Write ||
                  param->access == core::LibraryParam::Access::ReadWrite;
    std::optional<QualType> element = accessedElement(argument);
    if (sized || !param->string) {
      ArgRequirement requirement;
      requirement.argument = i;
      requirement.writes = writes;
      requirement.memberBound = stringRow && writes;
      requirement.enforced = true;
      requirement.format = match.entry->format.has_value();
      TermBound bound = TermBound::Exact;
      if (param->bytes) {
        requirement.need =
            valueTerm(*this, *param->bytes, call, match, args, bound);
      } else if (param->count && element) {
        auto size = sizeOf(*element).value_or(1);
        core::Term count =
            valueTerm(*this, *param->count, call, match, args, bound);
        if (count.known)
          requirement.need =
              count.isConstant()
                  ? core::Term::of(count.constant * size)
                  : core::Term::ofSym(count.var, count.scale * size,
                                      count.constant * size);
      } else if (!param->count && element) {
        if (auto size = sizeOf(*element))
          requirement.need = core::Term::of(*size);
      } else {
        // An object only the library makes and reads (`FILE`): A3.
        publish(i, core::FacetDecision::proven());
        continue;
      }
      if (bound == TermBound::None)
        requirement.need = core::Term::unknown();
      else if (bound == TermBound::AtMost)
        requirement.bound = ArgRequirement::Bound::AtMost;
      else if (bound == TermBound::AtLeast)
        requirement.bound = ArgRequirement::Bound::AtLeast;
      requirements.push_back(std::move(requirement));
    }
    if (param->string) {
      ArgRequirement requirement;
      requirement.argument = i;
      requirement.kind = ArgRequirement::Kind::String;
      requirement.writes = writes;
      requirement.memberBound = stringRow && writes;
      requirement.argvElement = isArgvElement(argument);
      requirements.push_back(std::move(requirement));
    }
  }
  // A literal `printf` format: each `%s` argument is a string the call
  // reads (RFC 0012), and the conversions read no more arguments than are
  // passed (RFC 0030 §8.2).
  Transfer::FormatFacts format = formatFacts(call, match);
  if (format.literal) {
    for (unsigned argument : format.strings) {
      if (argument >= call.getNumArgs() || argument >= args.size() ||
          !call.getArg(argument)->getType()->isPointerType())
        continue;
      ArgRequirement requirement;
      requirement.argument = argument;
      requirement.kind = ArgRequirement::Kind::String;
      requirement.argvElement = isArgvElement(*call.getArg(argument));
      requirement.formatArgument = true;
      requirements.push_back(std::move(requirement));
    }
    // A `%.Ns` argument: at most N bytes, no terminator needed. Its record
    // states no need, but names it for the call's liveness guard.
    for (unsigned argument : format.bounded)
      if (argument < call.getNumArgs() && argument < args.size() &&
          call.getArg(argument)->getType()->isPointerType())
        requirements.push_back(
            ArgRequirement{.argument = argument,
                           .bound = ArgRequirement::Bound::AtMost,
                           .rowOnly = true,
                           .formatArgument = true});
    if (format.reads && *format.reads > format.passed) {
      core::Diagnostic diagnostic;
      diagnostic.id = core::diag::OutOfBounds;
      diagnostic.severity = core::Severity::Error;
      diagnostic.message = "format string of " + callee + " reads " +
                           std::to_string(*format.reads) + " arguments but " +
                           std::to_string(format.passed) + " are passed";
      diagnostic.location =
          toCoreLocation(context.getSourceManager(), call.getBeginLoc());
      run.report(std::move(diagnostic), core::Certainty::Definite, &call,
                 core::Facet::Spatial);
    }
  } else if (match.entry->format) {
    // A format that is no literal: which arguments its conversions read,
    // and as what, is not known here (RFC 0030 §8.2).
    int at = match.callArgument(match.entry->format->format);
    if (at >= 0)
      publish(static_cast<unsigned>(at),
              core::FacetDecision::unresolvedFor(
                  core::UnresolvedReason::Inexpressible,
                  "the format is not a string literal"));
  }
  decideArguments(call, site, requirements, args, /*library=*/true);
  // `disjoint(d, s, n)` (§3.3).
  for (const core::LibDisjoint &disjoint : match.entry->disjoint) {
    int first = match.callArgument(disjoint.first);
    int second = match.callArgument(disjoint.second);
    if (first < 0 || second < 0 ||
        static_cast<unsigned>(std::max(first, second)) >= args.size())
      continue;
    auto argument = static_cast<unsigned>(first);
    TermBound lengthBound = TermBound::Exact;
    core::Term length =
        valueTerm(*this, disjoint.length, call, match, args, lengthBound);
    if (lengthBound != TermBound::Exact)
      length = core::Term::unknown();
    const core::SymInfo a = heap.info(state, args[argument]);
    const core::SymInfo b =
        heap.info(state, args[static_cast<unsigned>(second)]);
    if (length.isConstant() && length.constant <= 0) {
      publish(argument, core::FacetDecision::proven());
      continue;
    }
    bool distinct =
        !a.top && !b.top && !a.targets.empty() && !b.targets.empty();
    if (distinct)
      for (const core::Target &x : a.targets)
        for (const core::Target &y : b.targets)
          if (heap.mayOverlap(state, x.object, y.object))
            distinct = false;
    auto isLiteral = [&](const core::SymInfo &info) {
      return info.targets.size() == 1 &&
             run.table().info(info.targets[0].object).key.kind ==
                 core::ObjectKind::Literal;
    };
    if (distinct || isLiteral(a) || isLiteral(b)) {
      publish(argument, core::FacetDecision::proven());
      continue;
    }
    // One object at known offsets: overlap is decided.
    if (a.targets.size() == 1 && b.targets.size() == 1 &&
        a.targets[0].object == b.targets[0].object &&
        run.table().info(a.targets[0].object).singular &&
        a.targets[0].offset.isConstant() && b.targets[0].offset.isConstant() &&
        length.isConstant()) {
      std::int64_t gap =
          a.targets[0].offset.constant - b.targets[0].offset.constant;
      if (gap < 0)
        gap = -gap;
      // RFC 0033 §1: a copy onto itself (`fe25519_copy(h, h)`, a struct
      // assigned to itself) is undefined by the letter but leaves the bytes
      // as they were in every supported C library: no violation.
      if (gap == 0 || gap >= length.constant) {
        publish(argument, core::FacetDecision::proven());
        continue;
      }
      publish(argument, core::FacetDecision::violation());
      std::string object = run.table().info(a.targets[0].object).name;
      core::Diagnostic diagnostic;
      diagnostic.id = core::diag::OutOfBounds;
      diagnostic.message = callee;
      diagnostic.message += " copies ";
      diagnostic.message += std::to_string(length.constant);
      diagnostic.message += " bytes between overlapping ranges of '";
      diagnostic.message += object;
      diagnostic.message += '\'';
      diagnostic.location =
          toCoreLocation(context.getSourceManager(), call.getBeginLoc());
      diagnostic.addNote("'" + object + "' is declared here",
                         run.table().info(a.targets[0].object).created);
      run.report(std::move(diagnostic), core::Certainty::Definite, &call,
                 core::Facet::Spatial);
      continue;
    }
    publish(argument, core::FacetDecision::unresolvedFor(
                          core::UnresolvedReason::Undecided));
  }
}

} // namespace weavec::analysis::engine
