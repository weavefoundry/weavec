//===- DataflowFormats.cpp - Formatted output (RFC 0024) ----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "Dataflow.h"
#include "IntegerSupport.h"
#include "weavec/Core/Format.h"

#include <algorithm>
#include <limits>

using namespace clang;
namespace weavec::analysis {
static QualType formatType(core::FormatType type, const ASTContext &context) {
  using enum core::FormatType;
  switch (type) {
  case Int:
    return context.IntTy;
  case UInt:
    return context.UnsignedIntTy;
  case Long:
    return context.LongTy;
  case ULong:
    return context.UnsignedLongTy;
  case LongLong:
    return context.LongLongTy;
  case ULongLong:
    return context.UnsignedLongLongTy;
  case IntMax:
    return context.getIntMaxType();
  case UIntMax:
    return context.getUIntMaxType();
  case SignedSize:
    return context.getSignedSizeType();
  case Size:
    return context.getSizeType();
  case Ptrdiff:
    return context.getPointerDiffType();
  case UPtrdiff:
    return context.getCorrespondingUnsignedType(context.getPointerDiffType());
  case Double:
    return context.DoubleTy;
  case LongDouble:
    return context.LongDoubleTy;
  case String:
    return context.getPointerType(context.CharTy);
  case Pointer:
    return context.VoidPtrTy;
  }
  return {};
}

FunctionDataflow::FormatResult FunctionDataflow::runtimeFormatArguments(
    const Expr &format, const CallExpr &call, unsigned first,
    const std::optional<core::ArgumentListState> &list,
    const std::optional<CheckedMemory> &destination, core::AnalysisState &state,
    std::optional<std::string_view> literal) {
  FormatResult result{.valid = true, .upper = {}, .exact = {}};
  const auto issue = [&](bool proved, bool required,
                         const std::string &reason) {
    safetyObligation(core::SafetyProperty::Call,
                     core::safetyOutcome(proved, required), call, "format",
                     reason);
    result.valid &= proved || required;
  };
  const auto *text = dyn_cast<StringLiteral>(format.IgnoreParenImpCasts());
  std::optional<std::string> spelling;
  if (literal)
    spelling = *literal;
  else if (text && text->isOrdinary())
    spelling = text->getBytes().str();
  if (spelling && !core::OutputFormat::parse(*spelling).valid()) {
    issue(false, false, core::OutputFormat::parse(*spelling).error);
    return result;
  }
  if (list) {
    const auto formatRef = builder.resolve(format);
    auto path =
        formatRef ? builder.summaryPathOf(formatRef->place) : std::nullopt;
    if (!path && spelling)
      path = core::SummaryPath::param(0);
    const auto source =
        list->input ? builder.summaryPathOf(*list->input) : std::nullopt;
    const bool representable =
        path && (spelling || formatRef) && (!list->input || source) &&
        (list->input || list->first <= core::MaxFormatArguments);
    if (representable && recording())
      inferred.checked.require(
          {.kind = core::CheckedRequirementKind::FormatArguments,
           .path = *path,
           .other = source.value_or(core::SummaryPath{}),
           .begin = core::PathAffine::ofConstant(list->input ? std::int64_t{-1}
                                                             : list->first),
           .family = spelling ? core::encodeFormatLiteral(*spelling)
                              : std::string{}});
    issue(false, representable,
          "format and variadic argument pack must be compatible");
    return result;
  }
  if (!spelling) {
    issue(false, false, "format string cannot be bound to a supported literal");
    return result;
  }
  const auto parsed = core::OutputFormat::parse(*spelling);
  if (!parsed.valid()) {
    issue(false, false, parsed.error);
    return result;
  }
  if (first > call.getNumArgs() ||
      parsed.arguments > call.getNumArgs() - first) {
    issue(false, false, "format conversion is missing a variadic argument");
    return result;
  }
  result.upper = static_cast<std::int64_t>(parsed.literalBytes);
  result.exact = result.upper;
  const auto constant = [&](unsigned index) -> std::optional<std::int64_t> {
    const auto value = builder.affineOf(*call.getArg(first + index));
    const auto folded = value ? foldAffine(*value, state) : core::Affine{};
    return value && folded.isConstant() ? std::optional(folded.constant)
                                        : std::nullopt;
  };
  const auto add = [](std::optional<std::int64_t> &total,
                      const std::optional<std::int64_t> &part) {
    std::int64_t sum = 0;
    if (total && part && !__builtin_add_overflow(*total, *part, &sum) &&
        sum <= std::numeric_limits<int>::max())
      total = sum;
    else
      total.reset();
  };
  for (const auto &conversion : parsed.conversions) {
    const auto &argument = *call.getArg(first + conversion.argument);
    const auto actual = argument.getType();
    const auto expected = formatType(conversion.type, context);
    bool compatible = ASTContext::hasSameUnqualifiedType(actual, expected);
    if (conversion.type == core::FormatType::String)
      compatible =
          actual->isPointerType() && actual->getPointeeType()->isCharType();
    if (conversion.narrow && conversion.type == core::FormatType::UInt)
      compatible |= ASTContext::hasSameUnqualifiedType(actual, context.IntTy);
    issue(compatible, false,
          "format argument must have the required promoted type");
    std::optional<std::int64_t> width = conversion.width;
    std::optional<std::int64_t> precision = conversion.precision;
    bool precisionKnown = true;
    for (const auto index :
         {conversion.widthArgument, conversion.precisionArgument})
      if (index)
        issue(ASTContext::hasSameUnqualifiedType(
                  call.getArg(first + *index)->getType(), context.IntTy),
              false, "format width and precision arguments must have type int");
    if (conversion.widthArgument) {
      width = constant(*conversion.widthArgument);
      if (width && *width < 0)
        width = -*width;
    }
    if (conversion.precisionArgument) {
      precision = constant(*conversion.precisionArgument);
      precisionKnown = precision.has_value();
      if (precision && *precision < 0)
        precision.reset();
    }
    std::optional<std::int64_t> upper;
    std::optional<std::int64_t> exact;
    if (conversion.type == core::FormatType::String && compatible) {
      const auto limit =
          precision ? std::optional(core::Affine::ofConstant(*precision))
                    : std::nullopt;
      const auto memory = runtimeString(argument, limit, call, state);
      result.valid &= memory.has_value();
      if (memory && destination)
        result.valid &= runtimeSeparate(*memory, *destination, call, state);
      // Forwarded packs must also be separate from fixed output-capable inputs.
      if (memory && !destination)
        for (unsigned index = 0; index < first; ++index) {
          const auto *fixed = call.getArg(index);
          if (fixed == &format || !fixed->getType()->isPointerType() ||
              fixed->getType()->getPointeeType().isConstQualified())
            continue;
          if (auto output = checkedMemory(*fixed, {}, {}, state);
              output && output->extent) {
            output->end = *output->extent;
            result.valid &= runtimeSeparate(*memory, *output, call, state);
          }
        }
      const auto length = stringLengthOf(argument, state);
      const auto folded = length ? foldAffine(*length, state) : core::Affine{};
      if (length && folded.isConstant()) {
        upper = folded.constant;
        if (precision)
          upper = std::min(*upper, *precision);
        if (precisionKnown)
          exact = upper;
      } else if (precision) {
        upper = *precision;
      }
    } else if (conversion.conversion == 'c') {
      upper = 1;
      exact = 1;
    } else if (expected->isIntegerType()) {
      upper = static_cast<std::int64_t>(context.getIntWidth(expected)) + 3;
      if (precision)
        upper = std::max(*upper, *precision + 3);
      if (!precisionKnown)
        upper.reset();
      const auto range = integerRangeOf(argument, state);
      const auto value = range && !range->mayBeInvalid
                             ? range->values.constant()
                             : std::nullopt;
      if (value && !conversion.narrow && precisionKnown) {
        // Count digits after target promotions/conversions. In particular,
        // (unsigned)-1 prints UINT_MAX, not the magnitude of signed -1.
        const auto magnitude = value->magnitude();
        unsigned radix = 10;
        if (conversion.conversion == 'o')
          radix = 8;
        else if (conversion.conversion == 'x' || conversion.conversion == 'X')
          radix = 16;
        std::int64_t digits = 1;
        for (auto rest = magnitude / radix; rest; rest /= radix)
          ++digits;
        if (magnitude == 0 && precision && *precision == 0)
          digits = 0;
        if (precision)
          digits = std::max(digits, *precision);
        if (expected->isSignedIntegerType() &&
            (value->negative() || conversion.sign))
          ++digits;
        if (conversion.alternate && radix != 10)
          digits += radix == 8 ? 1 : 2;
        // Alternate octal and zero-valued hex need special cases. Keep an upper
        // bound.
        if (!conversion.alternate)
          exact = digits;
        upper = digits;
      }
    }
    if (upper && width)
      upper = std::max(*upper, *width);
    else
      upper.reset();
    if (exact && width)
      exact = std::max(*exact, *width);
    else
      exact.reset();
    add(result.upper, upper);
    add(result.exact, exact);
  }
  return result;
}

void FunctionDataflow::runtimeFormat(const CallExpr &call,
                                     std::string_view name,
                                     core::AnalysisState &state) {
  const bool variadicList = name.starts_with('v');
  if (variadicList)
    name.remove_prefix(1);
  const bool bounded = name == "snprintf";
  const bool buffer = bounded || name == "sprintf";
  const bool stream = name == "fprintf";
  const auto *callee = call.getDirectCallee();
  const bool fortified = callee != nullptr && callee->getBuiltinID() != 0 &&
                         callee->getName().starts_with("__builtin___") &&
                         callee->getName().ends_with("_chk");
  unsigned formatIndex = bounded ? 2U : 0U;
  if (!bounded && (buffer || stream))
    formatIndex = 1;
  if (fortified)
    formatIndex += 2;
  const unsigned first = formatIndex + 1;
  if (call.getNumArgs() < first + static_cast<unsigned>(variadicList))
    return;
  std::optional<core::ArgumentListState> list;
  if (variadicList) {
    list = runtimeList(*call.getArg(first), call, state);
    if (!list)
      return;
  }
  if (stream)
    runtimeStream(*call.getArg(0), call, state);
  else if (!buffer)
    runtimeStandardOutput(call, state);
  (void)runtimeString(*call.getArg(formatIndex), {}, call, state);
  std::optional<core::Affine> capacity;
  std::optional<CheckedMemory> destination;
  if (buffer) {
    capacity = bounded ? builder.affineOf(*call.getArg(1)) : std::nullopt;
    if (capacity)
      capacity = foldAffine(*capacity, state);
    if (!bounded || !capacity || !capacity->isConstant() ||
        capacity->constant != 0)
      destination = checkedMemory(*call.getArg(0), {}, {}, state);
    if (destination && capacity)
      if (const auto end =
              checkedByteSum(destination->begin, *capacity, state, call))
        destination->end = *end;
  }
  const auto format = runtimeFormatArguments(*call.getArg(formatIndex), call,
                                             first, list, destination, state);
  if (variadicList)
    if (const auto place = runtimeListPlace(*call.getArg(first)))
      state.safety->argumentLists[*place].phase =
          core::ArgumentListPhase::Consumed;
  if (!buffer || !format.valid)
    return;
  if (!bounded && format.upper)
    capacity = core::Affine::ofConstant(*format.upper + 1);
  if (!capacity) {
    safetyObligation(
        core::SafetyProperty::Bounds, core::SafetyOutcome::Unresolved, call,
        "format",
        "formatted output requires a represented capacity or upper bound");
    return;
  }
  if (fortified) {
    auto object = builder.affineOf(*call.getArg(formatIndex - 1));
    Expr::EvalResult evaluated;
    bool unknown = false;
    if (call.getArg(formatIndex - 1)->EvaluateAsInt(evaluated, context) &&
        evaluated.Val.isInt()) {
      const auto &value = evaluated.Val.getInt();
      unknown = value.isAllOnes();
      if (value.getActiveBits() <= 63)
        object = core::Affine::ofConstant(
            static_cast<std::int64_t>(value.getZExtValue()));
    } else if (const auto *query = dyn_cast<CallExpr>(
                   call.getArg(formatIndex - 1)->IgnoreParenImpCasts());
               query && query->getNumArgs() == 2) {
      const auto *builtin = query->getDirectCallee();
      Expr::EvalResult mode;
      // An unevaluated maximum-size query returns the all-ones sentinel when
      // the compiler cannot determine its object. Actual capacity is checked
      // independently below; this query never supplies missing capacity.
      unknown = builtin != nullptr && builtin->getBuiltinID() != 0 &&
                builtin->getName() == "__builtin_object_size" &&
                query->getArg(1)->EvaluateAsInt(mode, context) &&
                mode.Val.isInt() && mode.Val.getInt().getLimitedValue() <= 1;
    }
    const auto folded = object ? foldAffine(*object, state) : core::Affine{};
    // (size_t)-1 is the compiler's unknown object-size sentinel.
    unknown |= object && folded.isConstant() && folded.constant == -1;
    const bool fits =
        unknown || (object && checkedAtMost(*capacity, *object, state));
    safetyObligation(core::SafetyProperty::Bounds,
                     core::safetyOutcome(fits, false), call, "format",
                     "fortified output must fit its object-size bound");
    if (!fits)
      return;
  }
  if (bounded && capacity->isConstant() && capacity->constant == 0)
    return;
  destination =
      runtimeInterval(*call.getArg(0), *capacity, false, true, call, state);
  if (!destination)
    return;
  const auto positive = capacity->shifted(-1);
  core::PlaceGuard nonempty;
  if (!positive ||
      !checkedAtMost(core::Affine::ofConstant(0), *positive, state)) {
    if (!capacity->place || capacity->scale != 1 || capacity->constant != 0)
      return;
    nonempty.require(*capacity->place,
                     core::ValueFact::of(core::Outcome::Positive));
  }
  for (const auto outcome : {core::Outcome::Zero, core::Outcome::Positive}) {
    const auto post = [&](core::InitializedRange range) {
      range.when = nonempty;
      checkedPosts[&call].push_back({.path = core::SummaryPath::param(0),
                                     .range = range,
                                     .on = outcome,
                                     .storage = destination->storage,
                                     .objectType = {}});
    };
    post({.begin = destination->begin,
          .end = destination->end,
          .terminatedWithin = true});
    if (const auto end = destination->begin.shifted(1))
      post({.begin = destination->begin, .end = *end});
    if (format.exact && capacity->isConstant()) {
      const auto characters =
          bounded ? std::min(*format.exact, capacity->constant - 1)
                  : *format.exact;
      const auto zero = destination->begin.shifted(characters);
      const auto end = zero ? zero->shifted(1) : std::nullopt;
      if (zero && end) {
        post({.begin = destination->begin, .end = *end});
        post({.begin = *zero, .end = *end, .zeroed = true});
      }
    }
  }
  // RFC 0024: a checked returned count names the written prefix even when
  // the format has no exact output length. Constant capacities split the
  // min(result, capacity - 1) relation into two bounded result intervals.
  const auto result = numericCallResult(call);
  const auto type = integerTypeOf(call.getType(), context);
  if (bounded && result && type && capacity->isConstant() &&
      capacity->constant > 0 && destination->begin.isConstant()) {
    const auto maximum =
        core::IntegerRange::full(*type).maximum()->signedValue();
    if (!maximum)
      return;
    const auto establish = [&](core::InitializedRange range, std::int64_t lower,
                               std::int64_t upper) {
      if (lower > upper)
        return;
      range.when.require(*result,
                         core::ValueFact::ofInteger(core::IntegerRange::between(
                             core::IntegerValue::ofBits(
                                 *type, static_cast<std::uint64_t>(lower)),
                             core::IntegerValue::ofBits(
                                 *type, static_cast<std::uint64_t>(upper)))));
      checkedPosts[&call].push_back({.path = core::SummaryPath::param(0),
                                     .range = range,
                                     .on = {},
                                     .storage = destination->storage,
                                     .objectType = {}});
    };
    const auto zero =
        core::Affine::ofPlace(*result).shifted(destination->begin.constant);
    const auto end = zero ? zero->shifted(1) : std::nullopt;
    const auto fits = std::min(capacity->constant - 1, *maximum);
    if (zero && end) {
      establish({.begin = destination->begin, .end = *end}, 0, fits);
      establish({.begin = *zero, .end = *end, .zeroed = true}, 0, fits);
    }
    if (const auto last = destination->end.shifted(-1)) {
      establish({.begin = destination->begin, .end = destination->end},
                capacity->constant, *maximum);
      establish({.begin = *last, .end = destination->end, .zeroed = true},
                capacity->constant, *maximum);
    }
  }
}
} // namespace weavec::analysis
