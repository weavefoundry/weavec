//===- LibrarySummaries.cpp - LibrarySpec rows as summaries (RFC 0030 §8) -===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §8, §15 item 7: the engine applies a library call through the
// summary its `LibrarySpec` row states, at the provider step where RFC 0003
// consulted the shipped table. The row is read in the parameter positions
// of the declaration it governs, so a fortified alias
// (`__builtin___memcpy_chk(d, s, n, size)`) has the effects of the row on
// the arguments the alias maps (§8, *Which calls a row governs*).
//
//   access r / w / rw      a read / write of the pointee (a shared or mutable
//                          borrow for the call); `none` borrows nothing
//   release(F)             the argument is freed (family F)
//   realloc(F)             moved into the result on the non-null class; on
//                          the null class freed when the size is zero (§8.2)
//   retain(S), escape      the library keeps the argument: it escapes
//   init(F), fini(F)       the pointee is written; its storage stays valid
//   out(v)                 a store of `v` through the argument (`replaces`
//                          consumes the old value first, as `getline` does)
//   null                   forbidden, or allowed when a length is zero, is a
//                          requirement (§8.3); `null-ok` is none
//   bytes / count          a requirement on the extent, when the term is
//                          affine in one argument (the others are the call's
//                          requirement records, §15 item 4)
//   noreturn / exits       the call ends the path
//
// Results: `fresh(F)` with its extent and failure class, `arg(N)` as a copy,
// `interior(N)` as an interior copy; static storage, hidden state and `ptr`
// are values the summary cannot name. What the table states beyond a
// summary (hidden state, callbacks, strings, copies, fills, formats) is read
// from the row by the engine at the call.
//
//===----------------------------------------------------------------------===//

#include "IntegerSupport.h"
#include "weavec/Analysis/KindTable.h"
#include "weavec/Analysis/Summaries.h"

#include "clang/AST/ASTContext.h"

#include <set>
#include <vector>

using namespace clang;

