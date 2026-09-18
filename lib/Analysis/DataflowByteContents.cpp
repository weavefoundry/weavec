//===- DataflowByteContents.cpp - Bounded byte values (RFC 0029) ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "IntegerSupport.h"

using namespace clang;

namespace weavec::analysis {

std::optional<std::pair<std::int64_t, std::string>>
FunctionDataflow::checkedByteContents(const CheckedMemory &memory,
                                      const core::AnalysisState &state) {
  if (!state.safety || context.getCharWidth() != 8 || !memory.extent)
    return std::nullopt;
  const auto found = state.safety->memory.find(memory.storage);
  if (found == state.safety->memory.end() ||
      std::ranges::none_of(
          found->second,
          [](const auto &range) { return !range.bytes.empty(); }) ||
      !checkedValid(memory, state) ||
      !checkedInterval(memory.begin, memory.end, *memory.extent, state) ||
      !checkedInitialized(memory, state))
    return std::nullopt;
  auto first = foldAffine(memory.begin, state);
  std::optional<std::int64_t> low = first.constant;
  std::optional<std::int64_t> high = first.constant;
  if (first.place) {
    if (first.scale != 1)
      return std::nullopt;
    std::tie(low, high) = integerBounds(*first.place, state);
    const auto relations = checkedRelations(state);
    const auto upper = relations.bound(first.place, std::nullopt);
    const auto negativeLower = relations.bound(std::nullopt, first.place);
    if (relations.limited()) {
      inferred.checked.limited = true;
      inferred.incomplete.insert("traversal relational limit reached");
      return std::nullopt;
    }
    if (upper)
      high = high ? std::min(*high, *upper) : *upper;
    if (negativeLower && *negativeLower != INT64_MIN)
      low = low ? std::max(*low, -*negativeLower) : -*negativeLower;
    if (!low || !high || __builtin_add_overflow(*low, first.constant, &*low) ||
        __builtin_add_overflow(*high, first.constant, &*high))
      return std::nullopt;
  }
  if (*low < 0 || *high < *low || *high - *low >= 64)
    return std::nullopt;
  for (const auto &range : found->second) {
    if (range.bytes.empty() || range.source || !range.begin.isConstant() ||
        !range.end.isConstant() || range.begin.constant > *low ||
        *high >= range.end.constant)
      continue;
    auto when = range.when;
    if (!pruneGuard(when, state) || !when.trivial())
      continue;
    return std::pair{*low,
                     range.bytes.substr(
                         static_cast<std::size_t>(*low - range.begin.constant),
                         static_cast<std::size_t>(*high - *low + 1))};
  }
  return std::nullopt;
}

std::optional<core::IntegerRange>
FunctionDataflow::checkedByteRange(const Expr &expr,
                                   const core::AnalysisState &state) {
  if (!state.safety || !expr.getType()->isCharType() ||
      expr.getType().isVolatileQualified() || expr.getType()->isAtomicType())
    return std::nullopt;
  const auto type = integerTypeOf(expr.getType(), context);
  const auto memory = checkedLvalue(expr, state);
  const auto bytes =
      memory ? checkedByteContents(*memory, state) : std::nullopt;
  if (!type || !bytes)
    return std::nullopt;
  auto result = core::IntegerRange(*type);
  for (const char byte : bytes->second)
    result = result.united(core::IntegerRange::singleton(
        core::IntegerValue::ofBits(*type, static_cast<unsigned char>(byte))));
  return result;
}

} // namespace weavec::analysis
