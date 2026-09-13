//===- DataflowBufferContracts.cpp - Buffer interfaces (RFC 0026) --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "Dataflow.h"

using namespace clang;
namespace weavec::analysis {
std::optional<core::PlaceId>
FunctionDataflow::bufferArgument(const core::CheckedRequirement &requirement,
                                 const CallExpr &call,
                                 core::AnalysisState &state) {
  (void)state;
  const auto shape = core::BufferShape::decode(requirement.family);
  const auto object = builder.resolveSummaryPath(requirement.path, call, true);
  if (!shape || !object || !object->element.isWhole())
    return std::nullopt;
  if (const auto *decl =
          dyn_cast_or_null<ValueDecl>(builder.declFor(object->place))) {
    auto type = decl->getType();
    if (type->isPointerType())
      type = type->getPointeeType();
    if (const auto *record = type->getAsRecordDecl())
      registerBuffer(object->place, *record);
  }
  const auto found = bufferObjects.find(object->place);
  if (found == bufferObjects.end() || !found->second.sameLayoutAs(*shape)) {
    return std::nullopt;
  }
  return object->place;
}

void FunctionDataflow::checkedBufferCall(
    const core::CheckedRequirement &requirement, const CallExpr &call,
    core::AnalysisState &state) {
  const auto object = bufferArgument(requirement, call, state);
  bool proved = false;
  bool backingMissing = false;
  if (object) {
    const auto &shape = bufferObjects.at(*object);
    const auto data = places.field(*object, shape.data.name);
    if (const auto *fact = bufferFact(data, state)) {
      const auto required = core::BufferShape::decode(requirement.family);
      proved = fact->initialized && required && fact->shape.entails(*required);
      backingMissing =
          required && required->ownsBacking && !fact->shape.ownsBacking;
    }
  }
  safetyObligation(
      core::SafetyProperty::Call,
      proved ? core::SafetyOutcome::Proven : core::SafetyOutcome::Unresolved,
      call, "buffer",
      backingMissing ? "callee buffer backing storage requires allocation "
                       "release permission"
                     : "callee buffer allocation extent and initialized-prefix "
                       "precondition must hold");
}

void FunctionDataflow::captureBufferPosts(const CallExpr &call,
                                          const core::CheckedContract &contract,
                                          core::AnalysisState &state) {
  auto &posts = bufferPosts[&call];
  posts.clear();
  bufferCallInputs[&call].clear();
  bufferSequencePosts[&call].clear();
  bufferPostBounds[&call].clear();
  if (!contract.complete())
    return;
  for (const auto &requirement : contract.requirements) {
    if (requirement.kind != core::CheckedRequirementKind::Buffer)
      continue;
    const auto object = bufferArgument(requirement, call, state);
    if (!object)
      return;
    const auto *fact = bufferFact(
        places.field(*object, bufferObjects.at(*object).data.name), state);
    const auto required = core::BufferShape::decode(requirement.family);
    if (!fact || !fact->initialized || !required ||
        !fact->shape.entails(*required)) {
      return;
    }
  }
  for (const auto &post : contract.establishes)
    if (post.kind == core::CheckedRequirementKind::Buffer ||
        post.kind == core::CheckedRequirementKind::BufferPreserved ||
        post.kind == core::CheckedRequirementKind::BufferAppended) {
      const auto guard = checkedGuard(post.when, call, state);
      const auto object = bufferArgument(post, call, state);
      if (!guard || !guard->trivial() || !object)
        continue;
      auto captured = post;
      auto shape = core::BufferShape::decode(post.family).value();
      const auto data = places.field(*object, shape.data.name);
      if (!bufferCallInputs[&call].contains(*object)) {
        BufferCallInput input;
        if (const auto previous = state.safety->buffers.sequences.find(data);
            previous != state.safety->buffers.sequences.end())
          input.sequence = previous->second;
        if (const auto *before = bufferFact(data, state)) {
          input.ownsElements = before->shape.ownsElements;
          input.length =
              foldAffine(core::Affine::ofPlace(before->length), state);
        }
        bufferCallInputs[&call][*object] = input;
      }
      if (post.kind != core::CheckedRequirementKind::Buffer) {
        const auto &input = bufferCallInputs[&call][*object];
        auto &saved = bufferSequencePosts[&call][post];
        const auto result = numericCallResult(call);
        if (!bufferSequenceEvents.contains(&call))
          bufferSequenceEvents[&call] =
              result.value_or(places.create("buffer call"));
        saved.event = bufferSequenceEvents.at(&call);
        bufferSequenceCalls[saved.event] = &call;
        saved.sequence = input.sequence;
        saved.index = input.length;
        saved.ownsPrefix = input.ownsElements;
        saved.appended =
            post.kind == core::CheckedRequirementKind::BufferAppended;
        if (post.on)
          saved.when.require(saved.event, core::ValueFact::of(*post.on));
        if (saved.appended) {
          ValueOrigin origin;
          if (post.other.isParam() && post.other.isRoot() &&
              post.other.index < call.getNumArgs()) {
            origin = builder.classifyValue(*call.getArg(post.other.index));
          } else if (const auto ref =
                         builder.resolveSummaryPath(post.other, call, true)) {
            origin.kind = ValueOrigin::Kind::Copy;
            origin.place = *ref;
          }
          saved.nullValue = origin.kind == ValueOrigin::Kind::Null;
          if (origin.place) {
            if (origin.kind == ValueOrigin::Kind::Borrow)
              saved.borrowed = origin.place->place;
            if (origin.kind == ValueOrigin::Kind::Copy)
              saved.value = origin.place->place;
          }
          if (saved.value) {
            saved.nullValue |=
                origin.offset.isZero() &&
                state.nulls.stateOf(*saved.value) == core::Nullness::Null;
            const auto owned = state.resources.recordOf(*saved.value);
            const auto spatial = state.spatial.recordOf(*saved.value);
            auto ownershipGuard = owned ? owned->guard : core::PlaceGuard{};
            saved.ownsValue =
                owned && owned->origin == core::ResourceOrigin::Allocated &&
                owned->family == "free" && !owned->escaped &&
                !state.moves.recordOf(*saved.value) &&
                !state.safety->invalidatedPointers.contains(*saved.value) &&
                origin.offset.isZero() &&
                (!spatial || spatial->offset.isZero()) &&
                pruneGuard(ownershipGuard, state) && ownershipGuard.trivial();
          }
          saved.ownsValue |= saved.nullValue;
          if (saved.sequence && saved.value && !saved.sequence->appended)
            saved.sequence->appended = *saved.value;
          else
            saved.sequence.reset();
        }
      }
      if (const auto *before = bufferFact(data, state);
          before && before->shape.ownsBacking) {
        const auto effects = classifyCall(call, summaries);
        bool preserved = effects && effects->summary;
        if (preserved)
          for (const auto &[path, effect] : effects->summary->effects) {
            if (!effect.written && !effect.consumed())
              continue;
            const auto ref = builder.resolveSummaryPath(path, call, true);
            if (!ref || ref->place == data ||
                places.isDescendantOf(data, ref->place))
              preserved = false;
          }
        shape.ownsBacking |= preserved;
      }
      captured.family = shape.encode();
      posts.push_back(std::move(captured));
    }
  // Ownership is common to every result only if each result proves a sequence
  // transfer and each appended value has its own distinct release permission.
  const auto effects = classifyCall(call, summaries);
  for (auto &post : posts) {
    if (post.kind != core::CheckedRequirementKind::Buffer)
      continue;
    const auto object = bufferArgument(post, call, state);
    if (!object || !bufferCallInputs[&call][*object].ownsElements)
      continue;
    const auto covered = [&](std::optional<core::Outcome> outcome) {
      return std::ranges::any_of(bufferSequencePosts[&call],
                                 [&](const auto &entry) {
                                   const auto &proof = entry.first;
                                   const auto &input = entry.second;
                                   return proof.path == post.path &&
                                          (!proof.on || proof.on == outcome) &&
                                          (!input.appended || input.ownsValue);
                                 });
    };
    bool every = covered(std::nullopt);
    if (!every && effects && effects->summary &&
        !effects->summary->outcomes.empty())
      every = std::ranges::all_of(
          effects->summary->outcomes,
          [&](const auto &entry) { return covered(entry.first); });
    if (every) {
      auto shape = core::BufferShape::decode(post.family).value();
      shape.ownsElements = true;
      post.family = shape.encode();
    }
  }
}

void FunctionDataflow::applyBufferPosts(const CallExpr &call,
                                        core::AnalysisState &state) {
  const auto found = bufferPosts.find(&call);
  if (found == bufferPosts.end())
    return;
  for (const auto &post : found->second) {
    const auto object = bufferArgument(post, call, state);
    if (!object)
      continue;
    auto shape = core::BufferShape::decode(post.family).value();
    const auto data = places.field(*object, shape.data.name);
    if (const auto *previous = bufferFact(data, state))
      shape.ownsBacking |= previous->shape.ownsBacking;
    const auto fact = scalarFactOf(call, state);
    const bool active =
        !post.on || (fact && fact->implies(core::ValueFact::of(*post.on)));
    if (post.kind != core::CheckedRequirementKind::Buffer) {
      const auto proof = std::ranges::find_if(
          bufferSequencePosts[&call], [&](const auto &entry) {
            auto candidate = post;
            candidate.family = entry.first.family;
            return candidate == entry.first;
          });
      if (proof != bufferSequencePosts[&call].end()) {
        auto sequencePost = proof->second;
        if (active)
          sequencePost.when = {};
        auto &entries = state.safety->buffers.pendingSequences[data];
        if (std::ranges::find(entries, sequencePost) == entries.end()) {
          if (entries.size() < core::MaxBufferShapes)
            entries.push_back(std::move(sequencePost));
          else
            state.safety->buffers.limited = true;
        }
      }
      continue;
    }
    if (active) {
      // A proved postcondition names the current field. Retire its old-value
      // invalidation only; separately saved pointers keep their lifetime facts.
      state.safety->invalidatedPointers.erase(data);
      state.moves.reinitialize(data);
      state.safety->buffers.set(
          data, {.shape = shape,
                 .object = *object,
                 .length = places.field(*object, shape.length.name),
                 .capacity = places.field(*object, shape.capacity.name),
                 .initialized = true});
    } else if (post.on) {
      if (const auto result = numericCallResult(call)) {
        core::BufferPost guarantee{
            .fact = {.shape = shape,
                     .object = *object,
                     .length = places.field(*object, shape.length.name),
                     .capacity = places.field(*object, shape.capacity.name),
                     .initialized = true},
            .when = {}};
        guarantee.when.require(*result, core::ValueFact::of(*post.on));
        auto &entries = state.safety->buffers.pending[data];
        if (std::ranges::find(entries, guarantee) == entries.end()) {
          if (entries.size() < core::MaxBufferShapes)
            entries.push_back(std::move(guarantee));
          else
            state.safety->buffers.limited = true;
        }
      }
    }
    const auto bound =
        std::ranges::find_if(bufferPostBounds[&call], [&](const auto &entry) {
          auto captured = post;
          captured.family = entry.first.family;
          return captured == entry.first;
        });
    if (bound != bufferPostBounds[&call].end() &&
        post.end != core::PathAffine::ofConstant(0)) {
      core::PlaceGuard when;
      if (!active) {
        const auto result = numericCallResult(call);
        if (!result || !post.on)
          continue;
        when.require(*result, core::ValueFact::of(*post.on));
      }
      auto &bounds = state.safety->buffers.bounds[data];
      const core::BufferCapacityBound lower{
          .capacity = places.field(*object, shape.capacity.name),
          .minimum = bound->second,
          .when = when};
      if (std::ranges::find(bounds, lower) == bounds.end()) {
        if (bounds.size() < core::MaxBufferShapes)
          bounds.push_back(lower);
        else
          state.safety->buffers.limited = true;
      }
    }
  }
  materializeBuffers(state);
}

void FunctionDataflow::bufferOutputs(core::CheckedContract &outputs,
                                     const core::AnalysisState &state,
                                     std::optional<core::Outcome> outcome) {
  for (const auto &[data, fact] : state.safety->buffers.values) {
    if (!fact.initialized || !bufferFact(data, state))
      continue;
    const auto path = builder.summaryPathOf(fact.object);
    if (!path || path->isResult() || (path->isParam() && path->isRoot()))
      continue;
    outputs.establish({.kind = core::CheckedRequirementKind::Buffer,
                       .path = *path,
                       .other = {},
                       .family = fact.shape.encode(),
                       .on = outcome});
    auto basic = fact.shape;
    basic.ownsBacking = false;
    basic.ownsElements = false;
    basic.terminated = false;
    outputs.establish({.kind = core::CheckedRequirementKind::Buffer,
                       .path = *path,
                       .other = {},
                       .family = basic.encode(),
                       .on = outcome});
    // Publish capability projections as well as the strongest predicate, so
    // intersection across empty/nonempty return classes retains backing
    // ownership even when their element capabilities differ.
    for (unsigned capabilities = 1; capabilities < 8; ++capabilities) {
      auto projected = basic;
      projected.ownsBacking = (capabilities & 1U) != 0;
      projected.ownsElements = (capabilities & 2U) != 0;
      projected.terminated = (capabilities & 4U) != 0;
      if (fact.shape.entails(projected))
        outputs.establish({.kind = core::CheckedRequirementKind::Buffer,
                           .path = *path,
                           .other = {},
                           .family = projected.encode(),
                           .on = outcome});
    }
    const auto sequence = state.safety->buffers.sequences.find(data);
    const auto entry = bufferEntries.find(data);
    if (sequence != state.safety->buffers.sequences.end() &&
        entry != bufferEntries.end() &&
        sequence->second.origin == entry->second.origin) {
      const auto expected = core::Affine::ofPlace(
          sequence->second.length, 1, sequence->second.appended ? 1 : 0);
      const auto actual = core::Affine::ofPlace(fact.length);
      if (checkedAtMost(expected, actual, state) &&
          checkedAtMost(actual, expected, state)) {
        const auto source =
            sequence->second.appended
                ? stableSummaryPathOf(*sequence->second.appended)
                : path;
        if (source)
          outputs.establish(
              {.kind = sequence->second.appended
                           ? core::CheckedRequirementKind::BufferAppended
                           : core::CheckedRequirementKind::BufferPreserved,
               .path = *path,
               .other = *source,
               .family = basic.encode(),
               .on = outcome});
      }
    }
    for (const auto *param : function.parameters()) {
      if (!param->getType()->isIntegerType())
        continue;
      const auto place = builder.placeForVar(*param);
      const auto saved = numericEntryValues.find(place);
      const auto input =
          saved == numericEntryValues.end() ? place : saved->second;
      const auto bound = core::Affine::ofPlace(input);
      const auto projected = summaryAffineOf(bound);
      if (projected && checkedAtMost({}, bound, state) &&
          checkedAtMost(bound, core::Affine::ofPlace(fact.capacity), state))
        outputs.establish({.kind = core::CheckedRequirementKind::Buffer,
                           .path = *path,
                           .other = {},
                           .end = *projected,
                           .family = fact.shape.encode(),
                           .on = outcome});
    }
  }
}

void FunctionDataflow::invalidateBufferCall(const CallExpr &call,
                                            const CallEffects *effects,
                                            core::AnalysisState &state) {
  if (!effects || !effects->summary ||
      std::ranges::any_of(effects->summary->effects, [](const auto &entry) {
        return entry.second.written || entry.second.consumed();
      }))
    state.safety->buffers.pending.clear();
  if (state.safety->buffers.values.empty() &&
      state.safety->buffers.storage.empty())
    return;
  if (!effects || !effects->summary) {
    state.safety->buffers.values.clear();
    state.safety->buffers.pending.clear();
    state.safety->buffers.sequences.clear();
    state.safety->buffers.pendingSequences.clear();
    state.safety->buffers.storage.clear();
    return;
  }
  std::vector<core::PlaceId> retire;
  for (const auto &[path, effect] : effects->summary->effects) {
    if (!effect.written && !effect.consumed())
      continue;
    const auto ref = builder.resolveSummaryPath(path, call, true);
    if (!ref) {
      state.safety->buffers.values.clear();
      state.safety->buffers.pending.clear();
      state.safety->buffers.sequences.clear();
      state.safety->buffers.pendingSequences.clear();
      state.safety->buffers.storage.clear();
      return;
    }
    for (const auto &[data, shape] : bufferObjects) {
      const auto holder = places.field(data, shape.data.name);
      if (ref->place == holder ||
          places.isDescendantOf(ref->place, places.deref(holder)) ||
          places.isDescendantOf(holder, ref->place)) {
        state.safety->buffers.sequences.erase(holder);
        state.safety->buffers.pendingSequences.erase(holder);
      }
    }
    std::erase_if(state.safety->buffers.storage, [&](const auto &entry) {
      const auto &[data, fact] = entry;
      return std::ranges::any_of(
          std::array{data, fact.object, fact.capacity}, [&](const auto place) {
            return ref->place == place ||
                   places.isDescendantOf(place, ref->place) ||
                   (effect.consumed() &&
                    state.aliases.mayAlias(data, ref->place));
          });
    });
    for (const auto &[data, fact] : state.safety->buffers.values)
      for (const auto place : {data, fact.object, fact.length, fact.capacity})
        if (ref->place == place || places.isDescendantOf(place, ref->place) ||
            (effect.consumed() && state.aliases.mayAlias(data, ref->place)))
          retire.push_back(data);
  }
  for (const auto data : retire)
    state.safety->buffers.values.erase(data);
}

void FunctionDataflow::checkedBufferRelease(
    core::PlaceId data, const CallExpr &call,
    const core::FunctionSummary *summary, core::AnalysisState &state) {
  const auto *fact = bufferFact(data, state);
  if (!fact || !fact->shape.ownsElements ||
      foldAffine(core::Affine::ofPlace(fact->length), state) ==
          core::Affine::ofConstant(0))
    return;
  bool discharged = false;
  if (summary && summary->checked.complete()) {
    const auto preserved = [&](std::optional<core::Outcome> outcome) {
      return std::ranges::any_of(
          summary->checked.establishes, [&](const auto &post) {
            if ((post.kind != core::CheckedRequirementKind::BufferPreserved &&
                 post.kind != core::CheckedRequirementKind::BufferAppended) ||
                (post.on && post.on != outcome) || !post.when.trivial())
              return false;
            const auto object = bufferArgument(post, call, state);
            return object && *object == fact->object;
          });
    };
    discharged =
        preserved(std::nullopt) ||
        (!summary->outcomes.empty() &&
         std::ranges::all_of(summary->outcomes, [&](const auto &entry) {
           return preserved(entry.first);
         }));
    for (const auto &release : summary->arrayReleases) {
      const auto storage =
          builder.resolveSummaryPath(release.storage, call, true);
      const auto count = builder.affineFromPath(release.count, call);
      const auto guard = checkedGuard(release.when, call, state);
      const auto length = core::Affine::ofPlace(fact->length);
      if (storage &&
          (storage->place == data || storage->place == places.deref(data)) &&
          release.begin == core::PathAffine::ofConstant(0) && count && guard &&
          guard->trivial() && checkedAtMost(*count, length, state) &&
          checkedAtMost(length, *count, state))
        discharged = true;
    }
  }
  if (!discharged)
    safetyObligation(core::SafetyProperty::Resource,
                     core::SafetyOutcome::Unresolved, call, "buffer ownership",
                     "owned buffer elements require complete release or "
                     "transfer before container mutation");
}
} // namespace weavec::analysis
