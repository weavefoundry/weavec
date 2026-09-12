//===- DataflowContainerTransfer.cpp - Chain transfer (RFC 0023) ---------===//
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

bool FunctionDataflow::separateContainers(core::PlaceId first,
                                          core::PlaceId second, const Stmt &at,
                                          core::AnalysisState &state) {
  auto &facts = state.safety->containers;
  if (facts.separated(first, second))
    return true;
  const auto *a = facts.find(first);
  const auto *b = facts.find(second);
  if (!a || !b || first == second)
    return false;
  // Distinct explicit storage witnesses are sufficient only when neither
  // proof contains an inductively summarized allocation or input region.
  const auto am = checkedMemoryAt(first, {}, {}, state);
  const auto bm = checkedMemoryAt(second, {}, {}, state);
  if (am && bm && a->inputs.empty() && b->inputs.empty() && !a->suffix &&
      !b->suffix && builder.declFor(am->storage) &&
      builder.declFor(bm->storage)) {
    bool overlap = false;
    for (const auto member : a->members)
      overlap |= b->members.contains(member);
    if (!overlap) {
      facts.separate(first, second);
      return true;
    }
  }
  if (!a->allocationCompatible || !b->allocationCompatible ||
      a->inputs.empty() || b->inputs.empty())
    return false;
  for (const auto member : a->members)
    if (b->members.contains(member))
      return false;
  for (const auto input : a->inputs)
    if (b->inputs.contains(input) || !containerInputs.contains(input))
      return false;
  for (const auto input : b->inputs)
    if (!containerInputs.contains(input))
      return false;
  for (const auto ai : a->inputs)
    for (const auto bi : b->inputs)
      if (containerInputs.at(ai) == containerInputs.at(bi))
        return false;
  if (!requireContainer(*a, at, state) || !requireContainer(*b, at, state))
    return false;
  if (recording())
    for (const auto ai : a->inputs)
      for (const auto bi : b->inputs)
        inferred.checked.require(
            {.kind = core::CheckedRequirementKind::ContainerSeparated,
             .path = containerInputs.at(ai),
             .other = containerInputs.at(bi),
             .family = {}});
  facts.separate(first, second);
  safetyObligation(core::SafetyProperty::Aliasing,
                   core::SafetyOutcome::Required, at, "container separation",
                   "input container footprints must be disjoint");
  return true;
}

static bool containerEffectCovered(const core::SummaryPath &input,
                                   const core::ContainerShape &shape,
                                   const core::SummaryPath &effect,
                                   const core::PlaceEffect &operation) {
  if (input == effect)
    return true;
  if (!input.isProperPrefixOf(effect))
    return false;
  std::size_t index = input.steps.size();
  while (index < effect.steps.size()) {
    if (effect.steps[index++].step != core::PathStep::Deref)
      return false;
    if (index == effect.steps.size())
      return true;
    const auto &field = effect.steps[index++];
    if (field.step != core::PathStep::Field)
      return false;
    if (index == effect.steps.size())
      return !operation.consumed() || field.field == shape.link.name ||
             std::ranges::any_of(shape.payloads, [&](const auto &payload) {
               return payload.field.name == field.field;
             });
    if (field.field != shape.link.name) {
      // The payload certificate owns its allocation, not pointer targets
      // reachable through arbitrary payload contents.
      return false;
    }
  }
  return true;
}

