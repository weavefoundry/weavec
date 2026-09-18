//===- DataflowRecursiveConstruction.cpp - Fresh recursive outputs -------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "IntegerSupport.h"

#include <algorithm>
#include <utility>

using namespace clang;
namespace weavec::analysis {

static core::SummaryPath
constructionDataPath(const SummaryStore::RecursiveContractGroup &group) {
  const auto root = group.mutableReader ? core::SummaryPath::param(0).deref()
                                        : core::SummaryPath::param(0);
  return group.readerData ? root.field(group.readerData->getNameAsString())
                          : root;
}

static core::SummaryPath
constructionCountPath(const SummaryStore::RecursiveContractGroup &group) {
  const auto root = group.mutableReader ? core::SummaryPath::param(0).deref()
                                        : core::SummaryPath::param(0);
  return group.readerCount ? root.field(group.readerCount->getNameAsString())
                           : core::SummaryPath::param(1);
}

std::shared_ptr<const core::FunctionSummary>
FunctionDataflow::recursiveInputCandidate(const FunctionDecl &callee) {
  const auto *group = summaries.recursiveContractGroup(function);
  if (!group || (!group->constructs && !group->writes))
    return {};
  if (group->writes)
    return recursiveWriterCandidate(callee);
  if (group->extendsHead)
    return recursiveExtensionCandidate(callee);
  const bool outputSlot = callee.getNumParams() == 3;
  const auto type = outputSlot
                        ? callee.getParamDecl(2)->getType()->getPointeeType()
                        : callee.getReturnType();
  const auto *discovered = containerShape(type);
  if (!discovered)
    return {};
  auto shape = *discovered;
  shape.access = core::ContainerAccess::Release;
  shape.family = "free";
  shape.terminal = false;
  shape.emptyLinks.clear();
  shape.emptyPayloads.clear();
  shape.headValues.clear();
  if (!shape.valid())
    return {};
  auto result = std::make_shared<core::FunctionSummary>();
  result->checked.computed = true;
  result->checked.signature = functionTypeKey(callee.getType(), context);
  const auto input = constructionDataPath(*group);
  const auto length = core::PathAffine::ofPath(constructionCountPath(*group));
  result->checked.require({.kind = core::CheckedRequirementKind::Valid,
                           .path = input,
                           .other = {},
                           .family = {}});
  for (const auto kind : {core::CheckedRequirementKind::Extent,
                          core::CheckedRequirementKind::Initialized})
    result->checked.require({.kind = kind,
                             .path = input,
                             .other = {},
                             .end = length,
                             .family = {}});
  result->addEffect(input.deref(), {.read = true});
  if (group->mutableReader) {
    const auto reader = core::SummaryPath::param(0);
    const auto bytes =
        context
            .getTypeSizeInChars(
                callee.getParamDecl(0)->getType()->getPointeeType())
            .getQuantity();
    result->checked.require({.kind = core::CheckedRequirementKind::Valid,
                             .path = reader,
                             .other = {},
                             .family = {}});
    for (const auto kind : {core::CheckedRequirementKind::Extent,
                            core::CheckedRequirementKind::Initialized,
                            core::CheckedRequirementKind::Writable})
      result->checked.require({.kind = kind,
                               .path = reader,
                               .other = {},
                               .end = core::PathAffine::ofConstant(bytes),
                               .family = {}});
    result->checked.require({.kind = core::CheckedRequirementKind::Separated,
                             .path = reader,
                             .other = input,
                             .family = {}});
    result->addEffect(input, {.written = true});
    result->addEffect(constructionCountPath(*group), {.written = true});
  }
  const auto fresh = core::ValueSource::freshAt(
      "free", core::PointerOffset::zero(),
      core::PathAffine::ofConstant(
          static_cast<std::int64_t>(shape.object.bytes)));
  const auto output = outputSlot ? core::SummaryPath::param(2).deref()
                                 : core::SummaryPath::result();
  const auto success =
      outputSlot ? core::Outcome::Positive : core::Outcome::NonNull;
  if (outputSlot) {
    const auto slot = core::SummaryPath::param(2);
    const auto bytes = context.getTypeSizeInChars(type).getQuantity();
    result->checked.require({.kind = core::CheckedRequirementKind::Valid,
                             .path = slot,
                             .other = {},
                             .family = {}});
    for (const auto kind : {core::CheckedRequirementKind::Extent,
                            core::CheckedRequirementKind::Writable})
      result->checked.require({.kind = kind,
                               .path = slot,
                               .other = {},
                               .end = core::PathAffine::ofConstant(bytes),
                               .family = {}});
    result->checked.require({.kind = core::CheckedRequirementKind::Separated,
                             .path = input,
                             .other = slot,
                             .family = {}});
    result->addEffect(output, {.written = true});
    result->addStore({.dest = output, .value = fresh});
    result->addStore({.dest = output, .value = core::ValueSource::null()});
    result->outcomes[core::Outcome::Zero] = {};
    result->outcomes[success] = {};
    result->nullOn[core::Outcome::Zero].insert(output);
    result->nonNullOn[success].insert(output);
  } else {
    result->addReturn(fresh);
    result->addReturn(core::ValueSource::null());
  }
  for (const auto kind : {core::CheckedRequirementKind::Container,
                          core::CheckedRequirementKind::ContainerFresh})
    result->checked.establish({.kind = kind,
                               .path = output,
                               .other = {},
                               .family = shape.encode(),
                               .on = success});
  return result;
}

void FunctionDataflow::initializeRecursiveInput(core::AnalysisState &state) {
  const auto candidate = recursiveInputCandidate(function);
  if (!candidate)
    return;
  const auto &group = *summaries.recursiveContractGroup(function);
  if (group.extendsHead) {
    initializeRecursiveExtension(state);
    return;
  }
  const auto parameter = builder.placeForVar(*function.getParamDecl(0));
  const auto root = group.mutableReader ? places.deref(parameter) : parameter;
  const auto holder =
      group.readerData ? builder.fieldPlace(root, *group.readerData) : root;
  const auto count = group.readerCount
                         ? builder.fieldPlace(root, *group.readerCount)
                         : builder.placeForVar(*function.getParamDecl(1));
  if (group.readerCount && !numericEntryValues.contains(count)) {
    const auto type = integerTypeOf(*group.readerCount, context);
    if (!type)
      return;
    const auto saved = places.create("entry(" + nameOf(count) + ")");
    numericEntryValues.emplace(count, saved);
    state.scalars.set(
        saved, core::ValueFact::ofInteger(core::IntegerRange::full(*type)));
    snapshotPlaces.insert(saved);
    numericSnapshotExpressions.emplace(
        saved, core::IntegerExpression<core::SummaryPath>::input(
                   constructionCountPath(group), *type));
    state.numericValues.insert_or_assign(
        count, NumericExpression::input(saved, *type));
    state.relations.learn(count, core::Relation::Equal, saved);
  }
  const auto saved = numericEntryValues.find(count);
  const auto length = core::Affine::ofPlace(
      saved == numericEntryValues.end() ? count : saved->second);
  if (group.readerData)
    state.safety->positions.insert_or_assign(
        holder, core::PointerPosition{.storage = places.deref(holder),
                                      .offset = {},
                                      .extent = length,
                                      .input = holder});
  const auto memory = checkedMemoryAt(holder, {}, length, state);
  if (!memory || memory->input != constructionDataPath(group) ||
      memory->begin != core::Affine::ofConstant(0))
    return;
  // This is an explicit sufficient entry premise, never an output. Normal
  // stores, calls and alias invalidation retire the dependent memory facts.
  for (const auto &requirement : candidate->checked.requirements)
    inferred.checked.require(requirement);
  state.nulls.set(holder, {.state = core::Nullness::NonNull,
                           .location = {},
                           .reason = core::NullReason::Declared});
  state.safety->pointers.insert(holder);
  state.safety->accessible[memory->storage] = length;
  if (const auto position = state.safety->positions.find(holder);
      position != state.safety->positions.end())
    position->second.extent = length;
  state.safety->initialize(
      memory->storage, {.begin = {}, .end = length, .when = {}, .source = {}});
  if (group.writes)
    initializeRecursiveWriter(state);
  if (group.mutableReader) {
    const auto bytes =
        context
            .getTypeSizeInChars(
                function.getParamDecl(0)->getType()->getPointeeType())
            .getQuantity();
    state.nulls.set(parameter, {.state = core::Nullness::NonNull,
                                .location = {},
                                .reason = core::NullReason::Declared});
    state.safety->pointers.insert(parameter);
    state.safety->accessible[root] = core::Affine::ofConstant(bytes);
    state.safety->initialize(
        root, {.begin = {}, .end = core::Affine::ofConstant(bytes)});
  }
}

std::shared_ptr<const core::FunctionSummary>
FunctionDataflow::recursiveInputCall(const CallExpr &call,
                                     core::AnalysisState &state) {
  const auto *callee = call.getDirectCallee();
  const auto *group = summaries.recursiveContractGroup(function);
  if (!group || (!group->constructs && !group->writes) || !callee ||
      call.getNumArgs() != function.getNumParams() ||
      !summaries.recursiveContractPeer(function, *callee))
    return {};
  if (group->extendsHead)
    return recursiveExtensionCall(call, state);
  auto candidate = recursiveInputCandidate(*callee);
  auto length =
      group->readerCount
          ? builder.affineFromPath(
                core::PathAffine::ofPath(constructionCountPath(*group)), call)
          : integerAffineOf(*call.getArg(1), state);
  if (length)
    length = foldAffine(*length, state);
  const auto root = builder.placeForVar(*function.getParamDecl(0));
  const auto count =
      group->readerCount
          ? builder.fieldPlace(group->mutableReader ? places.deref(root) : root,
                               *group->readerCount)
          : builder.placeForVar(*function.getParamDecl(1));
  const auto saved = numericEntryValues.find(count);
  const auto entry = core::Affine::ofPlace(
      saved == numericEntryValues.end() ? count : saved->second);
  const auto memory = length ? checkedPathMemory(constructionDataPath(*group),
                                                 call, {}, *length, state)
                             : std::nullopt;
  if (!candidate || !length || !memory ||
      memory->input != constructionDataPath(*group) ||
      !checkedAtMost({}, *length, state) ||
      !checkedAtMost({}, memory->begin, state) ||
      !checkedAtMost(memory->end, entry, state) ||
      !checkedAtMost(*length, entry, state))
    return {};
  const auto successor = length->shifted(1);
  const bool strict = successor && checkedAtMost(*successor, entry, state);
  recursiveInputUsed = true;
  recursiveContractCalls.emplace(callee->getCanonicalDecl(), strict);
  const char *reason =
      "recursive forwarding requires progress on every group cycle";
  if (strict)
    reason =
        group->writes
            ? "recursive writer decreases the initialized input interval"
            : "recursive construction decreases the initialized input interval";
  safetyObligation(core::SafetyProperty::Call, core::SafetyOutcome::Required,
                   call, "recursive input interval", reason);
  // resolveCall retains this privately for the ordinary precondition and
  // output transfer machinery. No candidate is installed in SummaryStore.
  return candidate;
}

bool FunctionDataflow::recursiveInputRequirementsCovered(
    const core::FunctionSummary &candidate) {
  const auto &group = *summaries.recursiveContractGroup(function);
  const auto inputPath = constructionDataPath(group);
  const auto countPath = constructionCountPath(group);
  bool verified = true;
  const auto countType =
      group.readerCount
          ? integerTypeOf(*group.readerCount, context)
          : integerTypeOf(function.getParamDecl(1)->getType(), context);
  for (const auto &pre : inferred.checked.requirements) {
    auto unguarded = pre;
    unguarded.when.clear();
    if (candidate.checked.requirements.contains(unguarded))
      continue;
    const unsigned header = group.writes ? 2 : 0;
    if ((group.mutableReader || group.writes) &&
        pre.path == core::SummaryPath::param(header) &&
        (pre.kind == core::CheckedRequirementKind::Extent ||
         pre.kind == core::CheckedRequirementKind::Initialized ||
         pre.kind == core::CheckedRequirementKind::Writable)) {
      const auto bytes =
          context
              .getTypeSizeInChars(
                  function.getParamDecl(header)->getType()->getPointeeType())
              .getQuantity();
      if (pre.begin.isConstant() && pre.end.isConstant() &&
          pre.begin.constant >= 0 && pre.begin.constant <= pre.end.constant &&
          pre.end.constant <= bytes)
        continue;
    }
    bool covered = pre.path == inputPath &&
                   (pre.kind == core::CheckedRequirementKind::Valid ||
                    pre.kind == core::CheckedRequirementKind::Extent ||
                    pre.kind == core::CheckedRequirementKind::Initialized);
    if (covered && pre.kind != core::CheckedRequirementKind::Valid) {
      std::uint64_t minimum = 0;
      if (const auto condition = pre.when.conditions.find(countPath);
          condition != pre.when.conditions.end() && countType)
        if (const auto lower = condition->second.inType(*countType).minimum())
          minimum = lower->bits;
      for (const auto &condition : pre.when.integers)
        if (countType && condition.range &&
            condition.lhs.type() == *countType &&
            condition.lhs.inputKey() == countPath &&
            condition.range->type == *countType)
          if (const auto lower = condition.range->minimum())
            minimum = std::max(minimum, lower->bits);
      const auto coveredEndpoint = [&](const core::PathAffine &bound,
                                       bool upper) {
        if (bound.quantity != core::AffineQuantity::Integer)
          return false;
        auto path = bound.path;
        if (bound.expression) {
          if (!countType || bound.expression->type() != *countType ||
              bound.expression->inputKey() != countPath)
            return false;
          path = countPath;
        }
        if (!path)
          return bound.constant >= 0 &&
                 (!upper || std::cmp_less_equal(bound.constant, minimum));
        return path == countPath && bound.scale == 1 && bound.constant <= 0 &&
               std::uint64_t{0} - static_cast<std::uint64_t>(bound.constant) <=
                   minimum;
      };
      covered &=
          coveredEndpoint(pre.begin, false) && coveredEndpoint(pre.end, true);
    }
    verified &= covered;
  }
  return verified;
}

void FunctionDataflow::verifyRecursiveConstruction() {
  if (const auto *group = summaries.recursiveContractGroup(function);
      group && group->extendsHead) {
    verifyRecursiveExtension();
    return;
  }
  if (!recursiveInputUsed)
    return;
  const auto candidate = recursiveInputCandidate(function);
  const auto &group = *summaries.recursiveContractGroup(function);
  const auto inputPath = constructionDataPath(group);
  const auto countPath = constructionCountPath(group);
  const bool outputSlot = function.getNumParams() == 3;
  bool verified = candidate != nullptr;
  if (outputSlot) {
    const auto output = core::SummaryPath::param(2).deref();
    for (const auto &[outcome, effects] : inferred.outcomes) {
      (void)effects;
      if (outcome == core::Outcome::Zero) {
        const auto nulls = checkedNullOutputClasses.find(outcome);
        verified &= nulls != checkedNullOutputClasses.end() &&
                    nulls->second.contains(output);
      } else if (outcome == core::Outcome::Positive) {
        const auto nonnull = inferred.nonNullOn.find(outcome);
        verified &= nonnull != inferred.nonNullOn.end() &&
                    nonnull->second.contains(output);
      } else {
        verified = false;
      }
    }
  } else {
    verified &=
        inferred.returnsOnlyFresh() && inferred.freshReturnFamily() == "free";
  }
  if (candidate) {
    verified &= recursiveInputRequirementsCovered(*candidate);
    for (const auto &[path, effect] : inferred.effects)
      verified &=
          !path.isGlobal() &&
          (!effect.written ||
           (group.mutableReader && (path == inputPath || path == countPath)) ||
           (outputSlot && path == core::SummaryPath::param(2).deref())) &&
          !effect.consumed() && !effect.escaped && !effect.replaced;
    for (const auto &expected : candidate->checked.establishes) {
      const auto needed = core::ContainerShape::decode(expected.family);
      verified &=
          needed && std::ranges::any_of(
                        inferred.checked.establishes, [&](const auto &post) {
                          const auto actual =
                              core::ContainerShape::decode(post.family);
                          return post.kind == expected.kind &&
                                 post.path == expected.path &&
                                 post.when.trivial() && !post.ifNonNull &&
                                 (!post.on || post.on == expected.on) &&
                                 actual && actual->entails(*needed);
                        });
    }
  }
  safetyObligation(
      core::SafetyProperty::Semantics,
      verified ? core::SafetyOutcome::Proven : core::SafetyOutcome::Unresolved,
      *function.getBody(), "recursive construction output",
      verified ? "recursive construction establishes a fresh initialized forest"
               : "recursive construction does not establish its complete "
                 "output contract");
}

} // namespace weavec::analysis
