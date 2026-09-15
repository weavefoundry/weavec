//===- DataflowFootprints.cpp - Allocation conservation (RFC 0027) --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "IntegerSupport.h"

#include "llvm/Support/raw_ostream.h"

#include <algorithm>

using namespace clang;

namespace weavec::analysis {

bool FunctionDataflow::containerZeroField(core::PlaceId holder,
                                          const core::ContainerField &field,
                                          const core::AnalysisState &state) {
  const auto memory = checkedMemoryAt(holder, {}, {}, state);
  if (!memory || memory->begin != core::Affine::ofConstant(0))
    return false;
  const auto ranges = state.safety->memory.find(memory->storage);
  if (ranges == state.safety->memory.end())
    return false;
  return std::ranges::any_of(ranges->second, [&](const auto &range) {
    auto when = range.when;
    return range.zeroed && !range.source && pruneGuard(when, state) &&
           when.trivial() &&
           checkedAtMost(range.begin,
                         core::Affine::ofConstant(
                             static_cast<std::int64_t>(field.offset)),
                         state) &&
           checkedAtMost(core::Affine::ofConstant(static_cast<std::int64_t>(
                             field.offset + field.bytes)),
                         range.end, state);
  });
}

std::optional<bool>
FunctionDataflow::containerOwns(core::PlaceId holder, std::string_view field,
                                const core::ContainerShape &shape,
                                const core::AnalysisState &state) {
  const auto condition = shape.ownership.find(std::string(field));
  if (condition == shape.ownership.end())
    return true;
  if (containerZeroField(holder, condition->second.field, state))
    return condition->second.value == 0;
  const auto record = containerRecords.find(shape.object.toString());
  if (record == containerRecords.end())
    return std::nullopt;
  for (const auto *selector : record->second->fields()) {
    if (selector->getName() != condition->second.field.name)
      continue;
    const auto type = integerTypeOf(*selector, context);
    if (!type)
      return std::nullopt;
    const auto cell = builder.fieldPlace(places.deref(holder), *selector);
    if (const auto value = integerRangeAt(cell, *type, state).constant())
      return (value->bits & condition->second.mask) == condition->second.value;
    if (!state.numericWrites.contains(cell))
      if (const auto known =
              shape.headValues.find(condition->second.field.name);
          known != shape.headValues.end())
        return (known->second & condition->second.mask) ==
               condition->second.value;
    for (const auto &predicate : state.numericConditions.integers) {
      if (predicate.op != core::IntegerOp::Equal &&
          predicate.op != core::IntegerOp::NotEqual)
        continue;
      for (bool reverse : {false, true}) {
        const auto &bits = reverse ? predicate.rhs : predicate.lhs;
        const auto value =
            (reverse ? predicate.lhs : predicate.rhs).constantValue();
        if (!value || value->bits != condition->second.value ||
            bits.all().back().kind != core::IntegerNodeKind::Operation ||
            bits.all().back().op != core::IntegerOp::BitAnd)
          continue;
        const auto operands = bits.operands();
        for (bool swap : {false, true}) {
          auto input = operands[swap ? 1 : 0];
          const auto mask = operands[swap ? 0 : 1].constantValue();
          while (input.all().back().kind == core::IntegerNodeKind::Convert)
            input = input.operands().front();
          if (mask && mask->bits == condition->second.mask &&
              input.inputKey() == cell)
            return predicate.op == core::IntegerOp::Equal;
        }
      }
    }
  }
  return std::nullopt;
}

core::PlaceId FunctionDataflow::footprintContribution(core::PlaceId holder,
                                                      std::string_view field) {
  const auto cell = places.field(places.deref(holder), field);
  const auto [entry, inserted] = footprintContributions.try_emplace(cell);
  if (inserted)
    entry->second = places.create("conditional owned footprint");
  return entry->second;
}

core::PlaceId FunctionDataflow::footprintHead(core::PlaceId holder) {
  const auto [entry, inserted] = footprintHeads.try_emplace(holder);
  if (inserted)
    entry->second = places.create("container head footprint");
  return entry->second;
}

core::PlaceId FunctionDataflow::footprintAtom(core::PlaceId storage) {
  const auto [entry, inserted] = footprintAtoms.try_emplace(storage);
  if (inserted)
    entry->second = places.create("allocation identity");
  return entry->second;
}

void FunctionDataflow::initializeFootprint(core::PlaceId holder,
                                           const core::SummaryPath &path,
                                           const core::ContainerShape &shape,
                                           core::AnalysisState &state) {
  const auto [entry, inserted] = footprintEntries.try_emplace(path);
  if (inserted)
    entry->second = {.holder = holder,
                     .identity = places.create("entry allocation footprint"),
                     .shape = shape};
  state.safety->footprints.constrain(
      {{holder, 1}, {entry->second.identity, -1}});
}

void FunctionDataflow::unfoldFootprint(core::PlaceId holder,
                                       const core::ContainerFact &fact,
                                       core::AnalysisState &state) {
  auto &relations = state.safety->footprints;
  const auto head = footprintHead(holder);
  if (fact.empty) {
    relations.constrain({{holder, 1}});
    relations.constrain({{head, 1}});
    return;
  }
  if (state.nulls.stateOf(holder) != core::Nullness::NonNull ||
      !fact.releasedChildren.empty() || !fact.releasedPayloads.empty() ||
      state.safety->unfoldedFootprints.contains(holder)) {
    if (state.nulls.stateOf(holder) == core::Nullness::NonNull)
      for (const auto &[name, condition] : fact.shape.ownership) {
        (void)condition;
        if (fact.releasedChildren.contains(name) ||
            fact.releasedPayloads.contains(name))
          continue;
        const auto cell = places.field(places.deref(holder), name);
        const auto owned = footprintContribution(holder, name);
        if (state.nulls.stateOf(cell) == core::Nullness::Null) {
          relations.constrain({{owned, 1}});
        } else if (const auto active =
                       containerOwns(holder, name, fact.shape, state)) {
          if (*active)
            relations.constrain({{owned, 1}, {cell, -1}});
          else
            relations.constrain({{owned, 1}});
        }
      }
    return;
  }
  core::FootprintSum split{{holder, 1}, {head, -1}};
  const auto add = [&](const std::string &name) {
    const auto cell = places.field(places.deref(holder), name);
    if ((fact.shape.terminal && fact.shape.recursiveLink(name)) ||
        fact.shape.emptyLinks.contains(name) ||
        state.nulls.stateOf(cell) == core::Nullness::Null)
      relations.constrain({{cell, 1}});
    if (fact.shape.ownership.contains(name)) {
      const auto owned = footprintContribution(holder, name);
      if (state.nulls.stateOf(cell) == core::Nullness::Null) {
        relations.constrain({{owned, 1}});
      } else if (const auto active =
                     containerOwns(holder, name, fact.shape, state)) {
        if (*active)
          relations.constrain({{owned, 1}, {cell, -1}});
        else
          relations.constrain({{owned, 1}});
      }
      --split[owned];
    } else {
      --split[cell];
    }
  };
  add(fact.shape.link.name);
  for (const auto &child : fact.shape.children)
    add(child.name);
  for (const auto &payload : fact.shape.payloads)
    add(payload.field.name);
  relations.constrain(std::move(split));
  state.safety->unfoldedFootprints.insert(holder);
}

void FunctionDataflow::captureFootprint(core::PlaceId dest,
                                        const ValueOrigin &origin,
                                        CheckedPointer &pointer,
                                        core::AnalysisState &state) {
  const auto *decl = dyn_cast_or_null<ValueDecl>(builder.declFor(dest));
  const bool freshContainer = pointer.fresh && decl != nullptr &&
                              containerShape(decl->getType()) != nullptr;
  const auto library =
      origin.call ? resolvedLibraryName(*origin.call) : std::string{};
  const bool freshAllocation = pointer.fresh && footprintAllocated &&
                               (library == "malloc" || library == "calloc");
  if (!pointer.container && !freshContainer && !freshAllocation &&
      !footprintHeads.contains(dest) &&
      !(origin.place && footprintHeads.contains(origin.place->place)))
    return;
  if (origin.kind == ValueOrigin::Kind::Conditional) {
    const auto allocated = std::ranges::count_if(
        origin.alternatives, [&](const auto &alternative) {
          return alternative.kind == ValueOrigin::Kind::Alloc &&
                 alternative.call == origin.call;
        });
    const auto nonNull =
        std::ranges::count_if(origin.alternatives, [](const auto &alternative) {
          return alternative.kind != ValueOrigin::Kind::Null;
        });
    // A nullable fresh call creates no allocation on its null outcome.
    // A conditional copy of an existing pointer has no such property.
    if (origin.call && allocated == 1 && nonNull == 1)
      return;
    if (footprintAllocated &&
        std::ranges::any_of(origin.alternatives, [](const auto &alternative) {
          return alternative.kind == ValueOrigin::Kind::Alloc;
        }))
      state.safety->footprints.forget(*footprintAllocated);
    pointer.footprint.reset();
    if (const auto saved = footprintSnapshots.find(dest);
        saved != footprintSnapshots.end()) {
      state.safety->footprints.forget(saved->second.first);
      state.safety->footprints.forget(saved->second.second);
    }
    return;
  }
  if (places.step(dest) == core::PathStep::Field) {
    const auto object = places.parent(dest);
    const auto parent = object && places.step(*object) == core::PathStep::Deref
                            ? places.parent(*object)
                            : std::nullopt;
    if (parent)
      if (const auto *fact = state.safety->containers.find(*parent))
        unfoldFootprint(*parent, *fact, state);
  }
  const auto [saved, inserted] = footprintSnapshots.try_emplace(dest);
  if (inserted)
    saved->second = {places.create("saved container footprint"),
                     places.create("saved head footprint")};
  const auto [whole, head] = saved->second;
  auto &relations = state.safety->footprints;
  relations.forget(whole);
  relations.forget(head);
  if (origin.kind == ValueOrigin::Kind::Null ||
      (pointer.container && pointer.container->empty)) {
    relations.assign(whole, {});
    relations.assign(head, {});
  } else if (pointer.fresh && pointer.storage) {
    // An allocation site can run repeatedly. Retiring its current atom keeps
    // equations among older snapshots without identifying two live instances.
    const auto atom = footprintAtom(*pointer.storage);
    relations.forget(atom);
    relations.assign(head, {{atom, 1}});
    if (freshAllocation) {
      relations.assign(whole, {{atom, 1}});
      relations.assign(*footprintAllocated,
                       {{*footprintAllocated, 1}, {atom, 1}});
    }
  } else if (origin.place && origin.offset.isZero()) {
    const auto source = origin.place->place;
    if (pointer.container)
      unfoldFootprint(source, *pointer.container, state);
    relations.assign(whole, {{source, 1}});
    relations.assign(head, {{footprintHead(source), 1}});
  }
  pointer.footprint = saved->second;
}

void FunctionDataflow::installFootprint(core::PlaceId dest,
                                        const CheckedPointer &pointer,
                                        core::AnalysisState &state) {
  auto &relations = state.safety->footprints;
  // Head identities below a replaced pointer name belong to its old object.
  // Ghost places are not ordinary descendants retired by reinitialization.
  for (const auto &[holder, head] : footprintHeads)
    if (holder != dest && places.isDescendantOf(holder, dest)) {
      relations.forget(head);
      state.safety->unfoldedFootprints.erase(holder);
    }
  for (const auto &[cell, owned] : footprintContributions)
    if (cell == dest || places.isDescendantOf(cell, dest))
      relations.forget(owned);
  if (!pointer.footprint) {
    if (footprintHeads.contains(dest)) {
      relations.forget(dest);
      relations.forget(footprintHeads.at(dest));
    }
    return;
  }
  const auto [whole, head] = *pointer.footprint;
  relations.assign(dest, {{whole, 1}});
  relations.assign(footprintHead(dest), {{head, 1}});
  relations.forget(whole);
  relations.forget(head);
}

void FunctionDataflow::releaseFootprint(core::PlaceId holder, bool whole,
                                        core::AnalysisState &state) {
  if (!footprintReleased)
    return;
  if (const auto *fact = state.safety->containers.find(holder))
    unfoldFootprint(holder, *fact, state);
  state.safety->footprints.assign(
      *footprintReleased,
      {{*footprintReleased, 1}, {whole ? holder : footprintHead(holder), 1}});
}

void FunctionDataflow::footprintOutputs(core::CheckedContract &outputs,
                                        const core::AnalysisState &state,
                                        std::optional<core::PlaceId> returned,
                                        std::optional<core::Outcome> outcome) {
  if (!footprintReleased)
    return;
  if (state.safety->footprints.limited()) {
    safetyObligation(core::SafetyProperty::Semantics,
                     core::SafetyOutcome::Unresolved, *function.getBody(),
                     "container footprint",
                     "allocation footprint relation limit reached");
    return;
  }
  const auto &relations = state.safety->footprints;
  if (options.dumpStream && recording()) {
    auto &out = *options.dumpStream;
    out << "  footprint exit '" << function.getNameAsString()
        << "' outcome=" << (outcome ? core::toString(*outcome) : "void")
        << " returned=" << (returned ? places.name(*returned) : "none")
        << ":\n";
    for (const auto &row : relations.all()) {
      out << "    ";
      for (const auto &[place, coefficient] : row)
        out << coefficient << "*" << places.name(place) << "#" << place.value
            << " ";
      out << "= 0\n";
    }
  }
  std::map<core::PlaceId, core::SummaryPath> visible;
  for (const auto &[holder, fact] : state.safety->containers.all()) {
    if (!fact.releasedChildren.empty() || !fact.releasedPayloads.empty() ||
        state.safety->invalidatedPointers.contains(holder) ||
        state.moves.recordOf(holder) || state.raw.isRaw(holder))
      continue;
    auto path = stableSummaryPathOf(holder);
    if (holder == returned)
      path = core::SummaryPath::result();
    if (const auto output = containerOutputPaths.find(holder);
        output != containerOutputPaths.end())
      path = output->second;
    if (path && (path->isResult() || path->isParam() || path->isGlobal()))
      visible.emplace(holder, *path);
  }
  std::map<core::SummaryPath, core::ContainerShape> premises;
  for (const auto &requirement : inferred.checked.requirements) {
    if (requirement.kind != core::CheckedRequirementKind::Container ||
        !requirement.when.trivial())
      continue;
    const auto required = core::ContainerShape::decode(requirement.family);
    const auto current = premises.find(requirement.path);
    if (required && (current == premises.end() ||
                     required->access > current->second.access))
      premises.insert_or_assign(requirement.path, *required);
  }
  core::FootprintSum balance{{*footprintReleased, -1}};
  if (footprintAllocated)
    ++balance[*footprintAllocated];
  std::vector<core::SummaryPath> ownedInputs;
  for (const auto &[path, entry] : footprintEntries) {
    auto premise = premises.find(path);
    if (premise == premises.end() &&
        (relations.equal(*footprintReleased, entry.identity) ||
         (returned && relations.equal(*returned, entry.identity))))
      premise = premises.emplace(path, entry.shape).first;
    if (premise == premises.end())
      continue;
    const auto &shape = premise->second;
    inferred.checked.require({.kind = core::CheckedRequirementKind::Container,
                              .path = path,
                              .other = {},
                              .family = shape.encode()});
    if (shape.access == core::ContainerAccess::Release) {
      ++balance[entry.identity];
      ownedInputs.push_back(path);
      if (relations.equal(*footprintReleased, entry.identity))
        outputs.establish(
            {.kind = core::CheckedRequirementKind::ContainerConsumed,
             .path = path,
             .other = path,
             .family = shape.encode(),
             .on = outcome});
    }
    for (auto first = visible.begin(); first != visible.end(); ++first) {
      if (relations.equal(first->first, entry.identity))
        outputs.establish(
            {.kind = core::CheckedRequirementKind::ContainerPreserved,
             .path = first->second,
             .other = path,
             .family = shape.encode(),
             .on = outcome});
      for (auto second = std::next(first); second != visible.end(); ++second) {
        if (first->second == second->second ||
            !state.safety->containers.separated(first->first, second->first))
          continue;
        core::FootprintSum partition{{entry.identity, -1}};
        ++partition[first->first];
        ++partition[second->first];
        if (relations.entails(std::move(partition))) {
          outputs.establish(
              {.kind = core::CheckedRequirementKind::ContainerPartition,
               .path = first->second,
               .other = second->second,
               .begin = core::PathAffine::ofPath(path),
               .family = shape.encode(),
               .on = outcome});
          outputs.establish(
              {.kind = core::CheckedRequirementKind::ContainerSeparated,
               .path = first->second,
               .other = second->second,
               .family = {},
               .on = outcome});
        }
      }
    }
  }
  for (std::size_t i = 0; i < ownedInputs.size(); ++i)
    for (std::size_t j = i + 1; j < ownedInputs.size(); ++j)
      inferred.checked.require(
          {.kind = core::CheckedRequirementKind::ContainerSeparated,
           .path = ownedInputs[i],
           .other = ownedInputs[j],
           .family = {}});
  for (auto first = footprintEntries.begin(); first != footprintEntries.end();
       ++first)
    for (auto second = std::next(first); second != footprintEntries.end();
         ++second) {
      const auto a = premises.find(first->first);
      const auto b = premises.find(second->first);
      if (a == premises.end() || b == premises.end() ||
          a->second.object != b->second.object ||
          a->second.link != b->second.link ||
          a->second.children != b->second.children ||
          a->second.ownership != b->second.ownership)
        continue;
      auto combinedShape = a->second;
      combinedShape.terminal = false;
      combinedShape.emptyLinks.clear();
      combinedShape.headValues.clear();
      combinedShape.emptyPayloads.clear();
      if (b->second.access < combinedShape.access) {
        combinedShape.access = b->second.access;
        combinedShape.family = b->second.family;
        combinedShape.payloads = b->second.payloads;
      }
      for (const auto &[holder, output] : visible)
        if (relations.entails({{holder, 1},
                               {first->second.identity, -1},
                               {second->second.identity, -1}})) {
          inferred.checked.require(
              {.kind = core::CheckedRequirementKind::ContainerSeparated,
               .path = first->first,
               .other = second->first,
               .family = {}});
          outputs.establish(
              {.kind = core::CheckedRequirementKind::ContainerCombined,
               .path = output,
               .other = first->first,
               .begin = core::PathAffine::ofPath(second->first),
               .family = combinedShape.encode(),
               .on = outcome});
        }
    }
  // Entry release permission does not transfer the caller's cleanup duty.
  // Without local acquisitions, a partial helper can be conditionally safe;
  // it exports no consumption promise unless the whole entry equality holds.
  // Callers must still settle their own acquired footprint, and recursive
  // consumption candidates are independently checked on every exit.
  bool accounted = relations.entails(balance) ||
                   (footprintAllocated && relations.empty(*footprintAllocated));
  for (auto first = visible.begin(); first != visible.end() && !accounted;
       ++first) {
    auto remaining = balance;
    --remaining[first->first];
    accounted = relations.entails(remaining);
    for (auto second = std::next(first); second != visible.end() && !accounted;
         ++second) {
      if (!state.safety->containers.separated(first->first, second->first))
        continue;
      auto partition = remaining;
      --partition[second->first];
      accounted = relations.entails(std::move(partition));
    }
  }
  safetyObligation(
      core::SafetyProperty::Semantics,
      accounted ? core::SafetyOutcome::Proven : core::SafetyOutcome::Unresolved,
      *function.getBody(), "container footprint",
      accounted
          ? "owned allocation footprint is preserved, transferred or released"
          : "container operation loses part of the owned allocation footprint");
}

void FunctionDataflow::captureFootprintPosts(
    const CallExpr &call, const core::CheckedContract &contract,
    core::AnalysisState &state) {
  auto &posts = footprintPosts[&call];
  posts.clear();
  if (!contract.complete())
    return;
  const auto arguments = containerArguments.find(&call);
  auto &inputs = footprintCallInputs[&call];
  if (footprintAllocated && containerCallObjects.contains(&call) &&
      std::ranges::any_of(contract.establishes, [&](const auto &post) {
        const auto guard = checkedGuard(post.when, call, state);
        return post.kind == core::CheckedRequirementKind::ContainerFresh &&
               post.path.isResult() && post.path.isRoot() &&
               (!post.on || post.on == core::Outcome::NonNull) && guard &&
               guard->trivial();
      })) {
    const auto region = containerCallObjects.at(&call);
    state.safety->footprints.forget(region);
    state.safety->footprints.assign(*footprintAllocated,
                                    {{*footprintAllocated, 1}, {region, 1}});
  }
  const auto capture = [&](const core::SummaryPath &path) {
    if (arguments == containerArguments.end() ||
        !arguments->second.contains(path))
      return false;
    if (arguments->second.at(path).empty) {
      const auto [saved, inserted] = inputs.try_emplace(path);
      if (inserted)
        saved->second = places.create("call entry allocation footprint");
      state.safety->footprints.assign(saved->second, {});
      return true;
    }
    const auto actual = builder.resolveSummaryPath(path, call);
    std::optional<core::PlaceId> source;
    if (actual) {
      source = actual->place;
    } else if (path.isParam() && path.isRoot() &&
               path.index < call.getNumArgs()) {
      const auto *nested =
          dyn_cast<CallExpr>(call.getArg(path.index)->IgnoreParenCasts());
      if (const auto region = containerCallObjects.find(nested);
          nested && region != containerCallObjects.end())
        source = region->second;
    }
    if (!source)
      return false;
    const auto [saved, inserted] = inputs.try_emplace(path);
    if (inserted)
      saved->second = places.create("call entry allocation footprint");
    if (actual)
      unfoldFootprint(actual->place, arguments->second.at(path), state);
    state.safety->footprints.assign(saved->second, {{*source, 1}});
    return true;
  };
  for (const auto &post : contract.establishes) {
    if (post.kind != core::CheckedRequirementKind::ContainerPreserved &&
        post.kind != core::CheckedRequirementKind::ContainerConsumed &&
        post.kind != core::CheckedRequirementKind::ContainerPartition &&
        post.kind != core::CheckedRequirementKind::ContainerCombined)
      continue;
    const auto guard = checkedGuard(post.when, call, state);
    if (!guard || !guard->trivial())
      continue;
    bool captured = false;
    if (post.kind == core::CheckedRequirementKind::ContainerPartition)
      captured = post.begin.path && capture(*post.begin.path);
    else
      captured = capture(post.other);
    if (post.kind == core::CheckedRequirementKind::ContainerCombined)
      captured &= post.begin.path && capture(*post.begin.path);
    if (captured)
      posts.push_back(post);
  }
}

void FunctionDataflow::applyFootprintPosts(
    const CallExpr &call, core::AnalysisState &state,
    std::optional<core::PlaceId> result) {
  const auto region = containerCallObjects.find(&call);
  const auto structural = containerPosts.find(&call);
  if (result && region != containerCallObjects.end() &&
      structural != containerPosts.end() &&
      std::ranges::any_of(structural->second, [](const auto &post) {
        return post.fresh && post.path.isResult() && post.path.isRoot();
      }))
    state.safety->footprints.assign(*result, {{region->second, 1}});
  const auto found = footprintPosts.find(&call);
  if (found == footprintPosts.end())
    return;
  const auto resolve =
      [&](const core::SummaryPath &path) -> std::optional<core::PlaceId> {
    if (path.isResult()) {
      auto holder = result;
      if (!holder && path.isRoot() && region != containerCallObjects.end())
        holder = region->second;
      for (const auto &step : path.steps) {
        if (!holder)
          return std::nullopt;
        holder = places.child(*holder, step.step, step.field);
      }
      return holder;
    }
    const auto actual = builder.resolveSummaryPath(path, call);
    return actual ? std::optional(actual->place) : std::nullopt;
  };
  for (const auto &post : found->second) {
    const bool partition =
        post.kind == core::CheckedRequirementKind::ContainerPartition;
    const bool hasResult =
        post.path.isResult() || (partition && post.other.isResult());
    const bool nested =
        hasResult && !result && region != containerCallObjects.end();
    if (!nested && hasResult != result.has_value())
      continue;
    if (post.on) {
      const auto outcome = scalarFactOf(call, state);
      const bool resultMatches =
          result &&
          ((post.on == core::Outcome::NonNull &&
            state.nulls.stateOf(*result) == core::Nullness::NonNull) ||
           (post.on == core::Outcome::Null &&
            state.nulls.stateOf(*result) == core::Nullness::Null));
      if (!resultMatches &&
          (!outcome || !outcome->implies(core::ValueFact::of(*post.on))))
        continue;
    }
    const auto &inputs = footprintCallInputs.at(&call);
    if (post.kind == core::CheckedRequirementKind::ContainerConsumed) {
      releaseFootprint(inputs.at(post.other), true, state);
      if (const auto actual = builder.resolveSummaryPath(post.path, call))
        state.resources.clear(actual->place);
      continue;
    }
    const auto holder = resolve(post.path);
    if (!holder)
      continue;
    if (partition) {
      const auto other = resolve(post.other);
      if (!other || *holder == *other || !post.begin.path ||
          !state.safety->containers.separated(*holder, *other))
        continue;
      core::FootprintSum relation{{inputs.at(*post.begin.path), -1}};
      ++relation[*holder];
      ++relation[*other];
      state.safety->footprints.constrain(std::move(relation));
    } else if (nested || state.safety->containers.find(*holder)) {
      core::FootprintSum footprint{{inputs.at(post.other), 1}};
      if (post.kind == core::CheckedRequirementKind::ContainerCombined &&
          post.begin.path)
        ++footprint[inputs.at(*post.begin.path)];
      state.safety->footprints.assign(*holder, std::move(footprint));
    }
  }
}

bool FunctionDataflow::handleRecursiveCleanup(const CallExpr &call,
                                              core::AnalysisState &state) {
  const auto *callee = call.getDirectCallee();
  if (!callee || callee->getCanonicalDecl() != function.getCanonicalDecl() ||
      function.getNumParams() != 1 || call.getNumArgs() != 1 ||
      !function.getReturnType()->isVoidType())
    return false;
  const auto *shape = containerShape(function.getParamDecl(0)->getType());
  if (!shape || shape->access != core::ContainerAccess::Release)
    return false;
  if (!recursiveCleanupCandidate) {
    // The initial induction rule has no hidden global effects or outputs.
    // Additional callbacks/mutators need their ordinary verified contracts.
    bool candidate = true;
    std::vector<const Stmt *> work{function.getBody()};
    for (std::size_t i = 0; i < work.size() && work.size() <= 65536; ++i) {
      const auto *stmt = work[i];
      if (!stmt)
        continue;
      if (const auto *operation = dyn_cast<CallExpr>(stmt)) {
        const auto *target = operation->getDirectCallee();
        const auto ref =
            !target ? builder.resolve(*operation->getCallee()) : std::nullopt;
        const auto path =
            ref ? builder.summaryPathOf(ref->place) : std::nullopt;
        const auto binding =
            path ? callbackBindings.find(*path) : callbackBindings.end();
        const bool actualFree =
            binding != callbackBindings.end() && !binding->second.unknown &&
            !binding->second.null &&
            binding->second.functions == std::set<std::string>{"free"} &&
            operation->getNumArgs() == 1;
        candidate &=
            actualFree ||
            (target != nullptr &&
             (target->getCanonicalDecl() == function.getCanonicalDecl() ||
              (target->getName() == "free" && !target->hasBody() &&
               operation->getNumArgs() == 1)));
      }
      if (const auto *assignment = dyn_cast<BinaryOperator>(stmt);
          assignment && assignment->isAssignmentOp()) {
        const auto *reference =
            dyn_cast<DeclRefExpr>(assignment->getLHS()->IgnoreParenImpCasts());
        const auto *var =
            reference ? dyn_cast<VarDecl>(reference->getDecl()) : nullptr;
        const auto *member =
            dyn_cast<MemberExpr>(assignment->getLHS()->IgnoreParenImpCasts());
        const bool localCursor = var != nullptr && var->hasLocalStorage() &&
                                 var->getType()->isPointerType();
        const bool clearedPayload =
            member != nullptr &&
            builder.classifyValue(*assignment->getRHS()).kind ==
                ValueOrigin::Kind::Null &&
            std::ranges::any_of(shape->payloads, [&](const auto &payload) {
              return member->getMemberNameInfo().getAsString() ==
                     payload.field.name;
            });
        candidate &= assignment->getOpcode() == BO_Assign &&
                     (localCursor || clearedPayload);
      }
      if (const auto *unary = dyn_cast<UnaryOperator>(stmt))
        candidate &= !unary->isIncrementDecrementOp();
      for (const auto *child : stmt->children())
        work.push_back(child);
    }
    recursiveCleanupCandidate = candidate && work.size() <= 65536;
  }
  if (!*recursiveCleanupCandidate)
    return false;
  const auto actual = builder.resolvePointerValue(*call.getArg(0));
  if (!actual)
    return false;
  const auto fact = containerAt(actual->place, *shape, state);
  if (!fact || !fact->tailOf || fact->tailField.empty() ||
      !shape->recursiveLink(fact->tailField) ||
      state.nulls.stateOf(*fact->tailOf) != core::Nullness::NonNull)
    return false;
  auto source = stableSummaryPathOf(*fact->tailOf);
  if (!source && fact->inputs.size() == 1)
    if (const auto entry = containerInputs.find(*fact->inputs.begin());
        entry != containerInputs.end())
      source = entry->second;
  if (!source || !source->isParam() || !source->isRoot())
    return false;
  if (!requireContainer(*fact, call, state))
    return false;
  recursiveCleanupPremises.insert(*source);
  state.safety->containers.set(actual->place, *fact);
  releaseFootprint(actual->place, true, state);
  invalidateContainers(actual->place, true, false, state);
  state.moves.markMoved(actual->place, core::MoveReason::Freed, locate(call));
  state.resources.clear(actual->place);
  safetyObligation(
      core::SafetyProperty::Call, core::SafetyOutcome::Required, call,
      "proper recursive child",
      "recursive cleanup uses a proper child induction hypothesis");
  return true;
}

void FunctionDataflow::verifyRecursiveCleanup() {
  for (const auto &path : recursiveCleanupPremises) {
    const bool verified = std::ranges::any_of(
        inferred.checked.establishes, [&](const auto &post) {
          return post.kind == core::CheckedRequirementKind::ContainerConsumed &&
                 post.path == path && post.when.trivial() && !post.on;
        });
    safetyObligation(core::SafetyProperty::Semantics,
                     verified ? core::SafetyOutcome::Proven
                              : core::SafetyOutcome::Unresolved,
                     *function.getBody(), "recursive footprint",
                     verified ? "recursive cleanup conserves and releases the "
                                "complete input footprint"
                              : "recursive cleanup does not establish complete "
                                "input footprint consumption");
  }
}

} // namespace weavec::analysis
