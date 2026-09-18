//===- DataflowFootprints.cpp - Allocation conservation (RFC 0027) --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "IntegerSupport.h"
#include "weavec/Core/Induction.h"

#include "llvm/Support/raw_ostream.h"

#include <algorithm>

using namespace clang;

namespace weavec::analysis {

static bool freshFootprintSlot(const core::FunctionSummary &summary,
                               const core::CheckedContract &contract,
                               const core::CheckedRequirement &post) {
  if (!summary.returns.empty() || summary.outcomes.size() != 2 ||
      !summary.outcomes.contains(core::Outcome::Positive) ||
      !summary.outcomes.contains(core::Outcome::Zero) ||
      post.kind != core::CheckedRequirementKind::ContainerFresh ||
      !post.path.isParam() || !post.path.hasDeref() ||
      post.on != core::Outcome::Positive || post.ifNonNull)
    return false;
  const auto nulls = summary.nullOn.find(core::Outcome::Zero);
  return nulls != summary.nullOn.end() && nulls->second.contains(post.path) &&
         std::ranges::none_of(
             contract.establishes,
             [&](const auto &other) {
               return other.kind ==
                          core::CheckedRequirementKind::ContainerFresh &&
                      other.path != post.path;
             }) &&
         std::ranges::all_of(summary.stores,
                             [&](const auto &store) {
                               return !store.value.isFresh() ||
                                      store.dest == post.path;
                             }) &&
         std::ranges::all_of(summary.effects, [&](const auto &entry) {
           const auto &effect = entry.second;
           return !effect.consumed() && !effect.escaped &&
                  (!effect.written || entry.first == post.path);
         });
}

void FunctionDataflow::verifyFootprintTransfers() {
  const auto &contract = inferred.checked;
  for (const auto &transfer : footprintTransfers) {
    const auto applies = [&](const auto &post) {
      return post.when.trivial() && (!post.on || post.on == transfer.outcome);
    };
    const bool published =
        std::ranges::any_of(transfer.alternatives, [&](const auto &paths) {
          if (!std::ranges::all_of(paths, [&](const auto &path) {
                // The exit ledger already proved this proper returned forest
                // contains the complete transfer. A null result contributes
                // no allocation; fresh-result posts describe the non-null arm.
                if (path.isResult() && path.isRoot() &&
                    transfer.outcome == core::Outcome::Null)
                  return true;
                return std::ranges::any_of(
                    contract.establishes, [&](const auto &post) {
                      if (!applies(post))
                        return false;
                      if (post.kind ==
                          core::CheckedRequirementKind::ContainerPartition)
                        return post.path == path || post.other == path;
                      if (post.path != path)
                        return false;
                      if (post.kind ==
                          core::CheckedRequirementKind::ContainerFresh)
                        return (path.isResult() && path.isRoot()) ||
                               freshFootprintSlot(inferred, contract, post);
                      return post.kind == core::CheckedRequirementKind::
                                              ContainerPreserved ||
                             post.kind == core::CheckedRequirementKind::
                                              ContainerCombined ||
                             (post.kind == core::CheckedRequirementKind::
                                               ContainerExtended &&
                              !post.on);
                    });
              }))
            return false;
          return paths.size() < 2 ||
                 std::ranges::any_of(
                     contract.establishes,
                     [&](const auto &post) {
                       return applies(post) &&
                              post.kind == core::CheckedRequirementKind::
                                               ContainerSeparated &&
                              paths.contains(post.path) &&
                              paths.contains(post.other) &&
                              post.path != post.other;
                     });
        });
    if (!published) {
      safetyObligation(
          core::SafetyProperty::Semantics, core::SafetyOutcome::Unresolved,
          *function.getBody(), "container footprint",
          "allocation transfer has no surviving portable output guarantee");
      return;
    }
  }
}

void FunctionDataflow::recordAllocationConsumed(core::PlaceId holder,
                                                core::AnalysisState &state) {
  const auto path = builder.summaryPathOf(holder);
  if (!path || !path->isParam() || !path->isRoot() ||
      path->index >= function.getNumParams() ||
      state.safety->replacedPointers.contains(holder))
    return;
  const auto *parameter = function.getParamDecl(path->index);
  if (parameter->getType()->isPointerType() &&
      builder.placeForVar(*parameter) == holder)
    state.safety->consumedAllocations.insert(holder);
}

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

bool FunctionDataflow::unchangedPointerVariable(core::PlaceId place) {
  if (!changedPointerVariables) {
    auto &changed = changedPointerVariables.emplace();
    const auto note = [&](const Expr *expr) {
      if (const auto *ref = dyn_cast_or_null<DeclRefExpr>(
              expr ? expr->IgnoreParenImpCasts() : nullptr))
        if (const auto *var = dyn_cast<VarDecl>(ref->getDecl()))
          changed.insert(var->getCanonicalDecl());
    };
    std::vector<const Stmt *> work{function.getBody()};
    for (std::size_t i = 0; i < work.size(); ++i) {
      const auto *stmt = work[i];
      if (!stmt)
        continue;
      if (work.size() > 65536) {
        // An exhausted scan proves nothing about any variable.
        changed.insert(nullptr);
        break;
      }
      if (const auto *assignment = dyn_cast<BinaryOperator>(stmt);
          assignment && assignment->isAssignmentOp())
        note(assignment->getLHS());
      if (const auto *unary = dyn_cast<UnaryOperator>(stmt);
          unary &&
          (unary->isIncrementDecrementOp() || unary->getOpcode() == UO_AddrOf))
        note(unary->getSubExpr());
      for (const auto *child : stmt->children())
        work.push_back(child);
    }
  }
  const auto *var = builder.varForPlace(place);
  return var != nullptr && places.isBase(place) &&
         !changedPointerVariables->contains(nullptr) &&
         !changedPointerVariables->contains(var->getCanonicalDecl());
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

void FunctionDataflow::prepareReallocationFootprint(
    const CallExpr &call, core::AnalysisState &state) {
  if (!footprintAllocated || call.getNumArgs() == 0)
    return;
  const auto [saved, inserted] = reallocationFootprints.try_emplace(&call);
  if (inserted)
    saved->second = places.create("reallocation input footprint");
  // A loop can revisit the same call before an older result was refined.
  // That result must not release the new generation's saved input head.
  state.safety->forget(saved->second);
  auto &relations = state.safety->footprints;
  const auto origin = builder.classifyValue(*call.getArg(0));
  if (origin.kind == ValueOrigin::Kind::Null ||
      (origin.place &&
       state.nulls.stateOf(origin.place->place) == core::Nullness::Null)) {
    relations.assign(saved->second, {});
  } else if (origin.place && origin.offset.isZero() &&
             footprintHeads.contains(origin.place->place)) {
    relations.assign(saved->second, {{footprintHead(origin.place->place), 1}});
  }
}

void FunctionDataflow::refineAllocationFootprints(core::AnalysisState &state) {
  auto &pendingReleases = state.safety->pendingAllocationReleases;
  if (state.safety->havoc) {
    pendingReleases.clear();
    return;
  }
  for (auto entry = pendingReleases.begin(); entry != pendingReleases.end();) {
    const auto [holder, release] = *entry;
    const auto object = state.safety->objects.find(holder);
    if (!footprintReleased || object == state.safety->objects.end() ||
        object->second != release.storage ||
        state.safety->invalidatedPointers.contains(holder)) {
      entry = pendingReleases.erase(entry);
      continue;
    }
    const auto nullness = state.nulls.stateOf(holder);
    if (nullness == core::Nullness::NonNull) {
      state.safety->footprints.assign(
          *footprintReleased, {{*footprintReleased, 1}, {release.snapshot, 1}});
      entry = pendingReleases.erase(entry);
    } else if (nullness == core::Nullness::Null) {
      entry = pendingReleases.erase(entry);
    } else {
      ++entry;
    }
  }
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
  const bool reallocation =
      (library == "realloc" || library == "reallocarray") &&
      reallocationFootprints.contains(origin.call);
  const auto callee =
      origin.call ? callSummaries.find(origin.call) : callSummaries.end();
  const bool byteHelper =
      origin.call != nullptr && origin.call->getType()->isPointerType() &&
      origin.call->getType()->getPointeeType()->isCharType() &&
      origin.family == "free" && callee != callSummaries.end() &&
      callee->second && callee->second->checked.complete() &&
      std::ranges::none_of(
          callee->second->checked.establishes, [](const auto &post) {
            return post.kind == core::CheckedRequirementKind::ContainerFresh;
          });
  const bool freshAllocation = pointer.fresh && footprintAllocated &&
                               (library == "malloc" || library == "calloc" ||
                                reallocation || byteHelper);
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
    const auto owner =
        origin.call ? callSummaries.find(origin.call) : callSummaries.end();
    const bool mayAcquire = owner == callSummaries.end() || !owner->second ||
                            owner->second->returnsFresh();
    if (footprintAllocated && !materializingHeap && mayAcquire &&
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
      if (const auto previous = reallocationFootprints.find(origin.call);
          reallocation && previous != reallocationFootprints.end())
        pointer.pendingAllocationRelease = previous->second;
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
  state.safety->pendingAllocationReleases.erase(dest);
  if (pointer.pendingAllocationRelease && pointer.storage) {
    if (state.safety->pendingAllocationReleases.size() <
        core::MaxFootprintVariables)
      state.safety->pendingAllocationReleases.emplace(
          dest, core::SafetyState::PendingAllocationRelease{
                    .storage = *pointer.storage,
                    .snapshot = *pointer.pendingAllocationRelease});
    else
      inferred.checked.limited = true;
  }
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
      // RFC 0029: the live unchanged singleton head remains the complete
      // entry footprint. Every other output member must come from this
      // invocation, and the allocation ledger must account for all of it.
      if (first->first == entry.holder && first->second == path &&
          shape.access == core::ContainerAccess::Release &&
          shape.singletonHead() && footprintAllocated &&
          !state.safety->replacedPointers.contains(entry.holder)) {
        const auto *fact = state.safety->containers.find(first->first);
        const auto memory = checkedMemoryAt(entry.holder, {}, {}, state);
        const bool onlyEntry =
            fact != nullptr && fact->inputs.size() == 1 &&
            containerInputs.contains(*fact->inputs.begin()) &&
            containerInputs.at(*fact->inputs.begin()) == path;
        core::FootprintSum complete{{entry.identity, 1},
                                    {*footprintAllocated, 1},
                                    {*footprintReleased, -1},
                                    {first->first, -1}};
        if (onlyEntry && fact->allocationCompatible && memory &&
            memory->input == path && memory->begin == core::Affine{} &&
            checkedValid(*memory, state) && relations.entails(complete)) {
          auto extended = fact->shape;
          if (extended.access == core::ContainerAccess::Release) {
            // A case may know non-nullness from its captured values. Keep
            // the explicit live-entry premise required by this portable
            // ownership relation as well.
            inferred.checked.require(
                {.kind = core::CheckedRequirementKind::Valid,
                 .path = path,
                 .other = {},
                 .family = {}});
            outputs.establish(
                {.kind = core::CheckedRequirementKind::ContainerExtended,
                 .path = path,
                 .other = path,
                 .family = extended.encode(),
                 .on = outcome});
          }
        }
      }
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
      // RFC 0029: an attaching helper may also publish a payload acquired by
      // this call. The unchanged live head then owns both complete entry
      // footprints and exactly this invocation's outstanding acquisitions.
      if (!footprintAllocated ||
          a->second.access != core::ContainerAccess::Release ||
          b->second.access != core::ContainerAccess::Release ||
          a->second.family != b->second.family ||
          a->second.payloads != b->second.payloads)
        continue;
      for (const auto &[head, tail] :
           {std::pair{first, second}, std::pair{second, first}}) {
        const auto holder = head->second.holder;
        const auto output = visible.find(holder);
        const auto *fact = state.safety->containers.find(holder);
        const auto memory = checkedMemoryAt(holder, {}, {}, state);
        if (output == visible.end() || output->second != head->first || !fact ||
            !fact->allocationCompatible || !memory ||
            memory->begin != core::Affine{} ||
            !unchangedPointerVariable(holder) ||
            !checkedValid(*memory, state) ||
            relations.entails({{holder, 1},
                               {head->second.identity, -1},
                               {tail->second.identity, -1}}) ||
            !relations.entails({{holder, 1},
                                {head->second.identity, -1},
                                {tail->second.identity, -1},
                                {*footprintAllocated, -1},
                                {*footprintReleased, 1}}) ||
            !std::ranges::all_of(fact->inputs, [&](const auto input) {
              const auto source = containerInputs.find(input);
              return source != containerInputs.end() &&
                     (source->second == head->first ||
                      source->second == tail->first);
            }))
          continue;
        inferred.checked.require(
            {.kind = core::CheckedRequirementKind::ContainerSeparated,
             .path = first->first,
             .other = second->first,
             .family = {}});
        inferred.checked.require({.kind = core::CheckedRequirementKind::Valid,
                                  .path = head->first,
                                  .other = {},
                                  .family = {}});
        outputs.establish(
            {.kind = core::CheckedRequirementKind::ContainerCombined,
             .path = head->first,
             .other = head->first,
             .begin = core::PathAffine::ofPath(tail->first),
             .end = core::PathAffine::ofConstant(1),
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
  // RFC 0029: byte allocations participate in the same acquisition ledger,
  // but returning their live base transfers one allocation, not a forest.
  if (!accounted && returned && outcome == core::Outcome::NonNull &&
      footprintHeads.contains(*returned)) {
    const auto resource = state.resources.recordOf(*returned);
    const auto memory = checkedMemoryAt(*returned, {}, {}, state);
    if (resource && resource->origin == core::ResourceOrigin::Allocated &&
        resource->family == "free" && !resource->escaped &&
        !state.moves.recordOf(*returned) && memory &&
        memory->begin == core::Affine::ofConstant(0) &&
        checkedValid(*memory, state)) {
      auto transferred = balance;
      --transferred[footprintHead(*returned)];
      accounted = relations.entails(std::move(transferred));
    }
  }
  // A complete callee's fresh result returned without a local holder has no
  // resource record. Its identity is either null or that single allocation.
  if (!accounted && returned && footprintHeads.contains(*returned) &&
      (outcome == core::Outcome::NonNull || outcome == core::Outcome::Null))
    for (const auto &[expression, place] : checkedReturnPlaces) {
      const auto *call = dyn_cast<CallExpr>(expression);
      if (place != *returned || call == nullptr)
        continue;
      // Every alternative is null or this call's own allocation base.
      const std::function<bool(const ValueOrigin &)> single =
          [&](const ValueOrigin &origin) {
            if (origin.kind == ValueOrigin::Kind::Null)
              return true;
            if (origin.kind == ValueOrigin::Kind::Conditional)
              return !origin.alternatives.empty() &&
                     std::ranges::all_of(origin.alternatives, single);
            return origin.kind == ValueOrigin::Kind::Alloc &&
                   origin.call == call && origin.family == "free" &&
                   origin.offset.isZero();
          };
      if (!single(builder.classifyValue(*call)) ||
          !checkedObjects.contains({call, *returned}) ||
          state.safety->invalidatedPointers.contains(*returned))
        continue;
      auto transferred = balance;
      --transferred[footprintHead(*returned)];
      accounted = relations.entails(std::move(transferred));
    }
  const bool needsOutput = !accounted;
  FootprintTransfers transfers{.outcome = outcome, .alternatives = {}};
  const auto remember = [&](std::set<core::SummaryPath> paths) {
    accounted = true;
    if (std::ranges::find(transfers.alternatives, paths) !=
        transfers.alternatives.end())
      return;
    if (transfers.alternatives.size() < core::MaxContainerFacts)
      transfers.alternatives.push_back(std::move(paths));
    else
      inferred.checked.limited = true;
  };
  for (auto first = visible.begin(); first != visible.end() && needsOutput;
       ++first) {
    auto remaining = balance;
    --remaining[first->first];
    if (relations.entails(remaining))
      remember({first->second});
    for (auto second = std::next(first); second != visible.end(); ++second) {
      if (!state.safety->containers.separated(first->first, second->first))
        continue;
      auto partition = remaining;
      --partition[second->first];
      if (relations.entails(std::move(partition)))
        remember({first->second, second->second});
    }
  }
  if (needsOutput && accounted && recording() &&
      std::ranges::find(footprintTransfers, transfers) ==
          footprintTransfers.end()) {
    if (footprintTransfers.size() < core::MaxSafetyRequirements)
      footprintTransfers.push_back(std::move(transfers));
    else
      inferred.checked.limited = true;
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
  containerPrefixPosts[&call].clear();
  if (!contract.complete())
    return;
  const auto arguments = containerArguments.find(&call);
  auto &inputs = footprintCallInputs[&call];
  freshFootprintSlots.erase(&call);
  freshFootprintSlotParents.erase(&call);
  // A single success-published output slot carries an entire fresh forest,
  // not just the allocation of its head. The other returning class acquires
  // no output allocation. Keep this conditional region until the caller
  // actually tests the result; an unchecked call cannot discharge its ledger.
  const auto resolved = callSummaries.find(&call);
  if (call.getDirectCallee() != nullptr && resolved != callSummaries.end() &&
      resolved->second && resolved->second->returns.empty() &&
      resolved->second->outcomes.size() == 2 &&
      resolved->second->outcomes.contains(core::Outcome::Positive) &&
      resolved->second->outcomes.contains(core::Outcome::Zero)) {
    const auto &summary = *resolved->second;
    std::set<core::SummaryPath> slots;
    for (const auto &post : contract.establishes) {
      const auto nulls = summary.nullOn.find(core::Outcome::Zero);
      const auto guard = checkedGuard(post.when, call, state);
      if (post.kind == core::CheckedRequirementKind::ContainerFresh &&
          post.path.isParam() && post.path.hasDeref() &&
          post.on == core::Outcome::Positive && !post.ifNonNull && guard &&
          guard->trivial() && nulls != summary.nullOn.end() &&
          nulls->second.contains(post.path))
        slots.insert(post.path);
    }
    if (slots.size() == 1 &&
        std::ranges::all_of(summary.stores,
                            [&](const auto &store) {
                              return !store.value.isFresh() ||
                                     store.dest == *slots.begin();
                            }) &&
        std::ranges::all_of(summary.effects, [&](const auto &entry) {
          const auto &effect = entry.second;
          return !effect.consumed() && !effect.escaped &&
                 (!effect.written || entry.first == *slots.begin());
        }))
      freshFootprintSlots.emplace(&call, *slots.begin());
  }
  if (const auto slot = freshFootprintSlots.find(&call);
      slot != freshFootprintSlots.end())
    if (const auto actual = builder.resolveSummaryPath(slot->second, call);
        actual && places.step(actual->place) == core::PathStep::Field)
      if (const auto object = places.parent(actual->place);
          object && places.step(*object) == core::PathStep::Deref)
        if (const auto parent = places.parent(*object))
          if (const auto *fact = state.safety->containers.find(*parent))
            freshFootprintSlotParents.emplace(&call, std::pair{*parent, *fact});
  if (footprintAllocated && containerCallObjects.contains(&call) &&
      (freshFootprintSlots.contains(&call) ||
       std::ranges::any_of(contract.establishes, [&](const auto &post) {
         const auto guard = checkedGuard(post.when, call, state);
         return post.kind == core::CheckedRequirementKind::ContainerFresh &&
                post.path.isResult() && post.path.isRoot() &&
                (!post.on || post.on == core::Outcome::NonNull) && guard &&
                guard->trivial();
       }))) {
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
  std::set<core::PlaceId> extended;
  for (const auto &post : contract.establishes) {
    if (post.kind != core::CheckedRequirementKind::ContainerPreserved &&
        post.kind != core::CheckedRequirementKind::ContainerConsumed &&
        post.kind != core::CheckedRequirementKind::ContainerPartition &&
        post.kind != core::CheckedRequirementKind::ContainerCombined &&
        post.kind != core::CheckedRequirementKind::ContainerExtended)
      continue;
    const auto guard = checkedGuard(post.when, call, state);
    if (!guard || !guard->trivial())
      continue;
    if (post.kind == core::CheckedRequirementKind::ContainerExtended &&
        std::ranges::any_of(contract.establishes, [&](const auto &other) {
          return other.kind ==
                     core::CheckedRequirementKind::ContainerPreserved &&
                 other.path == post.path && other.other == post.other &&
                 other.when == post.when && other.on == post.on;
        }))
      continue;
    bool captured = false;
    if (post.kind == core::CheckedRequirementKind::ContainerPartition)
      captured = post.begin.path && capture(*post.begin.path);
    else
      captured = capture(post.other);
    if (post.kind == core::CheckedRequirementKind::ContainerCombined)
      captured &= post.begin.path && capture(*post.begin.path);
    const bool combinedExtension =
        post.kind == core::CheckedRequirementKind::ContainerCombined &&
        post.end == core::PathAffine::ofConstant(1);
    if (captured && combinedExtension && !footprintAllocated)
      continue;
    if (captured &&
        (post.kind == core::CheckedRequirementKind::ContainerExtended ||
         combinedExtension)) {
      if ((post.on && !combinedExtension) || !footprintAllocated)
        continue;
      const auto [extension, inserted] =
          footprintExtensions.try_emplace({&call, post.path});
      if (inserted)
        extension->second = places.create("fresh extension footprint");
      // Outcome-specific outputs of one path share one fresh region.
      if (extended.insert(extension->second).second) {
        state.safety->footprints.forget(extension->second);
        state.safety->footprints.assign(
            *footprintAllocated,
            {{*footprintAllocated, 1}, {extension->second, 1}});
      }
    }
    if (captured) {
      posts.push_back(post);
      captureContainerPrefixes(call, post, state);
    }
  }
}

void FunctionDataflow::applyFootprintPosts(
    const CallExpr &call, core::AnalysisState &state,
    std::optional<core::PlaceId> result,
    const std::optional<core::ValueFact> *prior) {
  const auto region = containerCallObjects.find(&call);
  const auto structural = containerPosts.find(&call);
  if (!prior && !result && region != containerCallObjects.end())
    if (const auto slot = freshFootprintSlots.find(&call);
        slot != freshFootprintSlots.end()) {
      const auto outcome = scalarFactOf(call, state);
      const bool failure =
          outcome && outcome->implies(core::ValueFact::of(core::Outcome::Zero));
      const bool success =
          outcome &&
          outcome->implies(core::ValueFact::of(core::Outcome::Positive));
      const auto actual = builder.resolveSummaryPath(slot->second, call);
      if (failure)
        state.safety->footprints.constrain({{region->second, 1}});
      else if (success)
        if (actual && state.safety->containers.find(actual->place)) {
          state.safety->footprints.assign(actual->place, {{region->second, 1}});
          snapshotContainerOutput(actual->place, state);
        }
      // The only possible write of this completed helper is the exact output
      // slot. Restore the captured parent frame and fold that one changed
      // child using its newly proved output (or its actual null failure).
      if (actual && (success || failure))
        if (const auto parent = freshFootprintSlotParents.find(&call);
            parent != freshFootprintSlotParents.end())
          if (const auto *field =
                  dyn_cast_or_null<FieldDecl>(builder.declFor(actual->place))) {
            const auto &[holder, before] = parent->second;
            state.safety->containers.set(holder, before);
            if (before.localAllocation)
              state.safety->containers.markFresh(holder);
            if (success)
              state.safety->containers.separate(holder, actual->place);
            foldContainerStores(holder, *field->getParent(), field, call,
                                state);
          }
    }
  if (!prior && result && region != containerCallObjects.end() &&
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
    // An immediate result test installs only what the call could not, and
    // never replays consumption.
    // On an immediate result test, install only the transfers that add a
    // guarantee. Replaying a preserved or consumed output could replace
    // current evidence with the weaker captured entry footprint.
    if (prior &&
        (!post.on ||
         post.kind == core::CheckedRequirementKind::ContainerConsumed ||
         (*prior && (*prior)->implies(core::ValueFact::of(*post.on)))))
      continue;
    const bool combinedExtension =
        post.kind == core::CheckedRequirementKind::ContainerCombined &&
        post.end == core::PathAffine::ofConstant(1);
    if (post.on) {
      const auto outcome = scalarFactOf(call, state);
      const bool resultMatches =
          result &&
          ((post.on == core::Outcome::NonNull &&
            state.nulls.stateOf(*result) == core::Nullness::NonNull) ||
           (post.on == core::Outcome::Null &&
            state.nulls.stateOf(*result) == core::Nullness::Null));
      if (!resultMatches &&
          (!outcome || !outcome->implies(core::ValueFact::of(*post.on)))) {
        // The fresh region exists only on a covered outcome. Once every
        // covering output of this path is excluded, it is empty.
        const auto extension = footprintExtensions.find({&call, post.path});
        if (combinedExtension && outcome &&
            extension != footprintExtensions.end() &&
            std::ranges::none_of(found->second, [&](const auto &other) {
              return other.kind ==
                         core::CheckedRequirementKind::ContainerCombined &&
                     other.end == core::PathAffine::ofConstant(1) &&
                     other.path == post.path &&
                     (!other.on || outcome->classes.contains(*other.on));
            }))
          state.safety->footprints.constrain({{extension->second, 1}});
        continue;
      }
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
      if (combinedExtension) {
        const auto extension = footprintExtensions.find({&call, post.path});
        if (extension == footprintExtensions.end())
          continue;
        ++footprint[extension->second];
      }
      if (post.kind == core::CheckedRequirementKind::ContainerExtended) {
        const auto extension = footprintExtensions.find({&call, post.path});
        if (extension == footprintExtensions.end())
          continue;
        ++footprint[extension->second];
        state.safety->footprints.assign(footprintHead(*holder),
                                        {{inputs.at(post.other), 1}});
      }
      // RFC 0029: this output states the final footprint is exactly its
      // incoming regions, so the same contract's fresh region is empty.
      if (post.kind != core::CheckedRequirementKind::ContainerExtended &&
          !combinedExtension)
        if (const auto extension = footprintExtensions.find({&call, post.path});
            extension != footprintExtensions.end())
          state.safety->footprints.constrain({{extension->second, 1}});
      state.safety->footprints.assign(*holder, std::move(footprint));
      const auto *fact = state.safety->containers.find(*holder);
      const auto memory = checkedMemoryAt(*holder, {}, {}, state);
      if (fact && memory)
        for (const auto &[alias, edge] :
             state.definiteAliases.edgesFrom(*holder)) {
          const auto *otherFact = state.safety->containers.find(alias);
          if (alias == *holder || !edge.exact() || !otherFact ||
              *otherFact != *fact ||
              !state.definiteAliases.sameShare(*holder, alias))
            continue;
          const auto other = checkedMemoryAt(alias, {}, {}, state);
          if (other && other->storage == memory->storage &&
              other->begin == memory->begin)
            state.safety->footprints.assign(alias, {{*holder, 1}});
        }
    } else {
      continue;
    }
    applyContainerPrefixes(call, post, *holder, state);
    // RFC 0028: a verified partition or preservation transfers the cleanup
    // duty with the complete footprint, including an unassigned call result.
    // The function's allocation balance still rejects losing that result.
    if (footprintAllocated) {
      const auto retire = [&](const core::SummaryPath &source) {
        if (const auto actual = builder.resolveSummaryPath(source, call);
            actual && actual->place != *holder) {
          const auto resource = state.resources.recordOf(actual->place);
          if (resource && resource->origin == core::ResourceOrigin::Allocated)
            state.resources.clear(actual->place);
        }
      };
      if (partition) {
        if (post.begin.path)
          retire(*post.begin.path);
      } else {
        retire(post.other);
        if (post.kind == core::CheckedRequirementKind::ContainerCombined &&
            post.begin.path)
          retire(*post.begin.path);
      }
    }
  }
}

bool FunctionDataflow::handleRecursiveContract(const CallExpr &call,
                                               core::AnalysisState &state) {
  const auto *callee = call.getDirectCallee();
  const auto *group = summaries.recursiveContractGroup(function);
  if (!callee || !summaries.recursiveContractPeer(function, *callee) ||
      !group || function.getNumParams() != 1 || call.getNumArgs() != 1)
    return false;
  const bool releases = group->releases;
  const auto *shape = containerShape(function.getParamDecl(0)->getType());
  if (!shape || shape->access != (releases ? core::ContainerAccess::Release
                                           : core::ContainerAccess::Read))
    return false;
  if (!recursiveContractCandidate) {
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
            path && path->isGlobal() && binding != callbackBindings.end() &&
            !binding->second.unknown && !binding->second.null &&
            binding->second.functions == std::set<std::string>{"free"} &&
            operation->getNumArgs() == 1;
        // A callback stored in the current node need not be the callback in
        // its children. The structural predicate carries no per-node callback
        // behavior; a root specialization cannot authorize the induction.
        candidate &= (releases && actualFree) ||
                     (target != nullptr &&
                      (summaries.recursiveContractPeer(function, *target) ||
                       (releases && target->getName() == "free" &&
                        !target->hasBody() && operation->getNumArgs() == 1)));
      }
      if (const auto *assignment = dyn_cast<BinaryOperator>(stmt);
          assignment && assignment->isAssignmentOp()) {
        const auto *reference =
            dyn_cast<DeclRefExpr>(assignment->getLHS()->IgnoreParenImpCasts());
        const auto *var =
            reference ? dyn_cast<VarDecl>(reference->getDecl()) : nullptr;
        const auto *member =
            dyn_cast<MemberExpr>(assignment->getLHS()->IgnoreParenImpCasts());
        const bool localCursor =
            var != nullptr && var->hasLocalStorage() &&
            (var->getType()->isPointerType() ||
             (!releases && var->getType()->isIntegerType()));
        const bool clearedPayload =
            member != nullptr &&
            builder.classifyValue(*assignment->getRHS()).kind ==
                ValueOrigin::Kind::Null &&
            std::ranges::any_of(shape->payloads, [&](const auto &payload) {
              return member->getMemberNameInfo().getAsString() ==
                     payload.field.name;
            });
        candidate &= (assignment->getOpcode() == BO_Assign || !releases) &&
                     (localCursor || (releases && clearedPayload));
      }
      if (const auto *unary = dyn_cast<UnaryOperator>(stmt);
          unary && unary->isIncrementDecrementOp()) {
        const auto *reference =
            dyn_cast<DeclRefExpr>(unary->getSubExpr()->IgnoreParenImpCasts());
        const auto *var =
            reference ? dyn_cast<VarDecl>(reference->getDecl()) : nullptr;
        candidate &= !releases && var != nullptr && var->hasLocalStorage() &&
                     var->getType()->isIntegerType();
      }
      for (const auto *child : stmt->children())
        work.push_back(child);
    }
    recursiveContractCandidate = candidate && work.size() <= 65536;
  }
  if (!*recursiveContractCandidate)
    return false;
  // RFC 0029: ownership resolution intentionally strips pointer arithmetic.
  // An induction premise instead needs the exact node, not an interior or
  // one-past address into the same allocation.
  const auto origin = builder.classifyValue(*call.getArg(0));
  const auto actual = origin.place;
  if (origin.kind != ValueOrigin::Kind::Copy || !actual ||
      !actual->element.isWhole() || !origin.offset.isZero() ||
      (origin.boundsOffset && !origin.boundsOffset->isZero()) ||
      std::ranges::any_of(origin.spatialSteps,
                          [](const auto &step) { return !step.isZero(); }))
    return false;
  if (const auto spatial = spatialRecordAt(actual->place, state);
      spatial && !spatial->offset.isZero())
    return false;
  const auto fact = containerAt(actual->place, *shape, state);
  if (!fact)
    return false;
  const bool strict =
      fact->tailOf && !fact->tailField.empty() &&
      shape->recursiveLink(fact->tailField) &&
      state.nulls.stateOf(*fact->tailOf) == core::Nullness::NonNull;
  const bool forward =
      actual->place == builder.placeForVar(*function.getParamDecl(0)) &&
      !state.safety->replacedPointers.contains(actual->place) &&
      fact->releasedChildren.empty() && fact->releasedPayloads.empty();
  if (!strict && !forward)
    return false;
  auto source = stableSummaryPathOf(strict ? *fact->tailOf : actual->place);
  if (!source && fact->inputs.size() == 1)
    if (const auto entry = containerInputs.find(*fact->inputs.begin());
        entry != containerInputs.end())
      source = entry->second;
  if (!source || !source->isParam() || !source->isRoot())
    return false;
  if (!requireContainer(*fact, call, state))
    return false;
  recursiveContractPremises.insert(*source);
  recursiveContractCalls.emplace(callee->getCanonicalDecl(), strict);
  state.safety->containers.set(actual->place, *fact);
  if (releases) {
    releaseFootprint(actual->place, true, state);
    invalidateContainers(actual->place, true, false, state);
    state.moves.markMoved(actual->place, core::MoveReason::Freed, locate(call));
    state.resources.clear(actual->place);
  }
  safetyObligation(
      core::SafetyProperty::Call, core::SafetyOutcome::Required, call,
      strict ? "proper recursive child" : "recursive forwarding",
      strict ? "recursive call uses a proper child induction hypothesis"
             : "recursive forwarding requires progress on every group cycle");
  return true;
}

void FunctionDataflow::verifyRecursiveContract() {
  const auto *group = summaries.recursiveContractGroup(function);
  if (!group)
    return;
  if (group->constructs)
    verifyRecursiveConstruction();
  if (group->writes)
    verifyRecursiveWriter();
  if (group->members.size() == 1 && !recursiveContractCalls.empty()) {
    std::vector<core::InductionEdge> edges;
    for (const auto &[callee, strict] : recursiveContractCalls) {
      (void)callee;
      edges.push_back({.caller = 0, .callee = 0, .strict = strict});
    }
    if (!core::validInductionProgress(1, edges)) {
      summaries.failedRecursiveProgress.insert(function.getCanonicalDecl());
      safetyObligation(core::SafetyProperty::Semantics,
                       core::SafetyOutcome::Unresolved, *function.getBody(),
                       "recursive progress",
                       "recursive proof cycle has no strict progress");
    }
  }
  for (const auto &path : recursiveContractPremises) {
    const bool verified = std::ranges::any_of(
        inferred.checked.establishes, [&](const auto &post) {
          return post.kind ==
                     (group->releases
                          ? core::CheckedRequirementKind::ContainerConsumed
                          : core::CheckedRequirementKind::ContainerPreserved) &&
                 post.path == path && post.when.trivial() && !post.on;
        });
    const auto *success = group->releases
                              ? "recursive cleanup conserves and releases the "
                                "complete input footprint"
                              : "recursive traversal preserves the complete "
                                "input footprint";
    const auto *failure =
        group->releases ? "recursive cleanup does not establish complete "
                          "input footprint consumption"
                        : "recursive traversal does not establish complete "
                          "input footprint preservation";
    safetyObligation(core::SafetyProperty::Semantics,
                     verified ? core::SafetyOutcome::Proven
                              : core::SafetyOutcome::Unresolved,
                     *function.getBody(), "recursive footprint",
                     verified ? success : failure);
  }
}

} // namespace weavec::analysis
