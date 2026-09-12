//===- DataflowArgumentLists.cpp - Variadic cursors (RFC 0024) -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "Dataflow.h"
#include "weavec/Core/Format.h"

using namespace clang;
namespace weavec::analysis {
bool FunctionDataflow::runtimeListType(QualType type) const {
  for (;;) {
    const auto *alias = type->getAs<TypedefType>();
    if (!alias)
      return false;
    if (alias->getDecl()->getCanonicalDecl() ==
        context.getBuiltinVaListDecl()->getCanonicalDecl())
      return true;
    type = alias->desugar();
  }
}

std::optional<core::PlaceId>
FunctionDataflow::runtimeListPlace(const Expr &expr) {
  const auto *value = expr.IgnoreParenImpCasts();
  if (const auto *address = dyn_cast<UnaryOperator>(value);
      address && address->getOpcode() == UO_AddrOf)
    value = address->getSubExpr()->IgnoreParenImpCasts();
  const auto *reference = dyn_cast<DeclRefExpr>(value);
  const auto *variable =
      reference ? dyn_cast<VarDecl>(reference->getDecl()) : nullptr;
  if (!variable)
    return {};
  const auto *parameter = dyn_cast_or_null<ParmVarDecl>(variable);
  const auto type =
      parameter ? parameter->getOriginalType() : variable->getType();
  if (!runtimeListType(type))
    return {};
  const auto place = builder.resolve(*value);
  return place ? std::optional(place->place) : std::nullopt;
}

std::optional<core::ArgumentListState>
FunctionDataflow::runtimeList(const Expr &expr, const Stmt &at,
                              core::AnalysisState &state) {
  const auto place = runtimeListPlace(expr);
  bool required = false;
  if (place && !state.safety->argumentLists.contains(*place))
    if (const auto path = builder.summaryPathOf(*place);
        path && path->isParam()) {
      state.safety->argumentLists[*place] = {
          .phase = core::ArgumentListPhase::Active, .input = *place};
      if (recording())
        inferred.checked.require(
            {.kind = core::CheckedRequirementKind::ArgumentList,
             .path = *path,
             .other = {},
             .family = {}});
      required = true;
    }
  const auto found = place ? state.safety->argumentLists.find(*place)
                           : state.safety->argumentLists.end();
  const bool active = found != state.safety->argumentLists.end() &&
                      found->second.phase == core::ArgumentListPhase::Active;
  safetyObligation(
      core::SafetyProperty::Initialization,
      core::safetyOutcome(active && !required, required), at, "argument list",
      "variadic traversal requires an active unconsumed argument list");
  return active ? std::optional(found->second) : std::nullopt;
}

bool FunctionDataflow::runtimeListIntrinsic(const CallExpr &call,
                                            core::AnalysisState &state) {
  const auto *callee = call.getDirectCallee();
  if (!callee || !callee->getBuiltinID())
    return false;
  const auto name = callee->getName();
  const bool start =
      name == "__builtin_va_start" || name == "__builtin_c23_va_start";
  const bool copy = name == "__builtin_va_copy";
  const bool end = name == "__builtin_va_end";
  if (!start && !copy && !end)
    return false;
  const auto dest =
      call.getNumArgs() ? runtimeListPlace(*call.getArg(0)) : std::nullopt;
  bool valid = dest.has_value();
  if (start && name == "__builtin_va_start") {
    const auto *last = function.getNumParams()
                           ? function.getParamDecl(function.getNumParams() - 1)
                           : nullptr;
    const auto *anchor =
        call.getNumArgs() == 2
            ? dyn_cast<DeclRefExpr>(call.getArg(1)->IgnoreParenImpCasts())
            : nullptr;
    valid &= last != nullptr && anchor != nullptr && anchor->getDecl() == last;
    if (last) {
      const auto type = last->getOriginalType();
      valid &= last->getStorageClass() != SC_Register && !type->isArrayType() &&
               !type->isFunctionType() &&
               !type->isSpecificBuiltinType(BuiltinType::Float) &&
               !context.isPromotableIntegerType(type);
    }
  }
  if (dest) {
    auto &list = state.safety->argumentLists[*dest];
    if (start || copy) {
      valid &= !list.needsEnd &&
               (start ? function.isVariadic() : call.getNumArgs() == 2);
      const auto source = copy && call.getNumArgs() == 2
                              ? runtimeList(*call.getArg(1), call, state)
                              : std::nullopt;
      valid &= !copy || source.has_value();
      if (valid) {
        list = source.value_or(
            core::ArgumentListState{.phase = core::ArgumentListPhase::Active,
                                    .first = function.getNumParams()});
        list.needsEnd = true;
        list.copy = copy;
        state.safety->initialized.insert(*dest);
      }
    } else {
      const bool deferred = options.deferCheckedCalls && list.needsEnd &&
                            list.phase == core::ArgumentListPhase::Unknown &&
                            state.safety->deferred.contains(*dest);
      valid &= (list.phase == core::ArgumentListPhase::Active ||
                list.phase == core::ArgumentListPhase::Consumed || deferred) &&
               list.needsEnd;
      if (deferred && recording())
        inferred.checked.deferred = true;
      if (valid) {
        list.phase = core::ArgumentListPhase::Ended;
        list.needsEnd = false;
      }
    }
  }
  safetyObligation(core::SafetyProperty::Semantics,
                   core::safetyOutcome(valid, false), call, "argument list",
                   "variadic list lifecycle operation must be valid");
  return true;
}

void FunctionDataflow::runtimeListReturns(const Stmt &at,
                                          const core::AnalysisState &state) {
  for (const auto &[place, list] : state.safety->argumentLists) {
    if (list.needsEnd)
      safetyObligation(
          core::SafetyProperty::Resource, core::SafetyOutcome::Unresolved, at,
          "argument list",
          "locally started or copied argument list requires va_end");
    if (list.input && !list.copy &&
        list.phase != core::ArgumentListPhase::Active && recording())
      if (const auto path = builder.summaryPathOf(*list.input))
        runtimeConsumedLists.insert(*path);
    (void)place;
  }
}

bool FunctionDataflow::runtimeRequirement(
    const core::CheckedRequirement &requirement, const CallExpr &call,
    core::AnalysisState &state) {
  if (requirement.kind == core::CheckedRequirementKind::StandardStream) {
    runtimeStandardOutput(call, state);
    return true;
  }
  const auto argument = [&](const core::SummaryPath &path) -> const Expr * {
    return path.isParam() && path.isRoot() && path.index < call.getNumArgs()
               ? call.getArg(path.index)
               : nullptr;
  };
  if (requirement.kind == core::CheckedRequirementKind::ArgumentList) {
    if (const auto *expr = argument(requirement.path))
      (void)runtimeList(*expr, call, state);
    else
      safetyObligation(core::SafetyProperty::Call,
                       core::SafetyOutcome::Unresolved, call, "argument list",
                       "argument-list input cannot be bound");
    return true;
  }
  if (requirement.kind != core::CheckedRequirementKind::FormatArguments)
    return false;
  const auto *format = argument(requirement.path);
  std::optional<core::ArgumentListState> list;
  if (requirement.begin.constant == -1)
    if (const auto *expr = argument(requirement.other))
      list = runtimeList(*expr, call, state);
  if (!format || (requirement.begin.constant == -1 && !list)) {
    safetyObligation(core::SafetyProperty::Call,
                     core::SafetyOutcome::Unresolved, call, "format",
                     "format argument pack cannot be bound");
    return true;
  }
  const auto literal = core::decodeFormatLiteral(requirement.family);
  if (!requirement.family.empty() && !literal) {
    safetyObligation(core::SafetyProperty::Call,
                     core::SafetyOutcome::Unresolved, call, "format",
                     "invalid portable format literal");
    return true;
  }
  (void)runtimeFormatArguments(
      *format, call,
      requirement.begin.constant < 0
          ? 0U
          : static_cast<unsigned>(requirement.begin.constant),
      list, {}, state,
      literal ? std::optional<std::string_view>(*literal) : std::nullopt);
  return true;
}
} // namespace weavec::analysis
