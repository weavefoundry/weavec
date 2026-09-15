//===- DataflowContainerTransfer.cpp - Chain transfer (RFC 0023) ---------===//
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

bool FunctionDataflow::separateContainers(core::PlaceId first,
                                          core::PlaceId second, const Stmt &at,
                                          core::AnalysisState &state) {
  auto &facts = state.safety->containers;
  const auto preserveAliases = [&] {
    std::vector<core::PlaceId> firstNames{first};
    std::vector<core::PlaceId> secondNames{second};
    for (const auto &[holder, fact] : facts.all()) {
      (void)fact;
      if (const auto offset = state.definiteAliases.offsetOf(holder, first);
          holder != first && offset && offset->isZero())
        firstNames.push_back(holder);
      if (const auto offset = state.definiteAliases.offsetOf(holder, second);
          holder != second && offset && offset->isZero())
        secondNames.push_back(holder);
    }
    for (const auto a : firstNames)
      for (const auto b : secondNames)
        facts.separate(a, b);
  };
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
      preserveAliases();
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
  preserveAliases();
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
      return !operation.consumed() || shape.recursiveLink(field.field) ||
             std::ranges::any_of(shape.payloads, [&](const auto &payload) {
               return payload.field.name == field.field;
             });
    if (!shape.recursiveLink(field.field)) {
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
  const auto *targetFact = facts.find(holder);
  const auto parent =
      release && !keepTail && targetFact ? targetFact->tailOf : std::nullopt;
  const auto childName = targetFact ? targetFact->tailField : std::string{};
  const auto parentMemory =
      parent ? checkedMemoryAt(*parent, {}, {}, state) : std::nullopt;
  const auto properIndependent = [&](core::PlaceId other,
                                     const core::ContainerFact &fact) {
    return parent && !childName.empty() &&
           (other == *parent ||
            (fact.tailOf == parent && !fact.tailField.empty() &&
             fact.tailField != childName));
  };
  std::vector<core::PlaceId> affected;
  for (const auto &[other, fact] : facts.all()) {
    if (properIndependent(other, fact))
      continue;
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
      if (const auto *fact = facts.find(pointer))
        independent |= properIndependent(pointer, *fact);
      if (parentMemory && memory && memory->storage == parentMemory->storage)
        independent = true;
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
      state.safety->boundedTermination.erase(object);
      state.safety->accessible.erase(object);
      state.safety->writtenStorage.insert(object);
      state.safety->objectTypes[object] = "?";
    }
  }
  for (const auto other : affected) {
    // Entry snapshots and accumulated releases retain the old allocation
    // identities. The current pointer's shape may change even when a helper
    // requires release permission (attachment/detachment also require it).
    // Keeping its old equality would identify a detached live child with the
    // empty new slot when the helper's structural output is unfolded.
    state.safety->footprints.forget(other);
    state.safety->unfoldedFootprints.erase(other);
    if (release)
      for (const auto &[cell, contribution] : footprintContributions)
        if (places.isDescendantOf(cell, other))
          state.safety->footprints.forget(contribution);
    facts.block(other);
  }
  if (parent)
    if (const auto *fact = facts.find(*parent)) {
      auto partial = *fact;
      partial.releasedChildren.insert(childName);
      facts.set(*parent, std::move(partial));
    }
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
  if (const auto *indirect =
          dyn_cast<UnaryOperator>(assignment->getLHS()->IgnoreParenImpCasts());
      indirect && indirect->getOpcode() == UO_Deref) {
    const auto *record = indirect->getType()->getAsRecordDecl();
    const auto holder = builder.resolvePointerValue(*indirect->getSubExpr());
    if (record && holder && holder->element.isWhole()) {
      foldContainerStores(holder->place, *record, nullptr, stmt, state);
      return;
    }
  }
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
  foldContainerStores(holder->place, *field->getParent(), field, stmt, state);
}

