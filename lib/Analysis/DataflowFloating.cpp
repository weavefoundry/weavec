//===- DataflowFloating.cpp - Numeric input facts (RFC 0029) -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "AffineSupport.h"
#include "Dataflow.h"
#include "IntegerSupport.h"

#include <algorithm>

using namespace clang;
namespace weavec::analysis {

static bool numericTextByte(std::uint64_t value) {
  return value == 0 || (value >= '0' && value <= '9') || value == '+' ||
         value == '-' || value == '.' || value == 'e' || value == 'E';
}

bool FunctionDataflow::checkedNumericByte(const Expr &value,
                                          const core::AnalysisState &state) {
  if (context.getCharWidth() != 8)
    return false;
  const auto range = integerRangeOf(value, state);
  if (!range || range->mayBeInvalid || range->values.empty())
    return false;
  const core::IntegerType byte{.width = 8, .isSigned = false};
  const auto actual = range->values.converted(byte);
  return std::ranges::all_of(actual.all(), [](const auto &interval) {
    return (interval.lower == interval.upper &&
            numericTextByte(interval.lower)) ||
           (interval.lower >= '0' && interval.upper <= '9') ||
           (interval.lower >= '-' && interval.upper <= '.');
  });
}

bool FunctionDataflow::checkedNumericText(const CheckedMemory &memory,
                                          const core::AnalysisState &state) {
  if (!state.safety || state.safety->havoc || context.getCharWidth() != 8)
    return false;
  // An unrepresented existential string endpoint is not an empty string.
  if (checkedAtMost(memory.end, memory.begin, state))
    return false;
  const auto ranges = state.safety->memory.find(memory.storage);
  if (ranges == state.safety->memory.end())
    return false;
  auto through = foldAffine(memory.begin, state);
  for (std::size_t step = 0; step <= ranges->second.size(); ++step) {
    if (checkedAtMost(memory.end, through, state))
      return true;
    bool advanced = false;
    for (const auto &range : ranges->second) {
      auto when = range.when;
      if ((!range.numericText && !range.zeroed) || range.source ||
          !pruneGuard(when, state) || !when.trivial() ||
          !checkedAtMost(range.begin, through, state) ||
          !checkedAtMost(through, range.end, state) ||
          checkedAtMost(range.end, through, state))
        continue;
      through = range.end;
      advanced = true;
      break;
    }
    if (!advanced)
      break;
  }
  return false;
}

bool FunctionDataflow::checkedFloatingValue(const Expr &value,
                                            const core::AnalysisState &state) {
  if (!state.safety || state.safety->havoc ||
      !value.getType()->isRealFloatingType())
    return false;
  llvm::APFloat constant(0.0);
  if (value.EvaluateAsFloat(constant, context))
    return !constant.isNaN();
  const auto *expr = value.IgnoreParenImpCasts();
  if (const auto *call = dyn_cast<CallExpr>(expr)) {
    const auto result = checkedFloatingResults.find(call);
    return result != checkedFloatingResults.end() &&
           state.safety->nonNan.contains(result->second);
  }
  const auto *reference = dyn_cast<DeclRefExpr>(expr);
  const auto *variable =
      reference ? dyn_cast<VarDecl>(reference->getDecl()) : nullptr;
  return variable != nullptr && variable->hasLocalStorage() &&
         !variable->getType().isVolatileQualified() &&
         !variable->getType()->isAtomicType() &&
         !addressTaken.contains(variable->getCanonicalDecl()) &&
         state.safety->nonNan.contains(builder.placeForVar(*variable));
}

void FunctionDataflow::checkedFloatingAfter(const Stmt &stmt,
                                            core::AnalysisState &state) {
  const auto assign = [&](const VarDecl &variable, const Expr *value) {
    if (!variable.getType()->isRealFloatingType())
      return;
    const auto place = builder.placeForVar(variable);
    const bool nonNan = value && checkedFloatingValue(*value, state);
    state.safety->nonNan.erase(place);
    if (nonNan && variable.hasLocalStorage() &&
        !variable.getType().isVolatileQualified() &&
        !variable.getType()->isAtomicType() &&
        !addressTaken.contains(variable.getCanonicalDecl()))
      state.safety->nonNan.insert(place);
  };
  if (const auto *declarations = dyn_cast<DeclStmt>(&stmt)) {
    for (const auto *declaration : declarations->decls()) {
      const auto *variable = dyn_cast<VarDecl>(declaration);
      if (!variable)
        continue;
      assign(*variable, variable->getInit());
      const auto *array = context.getAsConstantArrayType(variable->getType());
      const auto *initial = variable->getInit();
      if (!array || !array->getElementType()->isCharType() || !initial ||
          variable->getType().isVolatileQualified() ||
          context.getCharWidth() != 8)
        continue;
      bool numeric = false;
      if (const auto *text = dyn_cast<StringLiteral>(initial->IgnoreImpCasts()))
        numeric = text->isOrdinary() &&
                  std::ranges::all_of(text->getBytes(), [](unsigned char c) {
                    return numericTextByte(c);
                  });
      else if (const auto *list = dyn_cast<InitListExpr>(initial))
        numeric = std::ranges::all_of(list->inits(), [&](const Expr *value) {
          return checkedNumericByte(*value, state);
        });
      if (numeric)
        if (const auto bytes = byteSizeOf(variable->getType(), context))
          state.safety->initialize(builder.placeForVar(*variable),
                                   {.begin = {},
                                    .end = core::Affine::ofConstant(*bytes),
                                    .numericText = true});
    }
  }
  const Expr *written = nullptr;
  const Expr *value = nullptr;
  if (const auto *assignment = dyn_cast<BinaryOperator>(&stmt);
      assignment && assignment->isAssignmentOp()) {
    written = assignment->getLHS();
    if (assignment->getOpcode() == BO_Assign)
      value = assignment->getRHS();
  }
  if (const auto *unary = dyn_cast<UnaryOperator>(&stmt);
      unary && unary->isIncrementDecrementOp())
    written = unary->getSubExpr();
  const auto *reference =
      written ? dyn_cast<DeclRefExpr>(written->IgnoreParenImpCasts()) : nullptr;
  if (const auto *variable =
          reference ? dyn_cast<VarDecl>(reference->getDecl()) : nullptr)
    assign(*variable, value);
}
} // namespace weavec::analysis
