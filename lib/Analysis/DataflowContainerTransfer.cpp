//===- DataflowContainerTransfer.cpp - Chain transfer (RFC 0023) ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "IntegerSupport.h"
#include "RuntimeModels.h"

#include <algorithm>
#include <utility>

using namespace clang;

namespace weavec::analysis {

std::optional<FunctionDataflow::PayloadRelocation>
FunctionDataflow::capturePayloadRelocation(const Stmt &stmt,
                                           core::AnalysisState &state) {
  const auto *copy = dyn_cast<BinaryOperator>(&stmt);
  const auto found = payloadRelocationClears.find(copy);
  if (!copy || found == payloadRelocationClears.end() || unsafeBody ||
      unsafeStmts.contains(copy) || unsafeStmts.contains(found->second))
    return std::nullopt;
  const auto *clear = found->second;
  const auto *destination =
      dyn_cast<MemberExpr>(copy->getLHS()->IgnoreParenImpCasts());
  const auto *source =
      dyn_cast<MemberExpr>(copy->getRHS()->IgnoreParenImpCasts());
  const auto *cleared =
      dyn_cast<MemberExpr>(clear->getLHS()->IgnoreParenImpCasts());
  if (!destination || !source || !cleared || !destination->isArrow() ||
      !source->isArrow() || !cleared->isArrow() ||
      !clear->getRHS()->isNullPointerConstant(
          context, Expr::NPC_ValueDependentIsNotNull))
    return std::nullopt;
  const auto base = [](const MemberExpr &member) -> const VarDecl * {
    const auto *ref =
        dyn_cast<DeclRefExpr>(member.getBase()->IgnoreParenImpCasts());
    return ref ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
  };
  const auto *variable = base(*destination);
  const auto *from = dyn_cast<FieldDecl>(source->getMemberDecl());
  const auto *to = dyn_cast<FieldDecl>(destination->getMemberDecl());
  if (!variable || base(*source) != variable || base(*cleared) != variable ||
      !from || !to || from == to || cleared->getMemberDecl() != from ||
      from->getParent() != to->getParent() ||
      variable->getType().isVolatileQualified() ||
      !from->getType()->isPointerType() ||
      !ASTContext::hasSameType(from->getType(), to->getType()) ||
      from->getType().isVolatileQualified() || from->getType()->isAtomicType())
    return std::nullopt;
  const auto holder = builder.placeForVar(*variable);
  const auto *before = state.safety->containers.find(holder);
  const auto memory = checkedMemoryAt(holder, {}, {}, state);
  if (!before || before->empty || !before->allocationCompatible ||
      before->shape.access != core::ContainerAccess::Release ||
      !before->releasedChildren.empty() || !before->releasedPayloads.empty() ||
      !state.nulls.isNonNull(holder) || !memory ||
      memory->begin != core::Affine{} || !checkedValid(*memory, state))
    return std::nullopt;
  const auto payload = [&](const FieldDecl &field) {
    return std::ranges::find_if(before->shape.payloads, [&](const auto &entry) {
      return entry.field.name == field.getName();
    });
  };
  const auto original = payload(*from);
  const auto target = payload(*to);
  if (original == before->shape.payloads.end() ||
      target == before->shape.payloads.end() ||
      original->family != target->family ||
      containerOwns(holder, original->field.name, before->shape, state) !=
          true ||
      containerOwns(holder, target->field.name, before->shape, state) != true)
    return std::nullopt;
  const auto empty = [&](const core::ContainerField &field) {
    return before->shape.emptyPayloads.contains(field.name) ||
           state.nulls.stateOf(places.field(
               places.deref(holder), field.name)) == core::Nullness::Null ||
           containerZeroField(holder, field, state);
  };
  if (!empty(target->field))
    return std::nullopt;
  auto after = *before;
  after.shape.emptyPayloads.insert(original->field.name);
  if (empty(original->field))
    after.shape.emptyPayloads.insert(target->field.name);
  else
    after.shape.emptyPayloads.erase(target->field.name);
  if (!requireContainer(*before, stmt, state))
    return std::nullopt;
  unfoldFootprint(holder, *before, state);
  const auto [saved, inserted] = payloadRelocationSnapshots.try_emplace(copy);
  if (inserted)
    saved->second = {places.create("relocated forest footprint"),
                     places.create("relocated payload footprint")};
  state.safety->footprints.assign(saved->second.first, {{holder, 1}});
  state.safety->footprints.assign(
      saved->second.second,
      {{places.field(places.deref(holder), original->field.name), 1}});
  PayloadRelocation result{.clear = clear,
                           .evaluation = {},
                           .holder = holder,
                           .source = original->field.name,
                           .destination = target->field.name,
                           .snapshot = saved->second,
                           .aliases = {{holder, after}},
                           .separated = {}};
  std::vector<const Stmt *> work{clear};
  while (!work.empty()) {
    const auto *current = work.back();
    work.pop_back();
    if (!current)
      continue;
    result.evaluation.insert(current);
    for (const auto *child : current->children())
      work.push_back(child);
  }
  for (const auto &[other, fact] : state.safety->containers.all()) {
    const auto offset = state.definiteAliases.offsetOf(other, holder);
    const auto otherMemory = checkedMemoryAt(other, {}, {}, state);
    if (other != holder && offset && offset->isZero() &&
        fact.shape == before->shape && fact.empty == before->empty &&
        fact.allocationCompatible && fact.releasedChildren.empty() &&
        fact.releasedPayloads.empty() && otherMemory &&
        otherMemory->storage == memory->storage &&
        otherMemory->begin == memory->begin &&
        checkedValid(*otherMemory, state)) {
      auto updated = fact;
      updated.shape = after.shape;
      result.aliases.emplace(other, std::move(updated));
    } else if (state.safety->containers.separated(holder, other)) {
      result.separated.emplace(other, fact);
    }
  }
  return result;
}

void FunctionDataflow::applyPayloadRelocation(
    const PayloadRelocation &relocation, core::AnalysisState &state) {
  const auto source =
      places.field(places.deref(relocation.holder), relocation.source);
  const auto memory = checkedMemoryAt(relocation.holder, {}, {}, state);
  if (state.nulls.stateOf(source) != core::Nullness::Null || !memory ||
      memory->begin != core::Affine{} || !checkedValid(*memory, state))
    return;
  // These are the same allocations before and after two adjacent pure stores.
  // The temporary duplicate never supplies a container predicate.
  for (const auto &[holder, fact] : relocation.aliases) {
    state.safety->containers.set(holder, fact);
    state.safety->footprints.assign(holder, {{relocation.snapshot.first, 1}});
    state.safety->footprints.assign(
        places.field(places.deref(holder), relocation.source), {});
    const auto destination =
        places.field(places.deref(holder), relocation.destination);
    state.safety->footprints.assign(destination,
                                    {{relocation.snapshot.second, 1}});
    state.safety->footprints.assign(footprintHead(destination),
                                    {{relocation.snapshot.second, 1}});
    for (const auto &field : {relocation.source, relocation.destination})
      if (fact.shape.ownership.contains(field))
        state.safety->footprints.forget(footprintContribution(holder, field));
    state.safety->unfoldedFootprints.erase(holder);
    for (const auto &[other, before] : relocation.separated)
      if (const auto *current = state.safety->containers.find(other);
          current && *current == before)
        state.safety->containers.separate(holder, other);
    snapshotContainerOutput(holder, state);
  }
}

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
  const auto concrete = [&](const core::ContainerFact &fact) {
    return std::ranges::all_of(fact.members, [&](const auto member) {
      return builder.declFor(member) ||
             std::ranges::any_of(checkedObjects, [&](const auto &entry) {
               return entry.second == member;
             });
    });
  };
  if (am && bm && checkedValid(*am, state) && checkedValid(*bm, state) &&
      a->inputs.empty() && b->inputs.empty() && !a->suffix && !b->suffix &&
      concrete(*a) && concrete(*b)) {
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

void FunctionDataflow::captureContainerPrefixes(
    const CallExpr &call, const core::CheckedRequirement &post,
    core::AnalysisState &state) {
  if ((post.kind != core::CheckedRequirementKind::ContainerPreserved &&
       post.kind != core::CheckedRequirementKind::ContainerExtended) ||
      post.path != post.other || !post.path.isParam() || !post.path.isRoot())
    return;
  const auto actual = builder.resolveSummaryPath(post.other, call);
  const auto summary = callSummaries.find(&call);
  if (!actual || summary == callSummaries.end() || !summary->second)
    return;
  const auto *before = state.safety->containers.find(actual->place);
  if (!before || before->empty)
    return;
  std::set<core::PlaceId> ancestors = before->ancestors;
  if (before->tailOf && !before->tailField.empty())
    ancestors.insert(*before->tailOf);
  for (const auto ancestor : ancestors) {
    const auto *prefix = state.safety->containers.find(ancestor);
    const auto alias = state.definiteAliases.offsetOf(ancestor, actual->place);
    if (!prefix || prefix->empty || ancestor == actual->place ||
        (alias && alias->isZero()) || state.moves.recordOf(ancestor) ||
        state.safety->invalidatedPointers.contains(ancestor))
      continue;
    bool confined = true;
    for (const auto &[path, effect] : summary->second->effects) {
      if (effect.escaped) {
        confined = false;
        break;
      }
      if (!effect.written && !effect.consumed())
        continue;
      if (containerEffectCovered(post.other, before->shape, path, effect))
        continue;
      const auto destination = builder.resolveSummaryPath(path, call);
      if (effect.consumed() || !destination ||
          !destination->element.isWhole()) {
        confined = false;
        break;
      }
      auto framed = state;
      bool separate =
          preserveInputContainersAcrossLocalWrite(destination->place, framed);
      if (!separate)
        separate =
            preserveFreshContainersAcrossWrite(destination->place, framed);
      const auto *surviving = framed.safety->containers.find(ancestor);
      if (!separate || !surviving || *surviving != *prefix) {
        confined = false;
        break;
      }
    }
    if (!confined)
      continue;
    const auto [saved, inserted] =
        containerPrefixSnapshots[&call].try_emplace({post.path, ancestor});
    if (inserted)
      saved->second = {places.create("call enclosing prefix footprint"),
                       places.create("call enclosing head footprint")};
    const auto snapshot = saved->second;
    state.safety->footprints.assign(snapshot.first,
                                    {{ancestor, 1}, {actual->place, -1}});
    state.safety->footprints.assign(snapshot.second,
                                    {{footprintHead(ancestor), 1}});
    ContainerPrefix frame{.ancestor = ancestor,
                          .fact = *prefix,
                          .snapshot = snapshot,
                          .separated = {}};
    for (const auto neighbor : state.safety->containers.separatedFrom(ancestor))
      if (const auto *fact = state.safety->containers.find(neighbor))
        frame.separated.emplace(neighbor, *fact);
    containerPrefixPosts[&call][post].push_back(std::move(frame));
  }
}

void FunctionDataflow::applyContainerPrefixes(
    const CallExpr &call, const core::CheckedRequirement &post,
    core::PlaceId holder, core::AnalysisState &state) {
  const auto found = containerPrefixPosts.find(&call);
  if (found == containerPrefixPosts.end())
    return;
  const auto frames = found->second.find(post);
  auto &facts = state.safety->containers;
  const auto *current = facts.find(holder);
  if (frames == found->second.end() || !current || current->empty)
    return;
  auto output = *current;
  for (const auto &frame : frames->second) {
    auto required = frame.fact.shape;
    required.terminal = false;
    required.emptyLinks.clear();
    required.headValues.clear();
    required.emptyPayloads.clear();
    if (!output.entails(required))
      continue;
    auto prefix = frame.fact;
    prefix.members.insert(output.members.begin(), output.members.end());
    prefix.inputs.insert(output.inputs.begin(), output.inputs.end());
    prefix.allocationCompatible &= output.allocationCompatible;
    prefix.localAllocation &= output.localAllocation;
    facts.set(frame.ancestor, std::move(prefix));
    state.safety->pointers.insert(frame.ancestor);
    state.safety->invalidatedPointers.erase(frame.ancestor);
    state.safety->footprints.assign(frame.ancestor,
                                    {{frame.snapshot.first, 1}, {holder, 1}});
    state.safety->footprints.assign(footprintHead(frame.ancestor),
                                    {{frame.snapshot.second, 1}});
    state.safety->unfoldedFootprints.erase(frame.ancestor);
    for (const auto &[neighbor, before] : frame.separated)
      if (const auto *surviving = facts.find(neighbor);
          surviving && *surviving == before &&
          facts.separated(holder, neighbor))
        facts.separate(frame.ancestor, neighbor);
    output.ancestors.insert(frame.ancestor);
    snapshotContainerOutput(frame.ancestor, state);
  }
  facts.set(holder, std::move(output));
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

bool FunctionDataflow::preserveInputContainersAcrossLocalWrite(
    core::PlaceId storage, core::AnalysisState &state) {
  const auto root = places.root(storage);
  const auto *var = builder.varForPlace(root);
  const bool automatic = var != nullptr && var->hasLocalStorage() &&
                         !isa<ParmVarDecl>(var) &&
                         !places.innermostDeref(storage);
  const bool fresh =
      std::ranges::any_of(checkedObjects, [&](const auto &entry) {
        return entry.second == storage;
      });
  if (!automatic && !fresh)
    return false;
  // RFC 0029: a live entry forest predates this local object. Every
  // current member must still have entry provenance; root separation alone
  // cannot frame an attached local node or an unknown callee-produced region.
  // A forest made only of this invocation's heap allocations is likewise
  // disjoint from an automatic object; a fresh destination could be a member.
  std::vector<core::PlaceId> retired;
  for (const auto &[holder, fact] : state.safety->containers.all()) {
    const bool named = holder == root || places.isDescendantOf(holder, root);
    const bool allocated =
        automatic && fact.localAllocation && fact.inputs.empty();
    if (named || (!allocated &&
                  (fact.inputs.empty() ||
                   !std::ranges::all_of(fact.members, [&](const auto member) {
                     return containerInputs.contains(member);
                   }))))
      retired.push_back(holder);
  }
  for (const auto holder : retired) {
    state.safety->containers.block(holder);
    state.safety->footprints.forget(holder);
    state.safety->unfoldedFootprints.erase(holder);
  }
  return true;
}

bool FunctionDataflow::preserveFreshContainersAcrossWrite(
    core::PlaceId cell, core::AnalysisState &state) {
  if (places.step(cell) != core::PathStep::Deref &&
      (places.step(cell) != core::PathStep::Field ||
       !isa_and_nonnull<FieldDecl>(builder.declFor(cell))))
    return false;
  const auto object = places.innermostDeref(cell);
  const auto holder = object ? places.parent(*object) : std::nullopt;
  if (!holder)
    return false;
  const auto memory = checkedMemoryAt(*holder, {}, {}, state);
  if (!memory || !memory->input || !checkedValid(*memory, state) ||
      state.resources.isEscaped(*holder))
    return false;
  // RFC 0029: an unchanged entry object predates all fresh members. Incoming
  // singleton heads additionally need explicit separation from that object;
  // distinct parameter names alone cannot preserve an ownership selector.
  std::vector<core::PlaceId> retired;
  for (const auto &[other, fact] : state.safety->containers.all()) {
    bool separate = fact.localAllocation && fact.inputs.empty();
    std::set<core::PlaceId> entries;
    if (!separate && fact.allocationCompatible && !fact.inputs.empty()) {
      // A derived predicate's non-fresh members belong to these explicit
      // inputs. Its synthetic call region can also contain proved fresh
      // descendants; it is not an additional unknown incoming region.
      separate = std::ranges::all_of(fact.inputs, [&](const auto member) {
        if (const auto input = containerInputs.find(member);
            input != containerInputs.end()) {
          const auto shape = containerInputShapes.find(member);
          if (input->second == *memory->input ||
              shape == containerInputShapes.end() ||
              !shape->second.singletonHead())
            return false;
          entries.insert(member);
          return true;
        }
        return false;
      });
    }
    if (!separate || other == cell || places.isDescendantOf(other, cell)) {
      retired.push_back(other);
    } else if (recording()) {
      for (const auto entry : entries) {
        inferred.checked.require(
            {.kind = core::CheckedRequirementKind::Container,
             .path = containerInputs.at(entry),
             .other = {},
             .family = containerInputShapes.at(entry).encode()});
        inferred.checked.require(
            {.kind = core::CheckedRequirementKind::Separated,
             .path = *memory->input,
             .other = containerInputs.at(entry),
             .family = {}});
      }
    }
  }
  for (const auto other : retired) {
    state.safety->containers.block(other);
    state.safety->footprints.forget(other);
    state.safety->unfoldedFootprints.erase(other);
  }
  return true;
}

void FunctionDataflow::checkedContainerStore(const Stmt &stmt,
                                             core::AnalysisState &state) {
  const auto localValueWrite = [&](const Expr &value) {
    const auto ref = builder.resolve(value);
    if (!ref || !ref->element.isWhole()) {
      const auto memory = checkedLvalue(value, state);
      return memory &&
             preserveInputContainersAcrossLocalWrite(memory->storage, state);
    }
    if (!ref->derefs.empty() || !isLocalStorage(ref->place)) {
      if (const auto memory = checkedLvalue(value, state);
          memory &&
          preserveInputContainersAcrossLocalWrite(memory->storage, state))
        return true;
      return preserveFreshContainersAcrossWrite(ref->place, state);
    }
    const auto *var = builder.varForPlace(places.root(ref->place));
    if (!var || addressTaken.contains(var->getCanonicalDecl())) {
      const auto memory = checkedLvalue(value, state);
      return memory &&
             preserveInputContainersAcrossLocalWrite(memory->storage, state);
    }
    // Changing an unexposed automatic pointer/counter cell cannot change a
    // separate heap object's fields. Retire names reached through that cell;
    // an old exact-node predicate does not survive pointer arithmetic.
    std::vector<core::PlaceId> retired{ref->place};
    for (const auto &[holder, fact] : state.safety->containers.all()) {
      (void)fact;
      if (holder != ref->place && places.isDescendantOf(holder, ref->place))
        retired.push_back(holder);
    }
    for (const auto holder : retired) {
      state.safety->containers.replace(holder);
      state.safety->footprints.forget(holder);
      state.safety->unfoldedFootprints.erase(holder);
    }
    return true;
  };
  if (const auto *increment = dyn_cast<UnaryOperator>(&stmt);
      increment && increment->isIncrementDecrementOp() &&
      increment->getType()->isPointerType()) {
    if (!localValueWrite(*increment->getSubExpr()))
      state.safety->containers.clear();
    return;
  }
  const auto *assignment = dyn_cast<BinaryOperator>(&stmt);
  if (assignment && assignment->isCompoundAssignmentOp() &&
      assignment->getLHS()->getType()->isPointerType()) {
    if (!localValueWrite(*assignment->getLHS()))
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
        !assignment->getLHS()->getType()->isPointerType() &&
        !localValueWrite(*assignment->getLHS()))
      state.safety->containers.clear();
    return;
  }
  const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
  const auto holder = builder.resolvePointerValue(*member->getBase());
  if (!field || !holder || !holder->element.isWhole()) {
    if (const auto memory = checkedLvalue(*assignment->getLHS(), state);
        memory &&
        preserveInputContainersAcrossLocalWrite(memory->storage, state))
      return;
    state.safety->containers.clear();
    return;
  }
  if (!containerShapes.contains(field->getParent()) &&
      !state.safety->containers.find(holder->place))
    if (const auto memory = checkedLvalue(*assignment->getLHS(), state);
        memory &&
        preserveInputContainersAcrossLocalWrite(memory->storage, state))
      return;
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
  if (old && field && old->shape.access != core::ContainerAccess::Release &&
      std::ranges::any_of(old->shape.payloads, [&](const auto &payload) {
        return field->getName() == payload.field.name;
      })) {
    auto owned = old->shape;
    owned.access = core::ContainerAccess::Release;
    owned.family = "free";
    if (const auto strengthened = strengthenContainer(*old, owned)) {
      facts.set(holder, *strengthened);
      old = facts.find(holder);
    }
  }
  if (!candidate && !old) {
    if (const auto memory = checkedMemoryAt(holder, {}, {}, state);
        memory &&
        preserveInputContainersAcrossLocalWrite(memory->storage, state))
      return;
    if (!field || !preserveFreshContainersAcrossWrite(
                      builder.fieldPlace(places.deref(holder), *field), state))
      facts.clear();
    return;
  }
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
  // RFC 0029: an attachment can be the first operation that exposes a
  // concrete ownership forest. Prove that entire graph before resorting to
  // a compositional fold, which needs an already established parent frame.
  // The graph checks initialized links, allocation bases and unique ownership
  // for every reachable member, including zero-initialized calloc fields.
  if (!previous && (linkWrite || payloadWrite) && memory)
    if (const auto concrete = establishContainer(*memory, shape, state)) {
      facts.set(holder, *concrete);
      return;
    }
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
    const auto childField = std::ranges::find(
        shape.children, link->getNameAsString(), &core::ContainerField::name);
    const auto &descriptor =
        childField == shape.children.end() ? shape.link : *childField;
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
    } else if (state.nulls.stateOf(cell) == core::Nullness::Null ||
               containerZeroField(holder, descriptor, state)) {
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
  std::map<core::PlaceId, std::set<core::PlaceId>> prefixSeparation;
  std::vector<core::PlaceId> savedTails;
  std::vector<core::PlaceId> preservedSeparation;
  if (complete) {
    for (const auto &[other, fact] : facts.all()) {
      if (fact.tailOf == holder)
        savedTails.push_back(other);
      auto required = fact.shape;
      required.terminal = false;
      required.emptyLinks.clear();
      required.headValues.clear();
      required.emptyPayloads.clear();
      const auto alias = state.definiteAliases.offsetOf(holder, other);
      const bool sameHead = other == holder || (alias && alias->isZero());
      if (previous && head && !sameHead &&
          !state.safety->invalidatedPointers.contains(other) &&
          !state.moves.recordOf(other) && head->shape.entails(required) &&
          (previous->ancestors.contains(other) ||
           (previous->tailOf == other && !previous->tailField.empty())) &&
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
        for (const auto neighbor : facts.separatedFrom(other))
          if (std::ranges::all_of(children, [&](const auto &child) {
                return child.fact.empty ||
                       facts.separated(child.cell, neighbor);
              }))
            prefixSeparation[other].insert(neighbor);
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
    bool unchangedOwnership = false;
    if (previous && conditionWrite)
      if (const auto condition = shape.ownership.find(payload.field.name);
          condition != shape.ownership.end())
        if (const auto value =
                previous->shape.headValues.find(condition->second.field.name);
            value != previous->shape.headValues.end())
          unchangedOwnership = ((value->second & condition->second.mask) ==
                                condition->second.value) == *active;
    if (*active && !empty &&
        (!previous || (conditionWrite && !unchangedOwnership) ||
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
      // A direct fresh nullable allocation is empty or a separate base. A
      // local copy needs independent live acquisition and storage evidence;
      // its spelling alone cannot fold borrowed or duplicated ownership.
      const bool directAllocation =
          allocation == "malloc" || allocation == "calloc";
      const bool copiedAllocation = origin.kind == ValueOrigin::Kind::Copy &&
                                    origin.place && origin.offset.isZero() &&
                                    bytes &&
                                    !head->members.contains(bytes->storage) &&
                                    checkedValid(*bytes, state);
      if (!previous || !field || field->getName() != payload.field.name ||
          (!directAllocation && !copiedAllocation) || !resource ||
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
  for (const auto &[ancestor, fact] : prefixes) {
    (void)fact;
    head->ancestors.insert(ancestor);
  }
  if (previous && previous->tailOf &&
      head->ancestors.contains(*previous->tailOf)) {
    head->tailOf = previous->tailOf;
    head->tailField = previous->tailField;
  }
  head->empty = false;
  head->suffix = true; // A fold can represent unbounded allocation instances.
  facts.set(holder, *head);
  facts.publish(holder);
  for (const auto other : preservedSeparation)
    facts.separate(holder, other);
  std::vector<core::PlaceId> headAliases{holder};
  for (const auto &[alias, edge] : state.definiteAliases.edgesFrom(holder)) {
    if (alias == holder || !edge.exact() ||
        !state.definiteAliases.sameShare(holder, alias) ||
        state.moves.recordOf(alias) ||
        state.safety->invalidatedPointers.contains(alias))
      continue;
    headAliases.push_back(alias);
    facts.set(alias, *head);
    facts.publish(alias);
    state.safety->footprints.assign(alias, {{holder, 1}});
    state.safety->footprints.assign(footprintHead(alias),
                                    {{footprintHead(holder), 1}});
    state.safety->unfoldedFootprints.erase(alias);
    state.safety->pointers.insert(alias);
    for (const auto separate : facts.separatedFrom(holder))
      facts.separate(alias, separate);
    snapshotContainerOutput(alias, state);
  }
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
        descendant.ancestors.insert(headAliases.begin(), headAliases.end());
        descendant.ancestors.insert(head->ancestors.begin(),
                                    head->ancestors.end());
        // The completed fold proves this exact edge. Retain its proper-child
        // identity when another child slot is subsequently attached; the
        // existing child now overlaps its parent and is no longer a separate
        // forest. Sibling uniqueness was checked before publishing the fold.
        descendant.tailOf = holder;
        descendant.tailField = std::string(places.fieldName(child.cell));
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
    for (const auto neighbor : prefixSeparation[other])
      if (facts.find(neighbor))
        facts.separate(other, neighbor);
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
  if (containerLocalReleases.contains(&call))
    return;
  if (!effects || !effects->summary) {
    state.safety->containers.clear();
    return;
  }
  if (effects->source == SummarySource::Builtin) {
    const auto name = resolvedLibraryName(call);
    if (name == "malloc" || name == "calloc" || name == "strlen" ||
        name == "strnlen")
      return;
    if (const auto *model = runtimeModel(name);
        model && runtimeSignature(*model, call, context)) {
      if (model->family == RuntimeFamily::Compare ||
          model->family == RuntimeFamily::Numeric)
        return;
      if (model->family == RuntimeFamily::ParseNumeric) {
        const auto output = builder.classifyValue(*call.getArg(1));
        if (output.kind == ValueOrigin::Kind::Null ||
            (output.kind == ValueOrigin::Kind::Copy && output.place &&
             output.offset.isZero() &&
             state.nulls.stateOf(output.place->place) == core::Nullness::Null))
          return;
        if (const auto memory = checkedMemory(*call.getArg(1), {}, {}, state);
            memory &&
            preserveInputContainersAcrossLocalWrite(memory->storage, state))
          return;
      }
    }
    if (name == "free" && call.getNumArgs() == 1) {
      const auto origin = builder.classifyValue(*call.getArg(0));
      if (origin.kind == ValueOrigin::Kind::Null ||
          (origin.place &&
           state.nulls.stateOf(origin.place->place) == core::Nullness::Null))
        return;
    }
    bool framed = false;
    if ((name == "memset" || name == "__builtin_memset" ||
         name == "__builtin___memset_chk" || name == "memcpy" ||
         name == "__builtin_memcpy" || name == "__builtin___memcpy_chk" ||
         name == "memmove" || name == "__builtin_memmove" ||
         name == "__builtin___memmove_chk") &&
        call.getNumArgs() > 0)
      if (const auto destination =
              checkedMemory(*call.getArg(0), {}, {}, state))
        framed = preserveInputContainersAcrossLocalWrite(destination->storage,
                                                         state);
    if (!framed)
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
    const bool extends = std::ranges::any_of(
        effects->summary->checked.establishes, [&](const auto &post) {
          return post.kind == core::CheckedRequirementKind::ContainerExtended &&
                 post.path == requirement.path && post.other == post.path &&
                 !post.on && post.when.trivial();
        });
    if (const auto actual =
            builder.resolveSummaryPath(requirement.path, call)) {
      const bool selectorWritten =
          std::ranges::any_of(shape->ownership, [&](const auto &entry) {
            const auto selector =
                requirement.path.deref().field(entry.second.field.name);
            return std::ranges::any_of(
                effects->summary->effects, [&](const auto &operation) {
                  return operation.second.written &&
                         (operation.first == selector ||
                          operation.first.isProperPrefixOf(selector));
                });
          });
      if (selectorWritten)
        for (const auto &[holder, fact] : state.safety->containers.all())
          if (!state.safety->containers.separated(actual->place, holder))
            for (const auto &[name, condition] : fact.shape.ownership) {
              (void)condition;
              state.safety->footprints.forget(
                  footprintContribution(holder, name));
            }
      invalidateContainers(actual->place,
                           shape->access == core::ContainerAccess::Release &&
                               !extends,
                           false, state);
    }
  }
  for (const auto &[path, effect] : effects->summary->effects)
    if ((effect.written || effect.consumed()) &&
        !std::ranges::any_of(handled, [&](const auto &input) {
          return containerEffectCovered(input.first, input.second, path,
                                        effect);
        })) {
      // RFC 0028: writing a private numeric cell cannot change a heap graph.
      // Its scalar facts are invalidated by the normal call effects. Preserve
      // independently established object evidence across configuration calls.
      if (effect.written && !effect.consumed() && path.isGlobal() &&
          !path.hasDeref())
        if (const auto actual = contextPlace(path, state);
            actual && actual->second->isArithmeticType())
          continue;
      if (effect.written && !effect.consumed())
        if (const auto actual = builder.resolveSummaryPath(path, call);
            actual && actual->element.isWhole()) {
          if (preserveInputContainersAcrossLocalWrite(actual->place, state))
            continue;
          if (const auto object = places.innermostDeref(actual->place))
            if (const auto holder = places.parent(*object))
              if (const auto memory = checkedMemoryAt(*holder, {}, {}, state);
                  memory && preserveInputContainersAcrossLocalWrite(
                                memory->storage, state))
                continue;
          if (preserveFreshContainersAcrossWrite(actual->place, state))
            continue;
        }
      state.safety->containers.clear();
      break;
    }
}

} // namespace weavec::analysis