void FunctionDataflow::invalidateContainers(core::PlaceId holder, bool release,
                                            bool keepTail,
                                            core::AnalysisState &state) {
  auto &facts = state.safety->containers;
  std::vector<core::PlaceId> affected;
  for (const auto &[other, fact] : facts.all()) {
    if (facts.separated(holder, other) || (keepTail && fact.tailOf == holder))
      continue;
    affected.push_back(other);
  }
  if (release) {
    const auto *target = facts.find(holder);
    const auto targetMemory = checkedMemoryAt(holder, {}, {}, state);
    std::set<core::PlaceId> candidates = state.safety->pointers;
    for (const auto &[pointer, position] : state.safety->positions) {
      (void)position;
      candidates.insert(pointer);
    }
    for (const auto &[pointer, storage] : state.safety->objects) {
      (void)storage;
      candidates.insert(pointer);
    }
    candidates.insert(affected.begin(), affected.end());
    std::set<core::PlaceId> retired;
    std::set<core::PlaceId> storage;
    for (const auto pointer : candidates) {
      const auto memory = checkedMemoryAt(pointer, {}, {}, state);
      bool independent = facts.separated(holder, pointer);
      if (memory && !independent) {
        const auto *decl =
            dyn_cast_or_null<VarDecl>(builder.declFor(memory->storage));
        independent = builder.isLiteralPlace(memory->storage) ||
                      (decl != nullptr && !decl->getType()->isPointerType());
        // Explicit graph members are exact only before induction/projection.
        // Allocation identities may prove disjointness in that concrete case.
        const bool allocation =
            std::ranges::any_of(checkedObjects, [&](const auto &entry) {
              return entry.second == memory->storage;
            });
        if (allocation && targetMemory && !targetMemory->input) {
          if (keepTail)
            independent |= memory->storage != targetMemory->storage;
          else if (target && target->inputs.empty() && !target->suffix)
            independent |= !target->members.contains(memory->storage);
        }
        for (const auto &[other, fact] : facts.all()) {
          if (!facts.separated(holder, other) &&
              !(keepTail && fact.tailOf == holder))
            continue;
          if (pointer == other) {
            independent = true;
            break;
          }
          const auto preserved = checkedMemoryAt(other, {}, {}, state);
          if (preserved && fact.inputs.empty() && !fact.suffix &&
              memory->storage == preserved->storage)
            independent = true;
          if (fact.inputs.empty() && !fact.suffix &&
              fact.members.contains(memory->storage))
            independent = true;
          if (preserved && preserved->input && memory->input &&
              containerEffectCovered(*preserved->input, fact.shape,
                                     *memory->input,
                                     core::PlaceEffect{.freed = true}))
            independent = true;
        }
      }
      if (independent && std::ranges::find(affected, pointer) == affected.end())
        continue;
      retired.insert(pointer);
      if (memory)
        storage.insert(memory->storage);
    }
    for (const auto pointer : retired) {
      state.safety->invalidatedPointers.insert(pointer);
      state.safety->pointers.erase(pointer);
      state.safety->positions.erase(pointer);
      state.safety->replacedPointers.insert(pointer);
      facts.block(pointer);
    }
    for (const auto object : storage) {
      state.safety->memory.erase(object);
      state.safety->termination.erase(object);
      state.safety->accessible.erase(object);
      state.safety->writtenStorage.insert(object);
      state.safety->objectTypes[object] = "?";
    }
  }
  for (const auto other : affected)
    facts.block(other);
}

