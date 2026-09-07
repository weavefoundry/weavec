//===- DataflowCheckedIntegers.cpp - Checked sizes and results (RFC 0017)
//--===//
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

bool FunctionDataflow::handleCheckedIntegerCall(const CallExpr &call,
                                                core::AnalysisState &state) {
  const auto op = checkedIntegerOp(call);
  if (!op)
    return false;
  const auto pointee =
      builder.pointeeOf(builder.classifyValue(*call.getArg(2)));
  const auto type =
      integerTypeOf(call.getArg(2)->getType()->getPointeeType(), context);
  const auto a = integerRangeOf(*call.getArg(0), state);
  const auto b = integerRangeOf(*call.getArg(1), state);
  if (!pointee || !type || !a || !b) {
    reportIncomplete("unsupported checked integer output", call);
    forgetNullnessReachable(builder.classifyValue(*call.getArg(2)), state);
    return true;
  }
  doRead(*pointee, call, state, false);
  doMutationCheck(pointee->place, call, state);
  checkAnnotationOnWrite(*pointee, call, state);
  recordAccess(pointee->place, true, state);
  auto values = core::evaluateCheckedInteger(*op, a->values, b->values, *type);
  if (a->mayBeInvalid || b->mayBeInvalid) {
    values.values = core::IntegerRange::full(*type);
    values.overflow = core::IntegerRange::full(core::BooleanType);
  }
  const auto lhs = integerExpressionOf(*call.getArg(0), state);
  const auto rhs = integerExpressionOf(*call.getArg(1), state);
  std::optional<NumericExpression> stored;
  std::optional<NumericExpression> overflow;
  if (lhs && rhs) {
    overflow = NumericExpression::overflow(*op, *lhs, *rhs, *type);
    const auto convertedA = lhs->converted(*type);
    const auto convertedB = rhs->converted(*type);
    if (convertedA && convertedB)
      stored =
          NumericExpression::operation(*op, *convertedA, *convertedB, true);
  }
  auto &outputs = numericCallOutputs[&call];
  const auto resultPath = core::SummaryPath::result();
  if (!outputs.contains(resultPath)) {
    const auto result =
        places.create("overflow-result@" + std::to_string(locate(call).line));
    outputs.emplace(resultPath, result);
    snapshotPlaces.insert(result);
  }
  const auto result = outputs.at(resultPath);
  auto &saved = integerStatementResults[&call];
  if (!saved) {
    saved =
        places.create("checked-output@" + std::to_string(locate(call).line));
    snapshotPlaces.insert(*saved);
  }
  snapshotIntegerDependencies(result, &call, state);
  snapshotScalar(result, &call, state);
  snapshotIntegerDependencies(*saved, &call, state);
  snapshotScalar(*saved, &call, state);
  state.dropGuardsOn(result);
  state.dropGuardsOn(*saved);
  if (overflow)
    state.numericValues.insert_or_assign(result, *overflow);
  if (stored)
    state.numericValues.insert_or_assign(*saved, *stored);
  assignScalar(pointee->place, nullptr, state, &call);
  std::vector<core::PlaceId> cells = mirrors(pointee->place, state);
  cells.push_back(pointee->place);
  llvm::append_range(cells, borrowedImages(pointee->place, state));
  for (const auto cell : cells) {
    state.scalars.set(cell, core::ValueFact::ofInteger(values.values));
    if (const auto frozen = state.numericValues.find(*saved);
        frozen != state.numericValues.end() && !frozen->second.dependsOn(cell))
      state.numericValues.insert_or_assign(cell, frozen->second);
  }
  state.numericValues.erase(*saved);
  state.scalars.set(result, core::ValueFact::ofInteger(values.overflow));
  return true;
}

void FunctionDataflow::specializeIntegerBuiltin(const CallExpr &call,
                                                core::FunctionSummary &summary,
                                                core::AnalysisState &state) {
  const auto *callee = call.getDirectCallee();
  if (!callee || !callee->getIdentifier())
    return;
  const auto name = callee->getName();
  if (name != "calloc" && name != "reallocarray")
    return;
  const unsigned first = name == "calloc" ? 0 : 1;
  if (call.getNumArgs() != first + 2)
    return;
  const auto type = integerTypeOf(context.getSizeType(), context);
  const auto a = integerRangeOf(*call.getArg(first), state);
  const auto b = integerRangeOf(*call.getArg(first + 1), state);
  if (!type || !a || !b || a->mayBeInvalid || b->mayBeInvalid ||
      a->values.empty() || b->values.empty())
    return;
  const auto lhs = a->values.converted(*type);
  const auto rhs = b->values.converted(*type);
  if (rhs.minimum()->bits != 0 &&
      lhs.minimum()->bits > type->mask() / rhs.minimum()->bits) {
    // A checked product overflow returns null and reallocarray retains its
    // input allocation. There is no tiny successful wrapped allocation.
    summary = {};
    summary.addReturn(core::ValueSource::null());
    summary.addOutcome(core::Outcome::Null);
    return;
  }
  using Expression = core::IntegerExpression<core::SummaryPath>;
  const auto product = Expression::operation(
      core::IntegerOp::Multiply,
      Expression::input(core::SummaryPath::param(first), *type),
      Expression::input(core::SummaryPath::param(first + 1), *type));
  if (!product)
    return;
  auto returns = std::move(summary.returns);
  summary.returns.clear();
  for (auto value : returns) {
    // On a successful allocation, the checked mathematical product fits
    // size_t and equals this typed product. Failure has no object extent.
    if (value.isFresh()) {
      value.extent = core::PathAffine::ofExpression(*product);
      if (const auto overflow = Expression::overflow(
              core::IntegerOp::Multiply,
              Expression::input(core::SummaryPath::param(first), *type),
              Expression::input(core::SummaryPath::param(first + 1), *type),
              *type))
        value.when.requireInteger(
            {.lhs = *overflow,
             .op = core::IntegerOp::Equal,
             .rhs = Expression::constant(
                 core::IntegerValue::ofBits(core::BooleanType, 0))});
    }
    summary.addReturn(std::move(value));
  }
}

} // namespace weavec::analysis