void FunctionDataflow::foldContainerStores(core::PlaceId holder,
                                           const RecordDecl &record,
                                           const FieldDecl *field,
                                           const Stmt &stmt,
                                           core::AnalysisState &state) {
  auto &facts = state.safety->containers;
  const auto discovered = containerShapes.find(&record);
  const auto *candidate =
      discovered == containerShapes.end() ? nullptr : &discovered->second;
  const auto *old = facts.find(holder);
  if (!candidate && !old)
    return;
  auto shape = old ? old->shape : *candidate;
  const auto previous = old && field ? std::optional(*old) : std::nullopt;
  const bool linkWrite =
      field == nullptr || shape.recursiveLink(field->getNameAsString());
  const bool payloadWrite =
      std::ranges::any_of(shape.payloads, [&](const auto &payload) {
        return !field || field->getName() == payload.field.name;
      });
  const bool conditionWrite =
      std::ranges::any_of(shape.ownership, [&](const auto &entry) {
        return !field || field->getName() == entry.second.field.name;
      });
  // The old contribution belongs to the pre-store footprint. Retire its
  // current-cell name before refining any predicate under the new selector.
  if (conditionWrite)
    for (const auto &[name, condition] : shape.ownership)
      if (!field || field->getName() == condition.field.name)
        state.safety->footprints.forget(footprintContribution(holder, name));
  if (previous && field && payloadWrite &&
      previous->releasedPayloads.contains(field->getNameAsString()) &&
      state.nulls.stateOf(builder.fieldPlace(places.deref(holder), *field)) ==
          core::Nullness::Null)
    return; // Clearing a consumed slot preserves the remaining live head.
  if (previous && !linkWrite && !payloadWrite && !conditionWrite)
    return;

  const auto memory = checkedMemoryAt(holder, {}, {}, state);
  if (payloadWrite && field && memory && previous &&
      previous->localAllocation && previous->releasedChildren.empty() &&
      previous->releasedPayloads.empty()) {
    // A newly stored payload needs real allocation-base and separation
    // evidence. The concrete graph checks it against every node and payload;
    // its root atom denotes the same already established live head.
    state.safety->footprints.constrain(
        {{footprintAtom(memory->storage), 1}, {footprintHead(holder), -1}});
    if (const auto concrete = establishContainer(*memory, shape, state)) {
      facts.set(holder, *concrete);
      return;
    }
  }
  std::optional<core::ContainerFact> head;
  if (previous && !previous->empty &&
      shape.access != core::ContainerAccess::Read &&
      state.nulls.stateOf(holder) == core::Nullness::NonNull &&
      !state.moves.recordOf(holder)) {
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
      if (const auto resource = state.resources.recordOf(holder);
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
                             facts.unlinked(holder)};
    }
  }

  struct Child {
    core::PlaceId cell;
    core::ContainerFact fact;
    bool owned = true;
    bool retained = false;
  };
  std::vector<Child> children;
  const bool retainConditional =
      previous && field != nullptr && !conditionWrite &&
      shape.recursiveLink(field->getNameAsString()) &&
      previous->releasedChildren.empty() &&
      previous->releasedPayloads.empty() &&
      state.nulls.stateOf(builder.fieldPlace(places.deref(holder), *field)) ==
          core::Nullness::Null;
  bool complete = head.has_value();
  for (const auto *link : record.fields()) {
    if (!shape.recursiveLink(link->getNameAsString()))
      continue;
    const auto cell = builder.fieldPlace(places.deref(holder), *link);
    auto required = shape;
    required.terminal = false;
    required.emptyLinks.clear();
    required.headValues.clear();
    required.emptyPayloads.clear();
    std::optional<core::ContainerFact> tail;
    const auto active =
        containerOwns(holder, link->getNameAsString(), shape, state);
    if (active == false) {
      children.push_back({.cell = cell,
                          .fact = {.shape = required,
                                   .members = {},
                                   .inputs = {},
                                   .empty = true},
                          .owned = false});
      continue;
    }
    if (!active) {
      if (state.nulls.stateOf(cell) == core::Nullness::Null) {
        children.push_back({.cell = cell,
                            .fact = {.shape = required,
                                     .members = {},
                                     .inputs = {},
                                     .empty = true}});
        continue;
      }
      if (retainConditional) {
        // Clearing a different link preserves this guarded contribution.
        // The placeholder grants no pointee access or nullness fact.
        children.push_back({.cell = cell,
                            .fact = {.shape = required,
                                     .members = {},
                                     .inputs = {},
                                     .empty = true},
                            .retained = true});
        continue;
      }
      complete = false;
      continue;
    }
    if (const auto *stored = facts.find(cell)) {
      if (stored->entails(required)) {
        tail = *stored;
      } else if (shape.access == core::ContainerAccess::Release &&
                 stored->shape.object == shape.object &&
                 stored->shape.link == shape.link &&
                 stored->shape.children == shape.children &&
                 stored->shape.ownership == shape.ownership &&
                 stored->releasedChildren.empty() &&
                 stored->releasedPayloads.empty()) {
        // A borrowed child cannot be upgraded by attaching it to an owner.
        tail = *stored;
        if (head) {
          head->shape.access = stored->shape.access;
          head->shape.family = stored->shape.family;
        }
      }
    } else if (state.nulls.stateOf(cell) == core::Nullness::Null) {
      tail = core::ContainerFact{
          .shape = required, .members = {}, .inputs = {}, .empty = true};
    } else if (!linkWrite || link != field) {
      tail = containerAt(cell, required, state);
    } else if (const auto successor = checkedMemoryAt(cell, {}, {}, state)) {
      tail = establishContainer(*successor, required, state);
    }
    const bool disjoint =
        tail && (tail->empty || tail->tailOf == holder ||
                 separateContainers(holder, cell, stmt, state));
    if (!tail || !disjoint) {
      complete = false;
      continue;
    }
    for (const auto &sibling : children)
      if (!tail->empty && !sibling.fact.empty &&
          (tail->tailOf != holder || sibling.fact.tailOf != holder ||
           tail->tailField.empty() || sibling.fact.tailField.empty() ||
           tail->tailField == sibling.fact.tailField) &&
          !separateContainers(cell, sibling.cell, stmt, state))
        complete = false;
    children.push_back({.cell = cell, .fact = *tail});
  }
  std::vector<std::pair<core::PlaceId, core::ContainerFact>> prefixes;
  std::map<core::PlaceId, core::PlaceId> prefixFrames;
  std::vector<core::PlaceId> savedTails;
  std::vector<core::PlaceId> preservedSeparation;
  if (complete) {
    for (const auto &[other, fact] : facts.all()) {
      if (fact.tailOf == holder)
        savedTails.push_back(other);
      if (previous && previous->ancestors.contains(other) &&
          std::ranges::all_of(children, [&](const auto &child) {
            return child.fact.empty || child.fact.tailOf == holder ||
                   separateContainers(other, child.cell, stmt, state);
          })) {
        prefixes.emplace_back(other, fact);
        const auto [saved, inserted] = footprintSnapshots.try_emplace(other);
        if (inserted)
          saved->second = {places.create("saved container footprint"),
                           places.create("saved head footprint")};
        const auto frame = saved->second.first;
        state.safety->footprints.assign(frame, {{other, 1}, {holder, -1}});
        prefixFrames.emplace(other, frame);
      }
      if (facts.separated(holder, other) &&
          std::ranges::all_of(children, [&](const auto &child) {
            return child.fact.empty || facts.separated(child.cell, other);
          }))
        preservedSeparation.push_back(other);
    }
  }
  if (linkWrite || payloadWrite || conditionWrite)
    invalidateContainers(holder, false, true, state);
  if (!complete || !head)
    return;
  if (previous && !requireContainer(*previous, stmt, state))
    return;
  core::FootprintSum footprint{{footprintHead(holder), 1}};
  head->shape.terminal = true;
  head->shape.emptyLinks.clear();
  for (const auto &[name, condition] : shape.ownership) {
    (void)name;
    for (const auto *selector : record.fields())
      if (selector->getName() == condition.field.name)
        if (const auto type = integerTypeOf(*selector, context)) {
          const auto cell = builder.fieldPlace(places.deref(holder), *selector);
          if (const auto value = integerRangeAt(cell, *type, state).constant())
            head->shape.headValues[condition.field.name] = value->bits;
          else if (containerZeroField(holder, condition.field, state))
            head->shape.headValues[condition.field.name] = 0;
          else if (state.numericWrites.contains(cell))
            head->shape.headValues.erase(condition.field.name);
        }
  }
  for (const auto &child : children) {
    if (child.retained) {
      ++footprint[footprintContribution(holder, places.fieldName(child.cell))];
      head->shape.terminal = false;
      continue;
    }
    if (!requireContainer(child.fact, stmt, state))
      return;
    head->members.insert(child.fact.members.begin(), child.fact.members.end());
    head->inputs.insert(child.fact.inputs.begin(), child.fact.inputs.end());
    head->allocationCompatible &=
        child.fact.empty || child.fact.allocationCompatible;
    head->localAllocation &= child.fact.empty || child.fact.localAllocation;
    const bool null = state.nulls.stateOf(child.cell) == core::Nullness::Null ||
                      (child.owned && child.fact.empty);
    head->shape.terminal &= null;
    if (null) {
      state.safety->footprints.assign(child.cell, {});
      head->shape.emptyLinks.insert(std::string(places.fieldName(child.cell)));
    }
    if (shape.ownership.contains(std::string(places.fieldName(child.cell)))) {
      const auto contribution =
          footprintContribution(holder, places.fieldName(child.cell));
      state.safety->footprints.assign(
          contribution, child.owned ? core::FootprintSum{{child.cell, 1}}
                                    : core::FootprintSum{});
      ++footprint[contribution];
    } else {
      ++footprint[child.cell];
    }
  }
  if (head->shape.terminal)
    head->shape.emptyLinks.clear();
  for (const auto &payload : shape.payloads) {
    const auto cell = places.field(places.deref(holder), payload.field.name);
    const auto active = containerOwns(holder, payload.field.name, shape, state);
    if (!active && retainConditional) {
      ++footprint[footprintContribution(holder, payload.field.name)];
      continue;
    }
    if (!active)
      return;
    const bool empty = state.nulls.stateOf(cell) == core::Nullness::Null;
    if (*active && !empty &&
        (!previous || conditionWrite ||
         (field && field->getName() == payload.field.name) ||
         previous->releasedPayloads.contains(payload.field.name))) {
      const auto *assignment = dyn_cast<BinaryOperator>(&stmt);
      const auto origin = assignment
                              ? builder.classifyValue(*assignment->getRHS())
                              : ValueOrigin{};
      const auto allocation =
          origin.call ? resolvedLibraryName(*origin.call) : std::string{};
      const auto resource = state.resources.recordOf(cell);
      const auto bytes = checkedMemoryAt(cell, {}, {}, state);
      // A fresh nullable allocation is either empty or a separate allocation
      // base. A copied pointer, borrowed object or stale field supplies no such
      // separation from the existing inductive object and its other payloads.
      if (!previous || !field || field->getName() != payload.field.name ||
          (allocation != "malloc" && allocation != "calloc") || !resource ||
          resource->origin != core::ResourceOrigin::Allocated ||
          resource->escaped || resource->family != payload.family || !bytes ||
          bytes->begin != core::Affine::ofConstant(0) ||
          state.moves.recordOf(cell) || !footprintHeads.contains(cell))
        return;
      head->members.insert(bytes->storage);
      head->releasedPayloads.erase(payload.field.name);
    }
    if (empty) {
      head->shape.emptyPayloads.insert(payload.field.name);
      state.safety->footprints.constrain({{cell, 1}});
    } else {
      head->shape.emptyPayloads.erase(payload.field.name);
    }
    if (shape.ownership.contains(payload.field.name)) {
      const auto contribution =
          footprintContribution(holder, payload.field.name);
      state.safety->footprints.assign(contribution,
                                      *active ? core::FootprintSum{{cell, 1}}
                                              : core::FootprintSum{});
      ++footprint[contribution];
    } else {
      ++footprint[cell];
    }
  }
  state.safety->footprints.assign(holder, std::move(footprint));
  state.safety->unfoldedFootprints.insert(holder);
  head->tailOf.reset();
  head->tailField.clear();
  head->releasedChildren.clear();
  head->ancestors.clear();
  head->empty = false;
  head->suffix = true; // A fold can represent unbounded allocation instances.
  facts.set(holder, *head);
  facts.publish(holder);
  for (const auto other : preservedSeparation)
    facts.separate(holder, other);
  std::vector<std::pair<core::PlaceId, core::ContainerFact>> descendants;
  for (const auto &child : children) {
    if (!child.owned || child.fact.empty)
      continue;
    for (const auto &[other, fact] : facts.all()) {
      if (other == holder)
        continue;
      const auto alias = state.definiteAliases.offsetOf(other, child.cell);
      if (other == child.cell || (alias && alias->isZero())) {
        auto descendant = fact;
        descendant.ancestors.insert(holder);
        descendants.emplace_back(other, std::move(descendant));
      }
    }
  }
  for (auto &[other, fact] : descendants)
    facts.set(other, std::move(fact));
  for (const auto other : savedTails) {
    // An existing successor remains separate only when no new edge reaches it.
    const auto *saved = facts.find(other);
    if (std::ranges::all_of(children, [&](const auto &child) {
          return child.fact.empty || facts.separated(child.cell, other) ||
                 (saved && saved->tailOf == holder &&
                  child.fact.tailOf == holder && !saved->tailField.empty() &&
                  !child.fact.tailField.empty() &&
                  saved->tailField != child.fact.tailField);
        }))
      facts.separate(holder, other);
  }
  for (auto &[other, fact] : prefixes) {
    for (const auto &child : children) {
      fact.members.insert(child.fact.members.begin(), child.fact.members.end());
      fact.inputs.insert(child.fact.inputs.begin(), child.fact.inputs.end());
    }
    facts.set(other, std::move(fact));
    const auto frame = prefixFrames.at(other);
    state.safety->footprints.assign(other, {{frame, 1}, {holder, 1}});
    state.safety->footprints.forget(frame);
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
    if ((name == "memset" || name == "__builtin_memset" ||
         name == "__builtin___memset_chk") &&
        call.getNumArgs() > 0)
      if (const auto actual = builder.resolvePointerValue(*call.getArg(0)))
        if (const auto *decl =
                dyn_cast_or_null<ValueDecl>(builder.declFor(actual->place)))
          if (const auto *shape = containerShape(decl->getType()))
            if (const auto memory =
                    checkedMemoryAt(actual->place, {}, {}, state))
              if (const auto fact = establishContainer(*memory, *shape, state))
                state.safety->containers.set(actual->place, *fact);
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
}

} // namespace weavec::analysis