void FunctionDataflow::checkedContainerStore(const Stmt &stmt,
                                             core::AnalysisState &state) {
  if (const auto *increment = dyn_cast<UnaryOperator>(&stmt);
      increment && increment->isIncrementDecrementOp() &&
      increment->getType()->isPointerType()) {
    state.safety->containers.clear();
    return;
  }
  const auto *assignment = dyn_cast<BinaryOperator>(&stmt);
  if (assignment && assignment->isCompoundAssignmentOp() &&
      assignment->getLHS()->getType()->isPointerType()) {
    state.safety->containers.clear();
    return;
  }
  if (!assignment || !assignment->isAssignmentOp())
    return;
  const auto *member =
      dyn_cast<MemberExpr>(assignment->getLHS()->IgnoreParenImpCasts());
  if (!member || !member->isArrow()) {
    // Arbitrary byte and indirect stores can corrupt a previously folded
    // object. Direct holder replacement is handled by installCheckedPointer.
    if (!isa<DeclRefExpr>(assignment->getLHS()->IgnoreParenImpCasts()) &&
        !assignment->getLHS()->getType()->isPointerType())
      state.safety->containers.clear();
    return;
  }
  const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
  const auto holder = builder.resolvePointerValue(*member->getBase());
  if (!field || !holder || !holder->element.isWhole()) {
    state.safety->containers.clear();
    return;
  }
  auto &facts = state.safety->containers;
  const auto *candidate = containerShape(member->getBase()->getType());
  const auto *old = facts.find(holder->place);
  if (!candidate && !old)
    return;
  auto shape = old ? old->shape : *candidate;
  const auto previous = old ? std::optional(*old) : std::nullopt;
  const bool linkWrite = field->getName() == shape.link.name;
  const bool payloadWrite =
      std::ranges::any_of(shape.payloads, [&](const auto &payload) {
        return field->getName() == payload.field.name;
      });
  if (previous && !linkWrite && !payloadWrite)
    return;

  const auto memory = checkedMemoryAt(holder->place, {}, {}, state);
  std::optional<core::ContainerFact> head;
  if (previous && !previous->empty &&
      shape.access != core::ContainerAccess::Read &&
      state.nulls.stateOf(holder->place) == core::Nullness::NonNull &&
      !state.moves.recordOf(holder->place)) {
    head = *previous;
  } else if (memory && checkedValid(*memory, state) && memory->extent &&
             memory->begin == core::Affine::ofConstant(0) &&
             checkedInterval({},
                             core::Affine::ofConstant(
                                 static_cast<std::int64_t>(shape.object.bytes)),
                             *memory->extent, state)) {
    auto whole = *memory;
    whole.end =
        core::Affine::ofConstant(static_cast<std::int64_t>(shape.object.bytes));
    bool initialized = checkedWritePermission(whole, state).value_or(false);
    const auto view = state.safety->objectTypes.find(memory->storage);
    initialized &= view != state.safety->objectTypes.end() &&
                   view->second == shape.object.toString();
    for (const auto &part : shape.initialized) {
      auto bytes = *memory;
      bytes.begin =
          core::Affine::ofConstant(static_cast<std::int64_t>(part.offset));
      bytes.end = core::Affine::ofConstant(
          static_cast<std::int64_t>(part.offset + part.bytes));
      initialized &= checkedInitialized(bytes, state);
    }
    if (initialized) {
      if (const auto resource = state.resources.recordOf(holder->place);
          resource && resource->origin == core::ResourceOrigin::Allocated &&
          resource->family == "free" && !resource->escaped) {
        shape.access = core::ContainerAccess::Release;
        shape.family = "free";
      }
      head = core::ContainerFact{
          .shape = shape,
          .members = {memory->storage},
          .inputs = {},
          .allocationCompatible =
              shape.access == core::ContainerAccess::Release,
          .localAllocation = shape.access == core::ContainerAccess::Release &&
                             facts.unlinked(holder->place)};
    }
  }

  const FieldDecl *link = nullptr;
  for (const auto *f : field->getParent()->fields())
    if (f->getName() == shape.link.name)
      link = f;
  const auto cell = link ? std::optional(builder.fieldPlace(
                               places.deref(holder->place), *link))
                         : std::nullopt;
  auto tailShape = shape;
  tailShape.terminal = false;
  if (cell)
    if (const auto *next = facts.find(*cell);
        next && !next->empty && !next->shape.entails(shape) &&
        shape.access == core::ContainerAccess::Release) {
      tailShape.access = next->shape.access;
      tailShape.family = next->shape.family;
      if (head)
        head->shape = tailShape;
    }
  std::optional<core::ContainerFact> tail;
  if (cell) {
    if (const auto *stored = facts.find(*cell);
        stored && stored->entails(tailShape))
      tail = *stored;
    else if (state.nulls.stateOf(*cell) == core::Nullness::Null)
      tail = core::ContainerFact{
          .shape = tailShape, .members = {}, .inputs = {}, .empty = true};
    else if (!linkWrite)
      tail = containerAt(*cell, tailShape, state);
    else if (const auto successor = checkedMemoryAt(*cell, {}, {}, state))
      tail = establishContainer(*successor, tailShape, state);
  }
  // The saved pointer cell inherits whole-chain separation from its RHS.
  bool disjoint =
      tail && (tail->empty || tail->tailOf == holder->place ||
               (cell && separateContainers(holder->place, *cell, stmt, state)));
  std::vector<std::pair<core::PlaceId, core::ContainerFact>> prefixes;
  std::vector<core::PlaceId> savedTails;
  if (head && tail && disjoint) {
    for (const auto &[other, fact] : facts.all()) {
      if (fact.tailOf == holder->place)
        savedTails.push_back(other);
      if (previous && previous->ancestors.contains(other) &&
          (tail->empty ||
           (cell && separateContainers(other, *cell, stmt, state))))
        prefixes.emplace_back(other, fact);
    }
  }
  if (linkWrite || payloadWrite)
    invalidateContainers(holder->place, false, true, state);
  if (!head || !tail || !disjoint || payloadWrite)
    return;
  if (previous && !requireContainer(*previous, stmt, state))
    return;
  if (!requireContainer(*tail, stmt, state))
    return;
  head->members.insert(tail->members.begin(), tail->members.end());
  head->inputs.insert(tail->inputs.begin(), tail->inputs.end());
  head->allocationCompatible &= tail->empty || tail->allocationCompatible;
  head->localAllocation &= tail->empty || tail->localAllocation;
  head->shape.terminal = tail->empty;
  head->tailOf.reset();
  head->ancestors.clear();
  head->empty = false;
  head->suffix = true; // A fold can represent unbounded allocation instances.
  facts.set(holder->place, *head);
  facts.publish(holder->place);
  for (const auto other : savedTails)
    facts.separate(holder->place, other);
  for (auto &[other, fact] : prefixes) {
    fact.members.insert(tail->members.begin(), tail->members.end());
    fact.inputs.insert(tail->inputs.begin(), tail->inputs.end());
    facts.set(other, std::move(fact));
  }
}