namespace weavec::analysis {

/// The callee parameter that carries row argument `rowArg`.
static std::optional<std::uint32_t> calleeParam(const core::LibraryMatch &match,
                                                unsigned rowArg) {
  const int index = match.callArgument(rowArg);
  if (index < 0)
    return std::nullopt;
  return static_cast<std::uint32_t>(index);
}

/// A row term as an affine in one callee parameter (`a2`, `a1 + 1`, `4`,
/// `2 * a0`), counted in units of `unit` bytes; nothing for anything else.
static std::optional<core::PathAffine>
affineTerm(const core::LibTerm &term, const core::LibraryMatch &match,
           std::int64_t unit) {
  using Kind = core::LibTerm::Kind;
  const auto scaled =
      [unit](std::int64_t value) -> std::optional<std::int64_t> {
    std::int64_t result = 0;
    if (__builtin_mul_overflow(value, unit, &result))
      return std::nullopt;
    return result;
  };
  switch (term.kind) {
  case Kind::Constant:
    if (const auto bytes = scaled(term.value))
      return core::PathAffine::ofConstant(*bytes);
    return std::nullopt;
  case Kind::Argument:
    if (const auto param = calleeParam(match, term.arg))
      return core::PathAffine::ofPath(core::SummaryPath::param(*param), unit);
    return std::nullopt;
  case Kind::Sum:
  case Kind::Product: {
    if (term.operands.size() != 2)
      return std::nullopt;
    const core::LibTerm &lhs = term.operands[0];
    const core::LibTerm &rhs = term.operands[1];
    const bool lhsConstant = lhs.kind == Kind::Constant;
    const core::LibTerm &constant = lhsConstant ? lhs : rhs;
    const core::LibTerm &variable = lhsConstant ? rhs : lhs;
    if (constant.kind != Kind::Constant || variable.kind != Kind::Argument)
      return std::nullopt;
    const auto param = calleeParam(match, variable.arg);
    if (!param)
      return std::nullopt;
    if (term.kind == Kind::Sum) {
      const auto offset = scaled(constant.value);
      if (!offset)
        return std::nullopt;
      return core::PathAffine::ofPath(core::SummaryPath::param(*param), unit,
                                      *offset);
    }
    const auto factor = scaled(constant.value);
    if (!factor || *factor <= 0)
      return std::nullopt;
    return core::PathAffine::ofPath(core::SummaryPath::param(*param), *factor);
  }
  case Kind::Difference: {
    if (term.operands.size() != 1 || term.operands[0].kind != Kind::Argument)
      return std::nullopt;
    const auto param = calleeParam(match, term.operands[0].arg);
    const auto offset = scaled(-term.value);
    if (!param || !offset)
      return std::nullopt;
    return core::PathAffine::ofPath(core::SummaryPath::param(*param), unit,
                                    *offset);
  }
  default:
    return std::nullopt;
  }
}

/// The bytes of the pointee of `callee`'s parameter `index`, for `count(t)`
/// terms (one byte for `void` and incomplete pointees).
static std::optional<std::int64_t> elementBytes(const FunctionDecl &callee,
                                                std::uint32_t index) {
  const auto *prototype = callee.getType()->getAs<FunctionProtoType>();
  if (prototype == nullptr || index >= prototype->getNumParams())
    return std::nullopt;
  const QualType type = prototype->getParamType(index);
  if (!type->isPointerType())
    return std::nullopt;
  const QualType pointee = type->getPointeeType();
  if (pointee->isVoidType() || pointee->isIncompleteType() ||
      !pointee->isConstantSizeType())
    return 1;
  return callee.getASTContext().getTypeSizeInChars(pointee).getQuantity();
}

/// `size == 0` for the size a row's result extent states (`a1` for
/// `realloc`, `a1 * a2` for `reallocarray`): the condition under which a
/// `realloc(F)` argument is freed on the null class (§8.2).
static std::optional<core::PathGuard>
zeroSizeGuard(const core::LibraryEntry &row, const core::LibraryMatch &match,
              const FunctionDecl &callee) {
  if (!row.result.extent)
    return std::nullopt;
  const core::LibTerm &size = *row.result.extent;
  core::PathGuard guard;
  if (size.kind == core::LibTerm::Kind::Argument) {
    const auto param = calleeParam(match, size.arg);
    if (!param)
      return std::nullopt;
    guard.require(core::SummaryPath::param(*param),
                  core::ValueFact::of(core::Outcome::Zero));
    return guard;
  }
  if (size.kind != core::LibTerm::Kind::Product || size.operands.size() != 2 ||
      size.operands[0].kind != core::LibTerm::Kind::Argument ||
      size.operands[1].kind != core::LibTerm::Kind::Argument)
    return std::nullopt;
  const auto first = calleeParam(match, size.operands[0].arg);
  const auto second = calleeParam(match, size.operands[1].arg);
  const ASTContext &context = callee.getASTContext();
  const auto type = integerTypeOf(context.getSizeType(), context);
  if (!first || !second || !type)
    return std::nullopt;
  using Expression = core::IntegerExpression<core::SummaryPath>;
  const auto product = Expression::operation(
      core::IntegerOp::Multiply,
      Expression::input(core::SummaryPath::param(*first), *type),
      Expression::input(core::SummaryPath::param(*second), *type));
  if (!product)
    return std::nullopt;
  guard.requireInteger(
      {.lhs = *product,
       .op = core::IntegerOp::Equal,
       .rhs = Expression::constant(core::IntegerValue::ofBits(*type, 0))});
  return guard;
}

/// The value a result or `out` value stands for, without its null
/// alternative: a fresh resource, a copy of an argument, or unknown.
static core::ValueSource valueOf(const core::LibraryResult &value,
                                 const core::LibraryMatch &match) {
  using Kind = core::LibraryResult::Kind;
  switch (value.kind) {
  case Kind::Fresh: {
    // Stack storage (`alloca`) is the frame's, not a resource the caller
    // releases: the engine gives it its own value at the call.
    if (value.family == core::StackFamily)
      return core::ValueSource::unknown();
    std::optional<core::PathAffine> extent;
    if (value.extent)
      extent = affineTerm(*value.extent, match, 1);
    return core::ValueSource::freshAt(value.family, core::PointerOffset::zero(),
                                      std::move(extent));
  }
  case Kind::Arg:
    if (const auto param = calleeParam(match, value.arg))
      return core::ValueSource::copy(core::SummaryPath::param(*param));
    return core::ValueSource::unknown();
  case Kind::Interior:
    if (const auto param = calleeParam(match, value.arg))
      return core::ValueSource::interiorCopy(core::SummaryPath::param(*param));
    return core::ValueSource::unknown();
  default:
    return core::ValueSource::unknown();
  }
}

/// Whether a result may be null: `null-on-failure` and `null-ok`.
static bool mayBeNull(const core::LibraryResult &value) {
  return value.null != core::LibraryResult::Null::Never;
}

core::FunctionSummary librarySummaryOf(const core::LibraryMatch &match,
                                       const FunctionDecl &callee) {
  core::FunctionSummary summary;
  const core::LibraryEntry &row = *match.entry;
  using Effect = core::LibraryParam::Effect;
  using Access = core::LibraryParam::Access;
  for (unsigned rowArg = 0; rowArg < row.params.size(); ++rowArg) {
    const core::LibraryParam &param = row.params[rowArg];
    const auto index = calleeParam(match, rowArg);
    if (!index || param.type != core::LibraryParam::Type::Pointer)
      continue;
    const core::SummaryPath root = core::SummaryPath::param(*index);
    const core::SummaryPath pointee = root.deref();
    core::PlaceEffect access;
    access.read =
        param.access == Access::Read || param.access == Access::ReadWrite;
    access.written =
        param.access == Access::Write || param.access == Access::ReadWrite;
    switch (param.effect) {
    case Effect::Release:
      summary.addEffect(
          root, core::PlaceEffect{.freed = true, .family = param.family});
      break;
    case Effect::Realloc:
      summary.addEffect(
          root, core::PlaceEffect{.moved = true, .family = param.family});
      // §8.2: kept on the null class, unless the size was zero; released
      // (moved into the result) on the non-null class.
      summary.addOutcome(
          core::Outcome::NonNull, root,
          core::PlaceEffect{.moved = true, .family = param.family});
      if (const auto zero = zeroSizeGuard(row, match, callee))
        summary.addOutcome(core::Outcome::Null, root,
                           core::PlaceEffect{.freed = true,
                                             .family = param.family,
                                             .when = *zero});
      else
        summary.addOutcome(core::Outcome::Null);
      break;
    case Effect::Retain:
    case Effect::Escape:
      summary.addEffect(root, core::PlaceEffect{.escaped = true});
      break;
    case Effect::Init:
    case Effect::Fini:
      access.written = true;
      break;
    case Effect::Borrow:
      break;
    }
    if (param.effect != Effect::Release && param.effect != Effect::Realloc &&
        (access.read || access.written))
      summary.addEffect(pointee, access);
    if (param.null != core::LibraryParam::Null::Allowed)
      summary.requiresNonNull.insert(*index);
    // The extent the callee needs behind the pointer (RFC 0011).
    std::optional<core::PathAffine> need;
    if (param.bytes)
      need = affineTerm(*param.bytes, match, 1);
    else if (param.count)
      if (const auto unit = elementBytes(callee, *index))
        need = affineTerm(*param.count, match, *unit);
    if (need && (!need->isConstant() || need->constant > 0))
      summary.addRequirement(*index, core::ExtentRequirement{
                                         .need = std::move(*need), .when = {}});
    // `out(v)`: the call stores `v` through the argument; a null-ok
    // argument receives it only when it is not null.
    if (param.out) {
      if (param.out->replaces)
        summary.addEffect(pointee,
                          core::PlaceEffect{.moved = true,
                                            .replaced = true,
                                            .family = param.out->family});
      core::ValueSource value = valueOf(*param.out, match);
      if (param.null == core::LibraryParam::Null::Allowed)
        value.when.require(root, core::ValueFact::of(core::Outcome::NonNull));
      summary.addStore(core::Store{.dest = pointee, .value = std::move(value)});
    }
  }

  // An `out` value that is `null-on-failure` is stored only when the call
  // succeeds (§8.2): on the classes that report failure (a null result, or
  // a non-zero integer, which is how `posix_memalign`, `getaddrinfo` and
  // `getifaddrs` fail) the slot holds nothing the call stored.
  const core::LibraryResult &result = row.result;
  std::set<core::SummaryPath> failing;
  for (unsigned rowArg = 0; rowArg < row.params.size(); ++rowArg)
    if (const core::LibraryParam &param = row.params[rowArg];
        param.out && param.out->null == core::LibraryResult::Null::OnFailure)
      if (const auto index = calleeParam(match, rowArg))
        failing.insert(core::SummaryPath::param(*index).deref());
  if (!failing.empty() && result.kind != core::LibraryResult::Kind::Void) {
    const bool pointer = result.isPointer();
    for (const core::Outcome outcome :
         pointer ? std::vector{core::Outcome::NonNull, core::Outcome::Null}
                 : std::vector{core::Outcome::Zero, core::Outcome::Positive,
                               core::Outcome::Negative}) {
      summary.addOutcome(outcome);
      const bool succeeds =
          outcome == (pointer ? core::Outcome::NonNull : core::Outcome::Zero);
      summary.storesOn[outcome] =
          succeeds ? failing : std::set<core::SummaryPath>{};
      if (!succeeds)
        summary.nullOn[outcome] = failing;
    }
  }
  if (result.isPointer()) {
    summary.addReturn(valueOf(result, match));
    // `realpath(p, NULL)`, `getcwd(NULL, n)`: fresh when the argument is
    // null.
    if (result.kind == core::LibraryResult::Kind::Arg &&
        !result.family.empty()) {
      core::ValueSource fresh = core::ValueSource::fresh(result.family);
      if (const auto param = calleeParam(match, result.arg))
        fresh.when.require(core::SummaryPath::param(*param),
                           core::ValueFact::of(core::Outcome::Null));
      summary.addReturn(std::move(fresh));
    }
    if (mayBeNull(result))
      summary.addReturn(core::ValueSource::null());
  }
  summary.neverReturns = row.noreturn || row.exits;
  return summary;
}

} // namespace weavec::analysis
