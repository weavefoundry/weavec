//===- DataflowContainerContracts.cpp - Chain interfaces (RFC 0023) ------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

#include <algorithm>
#include <utility>

using namespace clang;

namespace weavec::analysis {

void FunctionDataflow::snapshotContainerOutput(core::PlaceId holder,
                                               core::AnalysisState &state) {
  const bool hasFact = state.safety->containers.find(holder) != nullptr;
  // Ordinary pointer assignments need no container witness. Existing output
  // witnesses still have to be retired when their holder loses its proof.
  if (!hasFact && containerOutputObjects.empty())
    return;
  const auto path = stableSummaryPathOf(holder);
  if (!path || (path->isParam() && path->isRoot()) ||
      (!path->isParam() && !path->isGlobal()))
    return;
  if (!hasFact) {
    if (const auto existing = containerOutputObjects.find(*path);
        existing != containerOutputObjects.end())
      state.safety->containers.replace(existing->second);
    return;
  }
  const auto [entry, inserted] = containerOutputObjects.try_emplace(*path);
  if (inserted)
    entry->second = places.create("container output witness");
  const auto output = entry->second;
  containerOutputPaths[output] = *path;
  state.safety->containers.replace(output);
  if (const auto *fact = state.safety->containers.find(holder)) {
    state.safety->containers.set(output, *fact);
    for (const auto separate : state.safety->containers.separatedFrom(holder))
      state.safety->containers.separate(output, separate);
  }
}

void FunctionDataflow::containerOutputs(core::CheckedContract &outputs,
                                        const core::AnalysisState &state,
                                        std::optional<core::PlaceId> returned,
                                        std::optional<core::Outcome> outcome) {
  std::map<core::PlaceId, core::SummaryPath> exported;
  for (const auto &[holder, fact] : state.safety->containers.all()) {
    if (!fact.valid() || state.safety->invalidatedPointers.contains(holder) ||
        !fact.releasedPayloads.empty() || state.moves.recordOf(holder) ||
        state.raw.isRaw(holder))
      continue;
    auto path = builder.summaryPathOf(holder);
    if (const auto output = containerOutputPaths.find(holder);
        output != containerOutputPaths.end())
      path = output->second;
    if (holder == returned && outcome == core::Outcome::NonNull)
      path = core::SummaryPath::result();
    if (!path || (path->isParam() && path->isRoot()) ||
        (!path->isResult() && !path->isParam() && !path->isGlobal()))
      continue;
    bool portable = true;
    for (const auto input : fact.inputs) {
      const auto source = containerInputs.find(input);
      if (source == containerInputs.end()) {
        portable = false;
        break;
      }
      inferred.checked.require(
          {.kind = core::CheckedRequirementKind::Container,
           .path = source->second,
           .other = {},
           .family = containerInputShapes.at(input).encode()});
    }
    if (!portable)
      continue;
    auto kind = core::CheckedRequirementKind::Container;
    core::SummaryPath source;
    core::PathAffine second;
    core::PathAffine third;
    std::set<core::SummaryPath> inputPaths;
    for (const auto input : fact.inputs)
      inputPaths.insert(containerInputs.at(input));
    if (fact.localAllocation && fact.inputs.empty() &&
        fact.shape.access == core::ContainerAccess::Release) {
      kind = core::CheckedRequirementKind::ContainerFresh;
    } else if (fact.allocationCompatible && !inputPaths.empty() &&
               inputPaths.size() <= 3) {
      kind = core::CheckedRequirementKind::ContainerDerived;
      auto input = inputPaths.begin();
      source = *input++;
      if (input != inputPaths.end())
        second = core::PathAffine::ofPath(*input++);
      if (input != inputPaths.end())
        third = core::PathAffine::ofPath(*input);
    }
    if (fact.tailOf)
      if (const auto sourceHead = stableSummaryPathOf(*fact.tailOf);
          sourceHead && sourceHead->isParam())
        outputs.establish({.kind = core::CheckedRequirementKind::ContainerTail,
                           .path = *path,
                           .other = *sourceHead,
                           .family = fact.shape.encode(),
                           .on = outcome});
    exported.emplace(holder, *path);
    outputs.establish({.kind = core::CheckedRequirementKind::Container,
                       .path = *path,
                       .other = {},
                       .family = fact.shape.encode(),
                       .on = outcome});
    outputs.establish({.kind = kind,
                       .path = *path,
                       .other = source,
                       .begin = second,
                       .end = third,
                       .family = fact.shape.encode(),
                       .on = outcome});
  }
  for (auto first = exported.begin(); first != exported.end(); ++first)
    for (auto second = std::next(first); second != exported.end(); ++second)
      if (first->second != second->second &&
          state.safety->containers.separated(first->first, second->first))
        outputs.establish(
            {.kind = core::CheckedRequirementKind::ContainerSeparated,
             .path = first->second,
             .other = second->second,
             .family = {},
             .on = outcome});
}

void FunctionDataflow::captureContainerPosts(
    const CallExpr &call, const core::CheckedContract &contract,
    core::AnalysisState &state) {
  auto &tails = containerTailPosts[&call];
  tails.clear();
  auto &separation = containerSeparationPosts[&call];
  separation.clear();
  auto &posts = containerPosts[&call];
  posts.clear();
  if (!contract.complete() || !call.getDirectCallee())
    return;
  const auto arguments = containerArguments.find(&call);
  for (const auto &requirement : contract.requirements)
    if (requirement.kind == core::CheckedRequirementKind::Container &&
        (arguments == containerArguments.end() ||
         !arguments->second.contains(requirement.path)))
      return;
  for (const auto &post : contract.establishes) {
    if (post.kind == core::CheckedRequirementKind::ContainerTail) {
      const auto guard = checkedGuard(post.when, call, state);
      if (guard && guard->trivial())
        tails.push_back(post);
      continue;
    }
    if (post.kind == core::CheckedRequirementKind::ContainerSeparated) {
      const auto guard = checkedGuard(post.when, call, state);
      if (guard && guard->trivial())
        separation.push_back(post);
      continue;
    }
    if (post.kind != core::CheckedRequirementKind::Container &&
        post.kind != core::CheckedRequirementKind::ContainerDerived &&
        post.kind != core::CheckedRequirementKind::ContainerFresh)
      continue;
    const auto shape = core::ContainerShape::decode(post.family);
    const auto guard = checkedGuard(post.when, call, state);
    if (!shape || !guard || !guard->trivial())
      continue;
    core::ContainerFact fact{
        .shape = *shape, .members = {}, .inputs = {}, .suffix = true};
    if (arguments != containerArguments.end())
      for (const auto &[path, input] : arguments->second) {
        (void)path;
        fact.members.insert(input.members.begin(), input.members.end());
        fact.inputs.insert(input.inputs.begin(), input.inputs.end());
      }
    if (post.kind == core::CheckedRequirementKind::ContainerFresh) {
      fact.allocationCompatible = true;
      fact.localAllocation = true;
    } else if (post.kind == core::CheckedRequirementKind::ContainerDerived &&
               arguments != containerArguments.end()) {
      std::vector<core::SummaryPath> sources{post.other};
      if (post.begin.path)
        sources.push_back(*post.begin.path);
      if (post.end.path)
        sources.push_back(*post.end.path);
      std::optional<core::ContainerShape> capability;
      bool compatible = true;
      bool locallyAllocated = true;
      bool allocationCompatible = true;
      auto needed = *shape;
      needed.terminal = false;
      for (const auto &path : sources) {
        const auto source = arguments->second.find(path);
        if (source != arguments->second.end() && source->second.empty)
          continue;
        if (source == arguments->second.end() ||
            !source->second.shape.entails(needed) ||
            source->second.shape.payloads != shape->payloads) {
          compatible = false;
          break;
        }
        auto actual = source->second.shape;
        actual.terminal = shape->terminal;
        if (!capability || capability->entails(actual)) {
          capability = actual;
        } else if (!actual.entails(*capability)) {
          compatible = false;
          break;
        }
        locallyAllocated &= source->second.localAllocation;
        allocationCompatible &= source->second.allocationCompatible;
      }
      if (compatible && !capability) {
        // Empty sources contribute no nodes. Every remaining node in a
        // derived output has the positively established fresh capability.
        capability = *shape;
        if (capability->access != core::ContainerAccess::Release) {
          capability->access = core::ContainerAccess::Release;
          capability->family = "free";
        }
      }
      if (compatible && capability) {
        fact.shape = *capability;
        fact.localAllocation = locallyAllocated;
        fact.allocationCompatible = allocationCompatible;
      } else {
        fact.allocationCompatible = false;
      }
    }
    const auto [object, inserted] = containerCallObjects.try_emplace(&call);
    if (inserted)
      object->second = places.create("container call region");
    fact.members.insert(object->second);
    if (fact.valid())
      posts.push_back(
          {.path = post.path,
           .fact = std::move(fact),
           .on = post.on,
           .fresh = post.kind == core::CheckedRequirementKind::ContainerFresh});
  }
}

void FunctionDataflow::applyContainerPosts(
    const CallExpr &call, core::AnalysisState &state,
    std::optional<core::PlaceId> result) {
  const auto found = containerPosts.find(&call);
  if (found == containerPosts.end())
    return;
  for (const auto &post : found->second) {
    if (post.path.isResult() != result.has_value())
      continue;
    if (post.on && !(result && post.on == core::Outcome::NonNull)) {
      const auto outcome = scalarFactOf(call, state);
      bool allOutcomes = false;
      if (call.getType()->isPointerType())
        allOutcomes =
            std::ranges::any_of(found->second, [&](const auto &other) {
              return other.path == post.path && other.fact == post.fact &&
                     other.on == (post.on == core::Outcome::Null
                                      ? core::Outcome::NonNull
                                      : core::Outcome::Null);
            });
      if (!allOutcomes &&
          (!outcome || !outcome->implies(core::ValueFact::of(*post.on))))
        continue;
    }
    auto holder = result;
    if (holder) {
      for (const auto &step : post.path.steps) {
        holder = places.child(*holder, step.step, step.field);
        if (!holder)
          break;
      }
    } else if (const auto ref = builder.resolveSummaryPath(post.path, call)) {
      holder = ref->place;
    }
    if (!holder)
      continue;
    std::vector<core::PlaceId> previous;
    if (post.fresh)
      for (const auto &[other, fact] : state.safety->containers.all()) {
        // Outputs of one invocation may denote the same fresh allocation.
        // A reused loop call-region identity also supplies no separation.
        const auto region = containerCallObjects.find(&call);
        if (region != containerCallObjects.end() &&
            fact.members.contains(region->second))
          continue;
        previous.push_back(other);
      }
    state.safety->containers.set(*holder, post.fact);
    state.safety->invalidatedPointers.erase(*holder);
    for (const auto other : previous)
      state.safety->containers.separate(*holder, other);
    state.safety->pointers.insert(*holder);
    snapshotContainerOutput(*holder, state);
    if (post.fact.shape.terminal)
      if (const auto *decl =
              dyn_cast_or_null<ValueDecl>(builder.declFor(*holder));
          decl && decl->getType()->isPointerType())
        if (const auto *record =
                decl->getType()->getPointeeType()->getAsRecordDecl())
          for (const auto *field : record->fields())
            if (field->getName() == post.fact.shape.link.name) {
              const auto cell =
                  builder.fieldPlace(places.deref(*holder), *field);
              reinit(cell, state);
              state.nulls.set(cell, {.state = core::Nullness::Null,
                                     .location = {},
                                     .reason = core::NullReason::Declared});
              state.safety->initialized.insert(cell);
              state.safety->pointers.insert(cell);
            }
  }
  if (result && containerReadOnlyCalls.contains(&call))
    if (const auto tails = containerTailPosts.find(&call);
        tails != containerTailPosts.end())
      for (const auto &post : tails->second) {
        if (!post.path.isResult() || !post.path.isRoot() ||
            (post.on && post.on != core::Outcome::NonNull))
          continue;
        const auto source = builder.resolveSummaryPath(post.other, call);
        const auto *existing = state.safety->containers.find(*result);
        if (!source || !existing || source->place == *result)
          continue;
        auto tail = *existing;
        tail.tailOf = source->place;
        state.safety->containers.set(*result, std::move(tail));
      }

  if (const auto separation = containerSeparationPosts.find(&call);
      separation != containerSeparationPosts.end())
    for (const auto &post : separation->second) {
      const bool hasResult = post.path.isResult() || post.other.isResult();
      if (hasResult != result.has_value())
        continue;
      if (post.on && !(hasResult && post.on == core::Outcome::NonNull)) {
        const auto outcome = scalarFactOf(call, state);
        if (!outcome || !outcome->implies(core::ValueFact::of(*post.on)))
          continue;
      }
      const auto resolve =
          [&](const core::SummaryPath &path) -> std::optional<core::PlaceId> {
        if (path.isResult()) {
          auto holder = result;
          for (const auto &step : path.steps) {
            if (!holder)
              return std::nullopt;
            holder = places.child(*holder, step.step, step.field);
          }
          return holder;
        }
        const auto ref = builder.resolveSummaryPath(path, call);
        return ref ? std::optional(ref->place) : std::nullopt;
      };
      const auto first = resolve(post.path);
      const auto second = resolve(post.other);
      if (first && second && state.safety->containers.find(*first) &&
          state.safety->containers.find(*second))
        state.safety->containers.separate(*first, *second);
    }
}

} // namespace weavec::analysis