void FunctionDataflow::checkedContainersAfterCall(const CallExpr &call,
                                                  const CallEffects *effects,
                                                  core::AnalysisState &state) {
  if (const auto release = containerPayloadReleases.find(&call);
      release != containerPayloadReleases.end()) {
    const auto *head = state.safety->containers.find(release->second);
    const auto saved = head ? std::optional(*head) : std::nullopt;
    invalidateContainers(release->second, false, true, state);
    if (saved)
      state.safety->containers.set(release->second, *saved);
    return;
  }
  if (const auto release = containerReleases.find(&call);
      release != containerReleases.end()) {
    invalidateContainers(release->second, true, true, state);
    return;
  }
  if (!effects || !effects->summary) {
    state.safety->containers.clear();
    return;
  }
  if (effects->source == SummarySource::Builtin) {
    const auto name = resolvedLibraryName(call);
    if (name == "malloc" || name == "calloc" || name == "strlen" ||
        name == "strnlen")
      return;
    if (name == "free" && call.getNumArgs() == 1) {
      const auto origin = builder.classifyValue(*call.getArg(0));
      if (origin.kind == ValueOrigin::Kind::Null ||
          (origin.place &&
           state.nulls.stateOf(origin.place->place) == core::Nullness::Null))
        return;
    }
    state.safety->containers.clear();
    return;
  }
  if (!effects->summary->checked.complete()) {
    state.safety->containers.clear();
    return;
  }
  if (std::ranges::none_of(effects->summary->effects, [](const auto &entry) {
        return entry.second.written || entry.second.consumed();
      }))
    containerReadOnlyCalls.insert(&call);
  std::vector<std::pair<core::SummaryPath, core::ContainerShape>> handled;
  for (const auto &requirement : effects->summary->checked.requirements) {
    if (requirement.kind != core::CheckedRequirementKind::Container)
      continue;
    const auto shape = core::ContainerShape::decode(requirement.family);
    if (!shape || shape->access == core::ContainerAccess::Read)
      continue;
    handled.emplace_back(requirement.path, *shape);
    if (const auto actual = builder.resolveSummaryPath(requirement.path, call))
      invalidateContainers(actual->place,
                           shape->access == core::ContainerAccess::Release,
                           false, state);
  }
  for (const auto &[path, effect] : effects->summary->effects)
    if ((effect.written || effect.consumed()) &&
        !std::ranges::any_of(handled, [&](const auto &input) {
          return containerEffectCovered(input.first, input.second, path,
                                        effect);
        })) {
      state.safety->containers.clear();
      break;
    }
  if (!call.getDirectCallee())
    state.safety->containers.clear();
}

} // namespace weavec::analysis
