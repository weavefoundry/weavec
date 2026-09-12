//===- DataflowRuntime.cpp - Checked runtime effects (RFC 0024) ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "Dataflow.h"
#include "IntegerSupport.h"
#include "RuntimeModels.h"

#include "clang/Basic/SourceManager.h"

#include <algorithm>
#include <array>

using namespace clang;
namespace weavec::analysis {

std::optional<FunctionDataflow::CheckedMemory>
FunctionDataflow::runtimeInterval(const Expr &pointer,
                                  const core::Affine &bytes, bool read,
                                  bool write, const Stmt &at,
                                  core::AnalysisState &state) {
  auto memory = checkedMemory(pointer, {}, bytes, state);
  if (!memory) {
    safetyObligation(core::SafetyProperty::Bounds,
                     core::SafetyOutcome::Unresolved, at, "runtime memory",
                     "runtime memory interval is not represented");
    return {};
  }
  bool complete = true;
  const auto require = [&](core::CheckedRequirementKind kind, bool proved,
                           core::SafetyProperty property, const char *reason) {
    const bool input = !proved && checkedRequire(kind, *memory, at, state);
    safetyObligation(property, core::safetyOutcome(proved, input), at,
                     "runtime memory", reason);
    complete &= proved || input;
  };
  require(core::CheckedRequirementKind::Valid, checkedValid(*memory, state),
          core::SafetyProperty::Validity,
          "runtime operation requires live non-null storage");
  require(core::CheckedRequirementKind::Extent,
          memory->extent && checkedInterval(memory->begin, memory->end,
                                            *memory->extent, state),
          core::SafetyProperty::Bounds,
          "runtime access interval must fit its object");
  if (read)
    require(core::CheckedRequirementKind::Initialized,
            checkedInitialized(*memory, state),
            core::SafetyProperty::Initialization,
            "runtime input interval must be initialized");
  if (write)
    complete &= checkedWrite(*memory, at, state);
  return complete ? memory : std::nullopt;
}

std::optional<FunctionDataflow::CheckedMemory>
FunctionDataflow::runtimeString(const Expr &pointer,
                                const std::optional<core::Affine> &limit,
                                const Stmt &at, core::AnalysisState &state) {
  const auto length = stringLengthOf(pointer, state);
  const auto through = length ? length->shifted(1) : std::nullopt;
  if (limit && (!through || !checkedAtMost(*through, *limit, state)))
    return runtimeInterval(pointer, *limit, true, false, at, state);
  if (through)
    return runtimeInterval(pointer, *through, true, false, at, state);
  auto memory = checkedMemory(pointer, {}, {}, state);
  const bool proved = memory && checkedTerminated(*memory, state);
  const bool required = memory && !proved &&
                        checkedRequire(core::CheckedRequirementKind::Terminated,
                                       *memory, at, state);
  safetyObligation(core::SafetyProperty::Initialization,
                   core::safetyOutcome(proved, required), at, "runtime string",
                   "runtime string requires an initialized terminator");
  if (memory && (proved || required)) {
    // An existential terminator does not give an exact string length. Use
    // the whole known object as a conservative read envelope for overlap.
    if (memory->extent)
      memory->end = *memory->extent;
    if (const auto witness = checkedWitness(*memory, state))
      if (const auto end = witness->zero.shifted(1))
        memory->end = *end;
    return memory;
  }
  return {};
}

bool FunctionDataflow::runtimeSeparate(const CheckedMemory &first,
                                       const CheckedMemory &second,
                                       const Stmt &at,
                                       core::AnalysisState &state) {
  bool proved = false;
  if (first.storage == second.storage) {
    // Empty endpoints also represent an unavailable envelope. They must not
    // turn an unknown string read or sprintf output into disjoint storage.
    proved = first.begin != first.end && second.begin != second.end &&
             (checkedAtMost(first.end, second.begin, state) ||
              checkedAtMost(second.end, first.begin, state));
  } else {
    const bool localA =
        isLocalStorage(first.storage) || builder.isLiteralPlace(first.storage);
    const bool localB = isLocalStorage(second.storage) ||
                        builder.isLiteralPlace(second.storage);
    const auto fresh = [&](const CheckedMemory &m) {
      const auto resource =
          state.resources.recordOf(m.holder.value_or(m.storage));
      if (!resource || resource->origin != core::ResourceOrigin::Allocated ||
          resource->escaped)
        return false;
      auto guard = resource->guard;
      return pruneGuard(guard, state) && guard.trivial();
    };
    proved = (localA && localB &&
              places.root(first.storage) != places.root(second.storage)) ||
             (localA && (second.input || fresh(second))) ||
             (localB && (first.input || fresh(first)));
    if (fresh(first) && fresh(second)) {
      const auto firstResource =
          state.resources.recordOf(first.holder.value_or(first.storage));
      const auto secondResource =
          state.resources.recordOf(second.holder.value_or(second.storage));
      proved |= firstResource->location != secondResource->location;
    }
  }
  const bool required =
      !proved && first.input && second.input && *first.input != *second.input;
  if (required && recording())
    inferred.checked.require({.kind = core::CheckedRequirementKind::Separated,
                              .path = *first.input,
                              .other = *second.input,
                              .family = {}});
  safetyObligation(core::SafetyProperty::Aliasing,
                   core::safetyOutcome(proved, required), at, "runtime overlap",
                   "runtime input and output intervals must be disjoint");
  return proved || required;
}

bool FunctionDataflow::runtimeStream(const Expr &pointer, const Stmt &at,
                                     core::AnalysisState &state,
                                     bool allowNull) {
  const auto origin = builder.classifyValue(pointer);
  bool proved = allowNull && origin.kind == ValueOrigin::Kind::Null;
  if (const auto *ref = dyn_cast<DeclRefExpr>(pointer.IgnoreParenImpCasts()))
    if (const auto *var = dyn_cast<VarDecl>(ref->getDecl());
        var &&
        context.getSourceManager().isInSystemHeader(var->getLocation())) {
      static constexpr auto Streams = std::to_array<std::string_view>(
          {"stdin", "stdout", "stderr", "__stdinp", "__stdoutp", "__stderrp"});
      if (std::ranges::find(Streams, var->getNameAsString()) != Streams.end()) {
        const auto place = builder.lookupVar(*var);
        proved |=
            !state.safety->havoc &&
            (!place || (!findMoved(*place, state) &&
                        !state.safety->replacedPointers.contains(*place) &&
                        !state.safety->invalidatedPointers.contains(*place) &&
                        state.nulls.stateOf(*place) != core::Nullness::Null));
      }
    }
  const auto memory = checkedMemory(pointer, {}, {}, state);
  if (memory) {
    const auto holder = memory->holder.value_or(memory->storage);
    const auto resource = state.resources.recordOf(holder);
    proved |= resource && resource->family == "fclose" &&
              !state.moves.recordOf(holder) && checkedValid(*memory, state) &&
              !state.safety->invalidatedPointers.contains(holder);
  }
  // Release-family evidence is a sufficient, deliberately strong generic
  // stream precondition. Stream reads themselves confer no ownership.
  const bool required = !proved && memory &&
                        checkedRequire(core::CheckedRequirementKind::Release,
                                       *memory, at, state, "fclose");
  safetyObligation(core::SafetyProperty::Resource,
                   core::safetyOutcome(proved, required), at, "stream",
                   "runtime operation requires a live C stream");
  return proved || required;
}

void FunctionDataflow::runtimeStandardOutput(const Stmt &at,
                                             core::AnalysisState &state) {
  bool valid = !state.safety->havoc;
  bool replaced = false;
  for (const auto *var : builder.variables()) {
    // A custom header can declare the real standard stream too. Such a
    // declaration may invalidate this dependency without granting provenance.
    if (!var->hasGlobalStorage() || !var->isExternallyVisible() ||
        (var->getName() != "stdout" && var->getName() != "__stdoutp"))
      continue;
    const auto place = builder.lookupVar(*var);
    if (!place)
      continue;
    bool moved = false;
    if (const auto hit = findMoved(*place, state)) {
      auto guard = hit->record.guard;
      moved = pruneGuard(guard, state);
    }
    const auto memory = checkedMemoryAt(*place, {}, {}, state);
    const auto resource = state.resources.recordOf(*place);
    const bool live = memory && resource && resource->family == "fclose" &&
                      checkedValid(*memory, state) && !moved;
    const bool changed = state.safety->replacedPointers.contains(*place) ||
                         state.safety->invalidatedPointers.contains(*place) ||
                         state.nulls.stateOf(*place) == core::Nullness::Null;
    valid &= !moved && (!changed || live);
    replaced |= changed && live;
  }
  const bool required = valid && !replaced && !function.isMain();
  if (required && recording())
    inferred.checked.require(
        {.kind = core::CheckedRequirementKind::StandardStream,
         .path = {},
         .other = {},
         .family = "stdout"});
  safetyObligation(core::SafetyProperty::Resource,
                   core::safetyOutcome(valid && !required, required), at,
                   "stdout", "implicit output requires a live standard stream");
}

bool FunctionDataflow::runtimeIntrinsic(const CallExpr &call,
                                        core::AnalysisState &state) {
  const auto *callee = call.getDirectCallee();
  if (!callee || !callee->getBuiltinID())
    return false;
  if (runtimeListIntrinsic(call, state))
    return true;
  const auto name = callee->getName();
  const bool expect =
      name == "__builtin_expect" || name == "__builtin_expect_with_probability";
  static constexpr auto Classifiers = std::to_array<std::string_view>(
      {"__builtin_isnan", "__builtin_isinf", "__builtin_isinf_sign",
       "__builtin_isfinite", "__builtin_isnormal", "__builtin_signbit"});
  if (!expect &&
      std::ranges::find(Classifiers, name.str()) == Classifiers.end())
    return false;
  prepareNumericCall(call, core::FunctionSummary{}, state);
  if (expect && call.getNumArgs() >= 2)
    if (const auto result = numericCallResult(call)) {
      if (const auto value = integerExpressionOf(*call.getArg(0), state))
        state.numericValues.insert_or_assign(*result, *value);
      if (const auto fact = scalarFactOf(*call.getArg(0), state))
        state.scalars.set(*result, *fact);
    }
  safetyObligation(core::SafetyProperty::Call, core::SafetyOutcome::Proven,
                   call, name.str(), "modeled compiler intrinsic");
  return true;
}

bool FunctionDataflow::checkedRuntimeCall(const CallExpr &call,
                                          const CallEffects *effects,
                                          core::AnalysisState &state,
                                          std::string_view name) {
  const auto *model = runtimeModel(name);
  if (!model || !effects || effects->source != SummarySource::Builtin)
    return false;
  if (!runtimeSignature(*model, call, context)) {
    safetyObligation(core::SafetyProperty::Call,
                     core::SafetyOutcome::Unresolved, call, std::string(name),
                     "runtime model requires a compatible C signature");
    return true;
  }
  if (model->family == RuntimeFamily::Format) {
    runtimeFormat(call, name, state);
    safetyObligation(core::SafetyProperty::Call, core::SafetyOutcome::Trusted,
                     call, std::string(name), "modeled C library contract");
    return true;
  }
  safetyObligation(core::SafetyProperty::Call, core::SafetyOutcome::Trusted,
                   call, std::string(name), "modeled C library contract");
  const auto argument = [&](unsigned index) -> const Expr & {
    return *call.getArg(index);
  };
  const auto count = [&](unsigned index) {
    return builder.affineOf(argument(index));
  };
  if (model->family == RuntimeFamily::Numeric)
    return true;
  if (model->family == RuntimeFamily::Compare ||
      model->family == RuntimeFamily::Search ||
      model->family == RuntimeFamily::Span) {
    std::optional<CheckedMemory> first;
    if (name == "memcmp" || name == "memchr") {
      const auto bytes = count(2);
      if (bytes) {
        first = runtimeInterval(argument(0), *bytes, true, false, call, state);
        if (name == "memcmp")
          (void)runtimeInterval(argument(1), *bytes, true, false, call, state);
      } else {
        safetyObligation(
            core::SafetyProperty::Bounds, core::SafetyOutcome::Unresolved, call,
            std::string(name), "runtime byte count is not represented");
      }
    } else {
      const auto limit = name == "strncmp" ? count(2) : std::nullopt;
      if (name == "strncmp" && !limit) {
        safetyObligation(
            core::SafetyProperty::Bounds, core::SafetyOutcome::Unresolved, call,
            std::string(name), "runtime byte count is not represented");
        return true;
      }
      first = runtimeString(argument(0), limit, call, state);
      if (model->parameters[1] == 's')
        (void)runtimeString(argument(1), limit, call, state);
    }
    if (first && model->family == RuntimeFamily::Search)
      if (const auto end = first->end.shifted(-1);
          end && checkedAtMost(first->begin, *end, state))
        checkedPositionPosts[&call].push_back(
            {.path = core::SummaryPath::result(),
             .position = {.storage = first->storage,
                          .offset = first->begin,
                          .extent = first->extent,
                          .input = first->inputPlace},
             .upper = *end,
             .when = {},
             .on = core::Outcome::NonNull});
    return true;
  }
  if (model->family == RuntimeFamily::Open ||
      model->family == RuntimeFamily::Stream) {
    if (name == "puts" || name == "putchar")
      runtimeStandardOutput(call, state);
    for (unsigned index = 0; index < model->parameters.size(); ++index) {
      if (model->parameters[index] == 's')
        (void)runtimeString(argument(index), {}, call, state);
      if (model->parameters[index] == 'f')
        runtimeStream(argument(index), call, state, name == "fflush");
    }
    return true;
  }
  const bool input = model->family == RuntimeFamily::Input;
  const bool descriptor = name == "read" || name == "write";
  const bool line = name == "fgets";
  const unsigned pointer = descriptor ? 1U : 0U;
  std::optional<core::Affine> bytes = descriptor ? count(2) : count(1);
  std::int64_t unit = 1;
  if (!descriptor && !line) {
    const auto size = integerExpressionOf(argument(1), state);
    const auto items = integerExpressionOf(argument(2), state);
    const bool fits = size && items &&
                      operationDoesNotOverflow(core::IntegerOp::Multiply, *size,
                                               *items, size->type(), state);
    safetyObligation(core::SafetyProperty::Arithmetic,
                     core::safetyOutcome(fits, false), call, std::string(name),
                     "runtime element-count product must not overflow");
    if (!fits)
      return true;
    const auto element = bytes ? foldAffine(*bytes, state) : core::Affine{};
    bytes = count(2);
    if (element.isConstant() && element.constant >= 0 && bytes) {
      unit = element.constant;
      bytes = bytes->times(unit);
    } else {
      bytes.reset();
    }
  }
  if (!descriptor)
    runtimeStream(argument(line ? 2U : 3U), call, state);
  if (!bytes) {
    safetyObligation(core::SafetyProperty::Bounds,
                     core::SafetyOutcome::Unresolved, call, std::string(name),
                     "runtime byte count is not represented");
    return true;
  }
  const auto memory =
      runtimeInterval(argument(pointer), *bytes, !input, input, call, state);
  if (input && memory && line) {
    const auto last = bytes->shifted(-1);
    if (last && checkedAtMost(core::Affine::ofConstant(0), *last, state)) {
      checkedPosts[&call].push_back({.path = core::SummaryPath::param(pointer),
                                     .range = {.begin = memory->begin,
                                               .end = memory->end,
                                               .terminatedWithin = true},
                                     .on = core::Outcome::NonNull,
                                     .storage = memory->storage,
                                     .objectType = {}});
      if (const auto end = memory->begin.shifted(1))
        checkedPosts[&call].push_back(
            {.path = core::SummaryPath::param(pointer),
             .range = {.begin = memory->begin, .end = *end},
             .on = core::Outcome::NonNull,
             .storage = memory->storage,
             .objectType = {}});
    }
  }
  if (input && memory && !line)
    if (const auto result = numericCallResult(call)) {
      if (const auto first = memory->begin.shifted(unit))
        checkedPosts[&call].push_back(
            {.path = core::SummaryPath::param(pointer),
             .range = {.begin = memory->begin, .end = *first},
             .on = core::Outcome::Positive,
             .storage = memory->storage,
             .objectType = {}});
      const auto end = core::Affine::ofPlace(*result, unit);
      if (const auto shifted = memory->begin.isConstant()
                                   ? end.shifted(memory->begin.constant)
                                   : std::nullopt)
        checkedPosts[&call].push_back(
            {.path = core::SummaryPath::param(pointer),
             .range = {.begin = memory->begin, .end = *shifted},
             .on = core::Outcome::Positive,
             .storage = memory->storage,
             .objectType = {}});
      const auto maximum = foldAffine(descriptor ? *bytes : *count(2), state);
      if (maximum.isConstant())
        state.relations.learnAtMost(*result, maximum.constant);
      else if (maximum.place && maximum.scale == 1)
        state.relations.learn(*result, core::Relation::LessEqual,
                              *maximum.place, maximum.constant);
    }
  return true;
}

} // namespace weavec::analysis
