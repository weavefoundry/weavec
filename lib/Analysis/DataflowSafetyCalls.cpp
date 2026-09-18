//===- DataflowSafetyCalls.cpp - Discharging contracts (RFC 0018) ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"
#include "weavec/Analysis/ProgramDatabase.h"

#include "llvm/ADT/ScopeExit.h"

#include <array>
#include <utility>

using namespace clang;

namespace weavec::analysis {

void FunctionDataflow::checkedCall(const CallExpr &call,
                                   const CallEffects *effects,
                                   core::AnalysisState &state,
                                   std::string_view target) {
  if (target.empty() && effects && checkedAlternatives(call, *effects, state))
    return;
  containerArguments.erase(&call);
  containerPosts.erase(&call);
  containerReadOnlyCalls.erase(&call);
  containerReleases.erase(&call);
  containerPayloadReleases.erase(&call);
  containerLocalReleases.erase(&call);
  footprintPosts.erase(&call);
  freshFootprintSlots.erase(&call);
  freshFootprintSlotParents.erase(&call);
  checkedCallAssignedPointers.clear();
  checkedWrites.erase(&call);
  checkedPosts.erase(&call);
  checkedPositionPosts.erase(&call);
  checkedProgressPosts.erase(&call);
  checkedSpanPosts.erase(&call);
  bufferAllocationSequences.erase(&call);
  const auto *callee = call.getDirectCallee();
  std::string name = callee ? callee->getNameAsString() : "indirect call";
  if (!callee && effects && effects->source == SummarySource::Builtin)
    if (const auto targets = callTargetsSeen.find(&call);
        targets != callTargetsSeen.end() && !targets->second.unknown &&
        !targets->second.null && targets->second.functions.size() == 1)
      name = *targets->second.functions.begin();
  if (!target.empty())
    name = target;
  if (callee && callee->getBuiltinID() != 0) {
    if (name.starts_with("__builtin___") && name.ends_with("_chk"))
      name = name.substr(12, name.size() - 16);
    else if (name.starts_with("__builtin_") &&
             name != "__builtin_object_size" &&
             name != "__builtin_dynamic_object_size")
      name = name.substr(10);
    if (name == "__builtin_object_size" ||
        name == "__builtin_dynamic_object_size") {
      safetyObligation(core::SafetyProperty::Call, core::SafetyOutcome::Proven,
                       call, name, "compiler object-size query");
      return;
    }
  }
  if (checkedRuntimeCall(call, effects, state, name))
    return;
  const auto obligation = [&](core::SafetyProperty property, bool proved,
                              bool required, const std::string &reason) {
    safetyObligation(property, core::safetyOutcome(proved, required), call,
                     name, reason);
  };
  const auto liveAllocation =
      [&](const std::optional<core::ResourceRecord> &resource) {
        if (!resource || resource->origin != core::ResourceOrigin::Allocated ||
            resource->escaped)
          return false;
        auto guard = resource->guard;
        return pruneGuard(guard, state) && guard.trivial();
      };
  const auto separate = [&](const CheckedMemory &left,
                            const CheckedMemory &right) {
    for (const auto holder : {left.holder, right.holder})
      if (holder && (state.moves.recordOf(*holder) ||
                     state.safety->invalidatedPointers.contains(*holder)))
        return false;
    if (left.storage == right.storage)
      return checkedInterval(left.end, left.end, right.begin, state) ||
             checkedInterval(right.end, right.end, left.begin, state);
    const auto a = state.resources.recordOf(left.holder.value_or(left.storage));
    const auto b =
        state.resources.recordOf(right.holder.value_or(right.storage));
    const bool localA = isLocalStorage(left.storage);
    const bool localB = isLocalStorage(right.storage);
    const bool literalA =
        left.pointer && isa<StringLiteral>(left.pointer->IgnoreParenImpCasts());
    const bool literalB =
        right.pointer &&
        isa<StringLiteral>(right.pointer->IgnoreParenImpCasts());
    if (localA && localB)
      return places.root(left.storage) != places.root(right.storage);
    if ((localA &&
         (right.input || right.inputPlace || liveAllocation(b) || literalB)) ||
        (localB &&
         (left.input || left.inputPlace || liveAllocation(a) || literalA)) ||
        (literalA && liveAllocation(b)) || (literalB && liveAllocation(a)))
      return true;
    if ((!left.input && !left.inputPlace && liveAllocation(a) &&
         (right.input || right.inputPlace)) ||
        (!right.input && !right.inputPlace && liveAllocation(b) &&
         (left.input || left.inputPlace)))
      return true;
    const auto freshObject = [&](core::PlaceId storage) {
      return std::ranges::any_of(checkedObjects, [&](const auto &entry) {
        return entry.second == storage;
      });
    };
    if ((freshObject(left.storage) && (right.input || right.inputPlace)) ||
        (freshObject(right.storage) && (left.input || left.inputPlace)))
      return true;
    // RFC 0029: an allocation identity acquired by this invocation remains
    // separate from its automatic objects after attachment retires the
    // allocation's resource record. Validity stays an independent obligation.
    if ((localA && freshObject(right.storage)) ||
        (localB && freshObject(left.storage)))
      return true;
    return liveAllocation(a) && liveAllocation(b) && a->location != b->location;
  };
  const auto valid = [&](const CheckedMemory &memory) {
    const bool proved = checkedValid(memory, state);
    const bool required = (!proved || memory.input) &&
                          checkedRequire(core::CheckedRequirementKind::Valid,
                                         memory, call, state);
    obligation(core::SafetyProperty::Validity, proved && !required, required,
               "call requires live non-null storage");
    return proved || required;
  };
  const auto interval = [&](unsigned arg, const core::Affine &bytes, bool read,
                            bool write) {
    if (arg >= call.getNumArgs())
      return false;
    const auto memory = checkedMemory(
        *call.getArg(arg), core::Affine::ofConstant(0), bytes, state);
    if (!memory) {
      obligation(core::SafetyProperty::Bounds, false, false,
                 "call memory interval is not represented");
      return false;
    }
    bool complete = valid(*memory);
    const bool bounds =
        memory->extent &&
        checkedInterval(memory->begin, memory->end, *memory->extent, state);
    const bool required = (!bounds || memory->input) &&
                          checkedRequire(core::CheckedRequirementKind::Extent,
                                         *memory, call, state);
    obligation(core::SafetyProperty::Bounds, bounds, required,
               "call interval must fit its object");
    complete &= bounds || required;
    if (read) {
      const bool initialized = checkedInitialized(*memory, state);
      const bool input =
          !initialized &&
          checkedRequire(core::CheckedRequirementKind::Initialized, *memory,
                         call, state);
      obligation(core::SafetyProperty::Initialization, initialized, input,
                 "call input interval must be initialized");
      complete &= initialized || input;
    }
    if (write)
      complete &= checkedWrite(*memory, call, state);
    if (write && complete)
      checkedWrites[&call].push_back(*memory);
    return complete;
  };
  const bool builtin =
      (effects != nullptr) && effects->source == SummarySource::Builtin;
  static constexpr auto Modeled = std::to_array<std::string_view>(
      {"malloc", "calloc",  "realloc", "reallocarray", "free",
       "fclose", "abort",   "exit",    "_Exit",        "memset",
       "memcpy", "memmove", "strlen",  "strnlen",      "strcpy",
       "stpcpy", "strcat",  "strdup",  "strndup",      "strncpy"});
  if (builtin && std::ranges::find(Modeled, name) != Modeled.end())
    safetyObligation(core::SafetyProperty::Call, core::SafetyOutcome::Trusted,
                     call, name, "modeled C library contract");
  if (builtin && (name == "malloc" || name == "calloc" || name == "abort" ||
                  name == "exit" || name == "_Exit")) {
    safetyObligation(core::SafetyProperty::Call, core::SafetyOutcome::Trusted,
                     call, name, "modeled C library contract");
    return;
  }
  if (builtin && (name == "realloc" || name == "reallocarray")) {
    auto bytes = call.getNumArgs() >= 2 ? builder.affineOf(*call.getArg(1))
                                        : std::nullopt;
    if (name == "reallocarray") {
      const auto size = call.getNumArgs() >= 3
                            ? builder.affineOf(*call.getArg(2))
                            : std::nullopt;
      if (size && bytes && size->isConstant())
        bytes = bytes->times(size->constant);
      else if (size && bytes && bytes->isConstant())
        bytes = size->times(bytes->constant);
      else
        bytes.reset();
    }
    if (!bytes || call.getNumArgs() < 2) {
      obligation(core::SafetyProperty::Bounds, false, false,
                 "reallocation size is not represented");
      return;
    }
    const bool positive =
        checkedInterval(core::Affine::ofConstant(1),
                        core::Affine::ofConstant(1), *bytes, state);
    obligation(core::SafetyProperty::Bounds, positive, false,
               "checked reallocation requires a positive size");
    const auto origin = builder.classifyValue(*call.getArg(0));
    const auto memory = checkedMemory(*call.getArg(0), {}, {}, state);
    bool releasable =
        origin.kind == ValueOrigin::Kind::Null ||
        (origin.place &&
         state.nulls.stateOf(origin.place->place) == core::Nullness::Null);
    if (memory) {
      const auto holder = memory->holder.value_or(memory->storage);
      const auto resource = state.resources.recordOf(holder);
      if (const auto *buffer = bufferFact(holder, state))
        releasable |= buffer->shape.ownsBacking &&
                      memory->begin == core::Affine::ofConstant(0);
      releasable |= resource && resource->family == "free" &&
                    !resource->escaped && !state.moves.recordOf(holder) &&
                    memory->begin == core::Affine::ofConstant(0);
    }
    const bool required = memory && !releasable &&
                          checkedRequire(core::CheckedRequirementKind::Release,
                                         *memory, call, state, "free");
    obligation(core::SafetyProperty::Release, releasable, required,
               "reallocation requires a live allocation base");
    if (positive && (releasable || required))
      prepareReallocationFootprint(call, state);
    if (positive && releasable)
      if (const auto ref = builder.resolve(*call.getArg(0))) {
        const auto *buffer = bufferFact(ref->place, state);
        const auto sequence = state.safety->buffers.sequences.find(ref->place);
        if (buffer && sequence != state.safety->buffers.sequences.end() &&
            foldAffine(core::Affine::ofPlace(buffer->length), state) ==
                core::Affine::ofConstant(0))
          bufferAllocationSequences[&call] = sequence->second;
      }
    if (positive && (releasable || required) && memory && memory->holder) {
      const auto data = *memory->holder;
      const auto *buffer = bufferFact(data, state);
      const auto sequence = state.safety->buffers.sequences.find(data);
      if (buffer && sequence != state.safety->buffers.sequences.end() &&
          checkedAtMost(core::Affine::ofPlace(buffer->length,
                                              static_cast<std::int64_t>(
                                                  buffer->shape.elementBytes)),
                        *bytes, state))
        bufferAllocationSequences[&call] = sequence->second;
    }
    if (positive && (releasable || required) && memory)
      for (const auto &range : checkedCopyRanges(*memory, {}, *bytes, state))
        checkedPosts[&call].push_back({.path = core::SummaryPath::result(),
                                       .range = range,
                                       .on = core::Outcome::NonNull,
                                       .storage = {},
                                       .objectType = {}});
    return;
  }
  if (builtin && (name == "strlen" || name == "strnlen" || name == "strcpy" ||
                  name == "stpcpy" || name == "strcat" || name == "strdup" ||
                  name == "strndup" || name == "strncpy")) {
    const bool copy = name == "strcpy" || name == "stpcpy" ||
                      name == "strcat" || name == "strncpy";
    const unsigned source = copy ? 1U : 0U;
    if (call.getNumArgs() <= source)
      return;
    if (name == "strlen") {
      core::CheckedContract input;
      input.require({.kind = core::CheckedRequirementKind::Terminated,
                     .path = core::SummaryPath::param(0),
                     .other = {},
                     .begin = {},
                     .end = {},
                     .family = {}});
      prepareCheckedStringInputs(call, input, state);
      checkedStringLength(call, state);
    }
    const auto length = stringLengthOf(*call.getArg(source), state);
    const auto throughNul = length ? length->shifted(1) : std::nullopt;
    std::optional<core::Affine> count;
    if (name == "strnlen" || name == "strndup" || name == "strncpy") {
      const unsigned arg = name == "strncpy" ? 2U : 1U;
      if (arg < call.getNumArgs())
        count = builder.affineOf(*call.getArg(arg));
    }
    auto read = throughNul;
    if (count) {
      read = count;
      if (throughNul && checkedInterval({}, *throughNul, *count, state))
        read = throughNul;
    }
    bool sourceOK = false;
    if (read) {
      sourceOK = interval(source, *read, true, false);
      if (!count)
        obligation(core::SafetyProperty::Initialization, length.has_value(),
                   false, "string input must have an initialized terminator");
    } else {
      const auto memory = checkedMemory(*call.getArg(source), {}, {}, state);
      const bool terminated = memory && checkedTerminated(*memory, state);
      const bool required =
          !terminated && memory &&
          checkedRequire(core::CheckedRequirementKind::Terminated, *memory,
                         call, state);
      obligation(core::SafetyProperty::Initialization, terminated, required,
                 "string input must have an initialized terminator");
      sourceOK = terminated || required;
    }
    if (copy) {
      auto written = name == "strncpy" ? count : throughNul;
      if (name == "strcat") {
        const auto before = stringLengthOf(*call.getArg(0), state);
        const auto prefix = before ? before->shifted(1) : std::nullopt;
        sourceOK &= prefix && interval(0, *prefix, true, false);
        written =
            before && throughNul ? sumOf(*before, *throughNul) : std::nullopt;
      }
      if (written)
        interval(0, *written, false, sourceOK);
      else
        obligation(core::SafetyProperty::Bounds, false, false,
                   "string output interval is not represented");
      // Overlap is undefined for these copying routines.
      const auto left = checkedMemory(*call.getArg(0), {},
                                      written.value_or(core::Affine{}), state);
      const auto right = checkedMemory(*call.getArg(source), {},
                                       read.value_or(core::Affine{}), state);
      bool separated = false;
      if (left && right && left->storage != right->storage) {
        const auto *a = builder.varForPlace(left->storage);
        const auto *b = builder.varForPlace(right->storage);
        separated = (a != nullptr) && !a->getType()->isPointerType() &&
                    (((b != nullptr) && !b->getType()->isPointerType()) ||
                     builder.isLiteralPlace(right->storage));
        separated |= liveAllocation(state.resources.recordOf(
                         left->holder.value_or(left->storage))) &&
                     builder.isLiteralPlace(right->storage);
      }
      if (left && right && left->storage == right->storage && written && read)
        separated =
            checkedInterval(left->end, left->end, right->begin, state) ||
            checkedInterval(right->end, right->end, left->begin, state);
      obligation(core::SafetyProperty::Aliasing, separated, false,
                 "string copy intervals must be disjoint");
    } else if ((name == "strdup" || name == "strndup") && sourceOK) {
      auto output = throughNul;
      if (name == "strndup" && count) {
        const auto maximum = count->shifted(1);
        if (!output ||
            (maximum && checkedInterval({}, *maximum, *output, state)))
          output = maximum;
      }
      if (output)
        checkedPosts[&call].push_back({.path = core::SummaryPath::result(),
                                       .range = {.begin = {}, .end = *output},
                                       .on = core::Outcome::NonNull,
                                       .storage = {},
                                       .objectType = {}});
    }
    return;
  }
  if (builtin && (name == "free" || name == "fclose")) {
    if (call.getNumArgs() == 0)
      return;
    if (name == "free" && checkedContainerRelease(call, state))
      return;
    const auto origin = builder.classifyValue(*call.getArg(0));
    bool released = origin.kind == ValueOrigin::Kind::Null && name == "free";
    bool required = false;
    if (origin.place) {
      const auto place = origin.place->place;
      if (name == "free")
        checkedBufferRelease(place, call, nullptr, state);
      const auto nullness = nullnessAt(place, state);
      released |=
          name == "free" && nullness && nullness->state == core::Nullness::Null;
      const auto resource = state.resources.recordOf(place);
      const auto spatial = state.spatial.recordOf(place);
      if (const auto *buffer = bufferFact(place, state))
        released |= name == "free" && buffer->shape.ownsBacking &&
                    origin.offset.isZero();
      released |=
          resource && !resource->escaped && !state.moves.recordOf(place) &&
          !state.safety->invalidatedPointers.contains(place) &&
          resource->family == (name == "free" ? "free" : "fclose") &&
          origin.offset.isZero() && (!spatial || spatial->offset.isZero());
      {
        const auto memory =
            checkedMemory(*call.getArg(0), core::Affine::ofConstant(0),
                          core::Affine::ofConstant(0), state);
        required =
            memory &&
            checkedRequire(core::CheckedRequirementKind::Release, *memory, call,
                           state, name == "free" ? "free" : "fclose");
      }
    }
    obligation(
        core::SafetyProperty::Release, released && !required, required,
        "release requires a live allocation base of the matching family");
    if (name == "free" && (released || required) && origin.place &&
        origin.offset.isZero())
      recordAllocationConsumed(origin.place->place, state);
    return;
  }
  if (builtin && (name == "memset" || name == "memcpy" || name == "memmove")) {
    const auto bytes = call.getNumArgs() >= 3
                           ? builder.affineOf(*call.getArg(2))
                           : std::nullopt;
    if (!bytes) {
      obligation(core::SafetyProperty::Bounds, false, false,
                 "memory operation length is not represented");
      return;
    }
    // Byte initialization is not a proof that pointer cells retained their
    // identities or that their separately owned pointees were released.
    // Conservatively retire sequence evidence across byte-level mutation.
    for (auto &[data, fact] : state.safety->buffers.values) {
      if (!fact.shape.pointerElements)
        continue;
      checkedBufferRelease(data, call, nullptr, state);
      fact.shape.ownsElements = false;
    }
    state.safety->buffers.sequences.clear();
    state.safety->buffers.pendingSequences.clear();
    state.safety->buffers.pending.clear();
    bool source = true;
    if (name != "memset")
      source = interval(1, *bytes, true, false);
    interval(0, *bytes, false, source);
    if (source && name != "memset") {
      const auto input = checkedMemory(*call.getArg(1), {}, *bytes, state);
      if (input && checkedNumericText(*input, state)) {
        // Capture both the byte contents and every endpoint before the copy's
        // effects, including an aliased length cell. This local primitive
        // contract uses the normal snapshot and postcondition machinery.
        core::CheckedContract copy;
        copy.computed = true;
        copy.establish(
            {.kind = core::CheckedRequirementKind::Copied,
             .path = core::SummaryPath::param(0),
             .other = core::SummaryPath::param(1),
             .end = core::PathAffine::ofPath(core::SummaryPath::param(2)),
             .family = {}});
        captureCheckedPosts(call, copy, state);
      }
    }
    if (name == "memcpy") {
      const auto left = checkedMemory(*call.getArg(0), {}, *bytes, state);
      const auto right = checkedMemory(*call.getArg(1), {}, *bytes, state);
      const bool separated = left && right && separate(*left, *right);
      const auto leftInput =
          left ? checkedSeparationInput(*left, state) : std::nullopt;
      const auto rightInput =
          right ? checkedSeparationInput(*right, state) : std::nullopt;
      const bool required =
          !separated && leftInput && rightInput && leftInput != rightInput;
      if (required && recording())
        inferred.checked.require(
            {.kind = core::CheckedRequirementKind::Separated,
             .path = *leftInput,
             .other = *rightInput,
             .family = {}});
      obligation(core::SafetyProperty::Aliasing, separated, required,
                 "memcpy intervals must be disjoint");
    }
    return;
  }
  if (effects && effects->source == SummarySource::Annotation)
    safetyObligation(core::SafetyProperty::Call, core::SafetyOutcome::Trusted,
                     call, "annotation:" + name,
                     "declared ownership annotation contract");
  const auto *contract =
      effects && effects->summary ? &effects->summary->checked : nullptr;
  if (!contract || !contract->computed) {
    if (callee && !callee->hasBody() && getAnnotations(*callee).unsafe) {
      safetyObligation(core::SafetyProperty::Call, core::SafetyOutcome::Trusted,
                       call, name, "explicit unsafe external contract");
      return;
    }
    const auto *external = callee;
    if (!external && !target.empty())
      external = summaries.callable(target);
    if (!external)
      if (const auto targets = callTargetsSeen.find(&call);
          targets != callTargetsSeen.end() && !targets->second.unknown &&
          !targets->second.null && targets->second.functions.size() == 1)
        external = summaries.callable(*targets->second.functions.begin());
    if (options.deferCheckedCalls && external && !external->hasBody() &&
        !builtin) {
      checkedDeferredCalls.insert(&call);
      if (recording())
        inferred.checked.deferred = true;
      for (const auto *argument : call.arguments())
        if (argument->getType()->isPointerType())
          if (const auto memory = checkedMemory(*argument, {}, {}, state)) {
            state.safety->deferred.insert(memory->storage);
            // RFC 0026: an unavailable mutator may replace a reachable
            // buffer's backing pointer. Its old null/count values cannot
            // decide post-call accesses during the compile-only phase.
            for (const auto &[object, shape] : bufferObjects) {
              if (object != memory->storage &&
                  !places.isDescendantOf(object, memory->storage))
                continue;
              const auto data = places.field(object, shape.data.name);
              for (const auto &field :
                   {shape.data, shape.length, shape.capacity})
                state.safety->deferred.insert(places.field(object, field.name));
              state.safety->deferred.insert(places.deref(data));
              state.nulls.forget(data);
            }
          }
      safetyObligation(core::SafetyProperty::Call,
                       core::SafetyOutcome::Required, call, name,
                       "external checked contract deferred until link");
      return;
    }
    obligation(core::SafetyProperty::Call, false, false,
               "callee has no complete checked contract");
    return;
  }
  if (callee &&
      contract->signature != functionTypeKey(callee->getType(), context))
    obligation(core::SafetyProperty::Call, false, false,
               "callee C signature differs from its checked definition");
  const bool pendingExternal =
      options.deferCheckedCalls && contract->deferred &&
      contract->obligations.complete() && !contract->limited;
  if (pendingExternal && recording())
    inferred.checked.deferred = true;
  prepareCheckedStringInputs(call, *contract, state);
  forwardCheckedCaseInputs(call, *contract);
  if (effects && effects->summary &&
      std::ranges::any_of(effects->summary->effects, [](const auto &entry) {
        return entry.second.written;
      }))
    for (const auto *argument : call.arguments())
      if (argument->getType()->isPointerType())
        if (const auto memory = checkedMemory(*argument, {}, {}, state)) {
          state.safety->writtenStorage.insert(memory->storage);
          for (const auto &[holder, position] : state.safety->positions)
            if (holder == memory->storage ||
                places.isDescendantOf(holder, memory->storage))
              state.safety->writtenStorage.insert(position.storage);
        }
  // RFC 0020: propagation only writes the final explanation ledger. During
  // transfer iterations safetyObligation is a no-op; avoid constructing and
  // escaping thousands of discarded call paths on those iterations.
  if (recording()) {
    const auto &origins = contract->obligations.propagation();
    const auto location = locate(call);
    const auto caller = function.getNameAsString();
    const auto propagate = [&](bool trusted) {
      inferred.checked.obligations.addCalls(contract->obligations, trusted,
                                            location, caller, name, inUnsafe);
    };
    if (!contract->complete() && !pendingExternal) {
      propagate(false);
      if (origins.unresolved.empty())
        obligation(core::SafetyProperty::Call, false, false,
                   "callee checked contract is incomplete");
    } else {
      obligation(core::SafetyProperty::Call, true, false,
                 "callee has a complete checked contract");
    }
    propagate(true);
  }
  // A requirement check reads the caller state without executing the callee.
  // Conditioning changes these domains through AnalysisState::learn/narrow;
  // checkedRequire can additionally establish pointer validity and capacity.
  // Keep the remaining ownership, heap and memory state in place: copying its
  // histories for each conditional obligation is particularly costly in SCCs.
  struct CheckedRequirementState {
    core::MoveTracker moves;
    core::ResourceTracker resources;
    core::FootprintRelations footprints;
    core::NullTracker nulls;
    core::ScalarTracker scalars;
    core::PlaceGuard numericConditions;
    core::PlaceGuard pointerFacts;
    std::set<core::PlaceId> pointers;
    std::map<core::PlaceId, core::Affine> accessible;
    std::map<core::PlaceId, std::string> objectTypes;

    explicit CheckedRequirementState(const core::AnalysisState &state)
        : moves(state.moves), resources(state.resources),
          footprints(state.safety->footprints), nulls(state.nulls),
          scalars(state.scalars), numericConditions(state.numericConditions),
          pointerFacts(state.pointerFacts), pointers(state.safety->pointers),
          accessible(state.safety->accessible),
          objectTypes(state.safety->objectTypes) {}

    void restore(core::AnalysisState &state) {
      state.moves = std::move(moves);
      state.resources = std::move(resources);
      state.safety->footprints = std::move(footprints);
      state.nulls = std::move(nulls);
      state.scalars = std::move(scalars);
      state.numericConditions = std::move(numericConditions);
      state.pointerFacts = std::move(pointerFacts);
      state.safety->pointers = std::move(pointers);
      state.safety->accessible = std::move(accessible);
      state.safety->objectTypes = std::move(objectTypes);
    }
  };

  // Discover and fold caller evidence before temporarily assuming a callee's
  // antecedent. Guarded requirement probes must not publish unconditional
  // buffer facts or numeric relations (RFC 0026).
  for (const auto &requirement : contract->requirements)
    if (requirement.kind == core::CheckedRequirementKind::Buffer)
      (void)bufferArgument(requirement, call, state);
  normalizeBuffers(state);
  if (effects && effects->summary)
    for (const auto &[path, effect] : effects->summary->effects)
      if (effect.consumed() || effect.written)
        if (const auto ref = builder.resolveSummaryPath(path, call, true)) {
          checkedBufferRelease(ref->place, call, effects->summary.get(), state);
          for (const auto &[data, fact] : state.safety->buffers.values)
            if (ref->place == fact.length || ref->place == fact.capacity ||
                ref->place == fact.object ||
                places.isDescendantOf(ref->place, places.deref(data)))
              checkedBufferRelease(data, call, effects->summary.get(), state);
        }
  // Requirements sharing an antecedent are a conjunction. Discharge them
  // together so translation, refinement and rollback happen once per guard.
  // Only independently proved or recorded entry facts can help the next
  // requirement; callee writes and outputs are still applied after checking.
  std::map<core::PathGuard, std::vector<const core::CheckedRequirement *>>
      groups;
  for (const auto &requirement : contract->requirements)
    groups[requirement.when].push_back(&requirement);
  for (const auto &[when, requirements] : groups) {
    auto condition = builder.translateGuard(when, call);
    if (!condition)
      continue;
    if (!pruneGuard(*condition, state))
      continue;
    std::optional<CheckedRequirementState> unconditioned;
    if (!condition->trivial()) {
      unconditioned.emplace(state);
      for (const auto &[place, fact] : condition->conditions) {
        if (!fact.isPointer())
          (void)state.scalars.narrow(place, fact);
        (void)state.learn(place, fact);
      }
      for (const auto &predicate : condition->integers)
        state.numericConditions.requireInteger(predicate);
      for (const auto &[pair, equal] : condition->pointers)
        state.pointerFacts.requirePointer(pair.first, pair.second, equal);
    }
    std::optional<core::PathGuard> projectedGuard;
    auto *previousGuard =
        std::exchange(checkedRequirementGuard, &projectedGuard);
    const auto restoreCondition = llvm::scope_exit([&] {
      checkedRequirementGuard = previousGuard;
      if (unconditioned)
        unconditioned->restore(state);
    });
    for (const auto *entry : requirements) {
      const auto &requirement = *entry;
      if (requirement.kind == core::CheckedRequirementKind::CallbackAllocate ||
          requirement.kind == core::CheckedRequirementKind::CallbackRelease) {
        checkedCallbackRequirement(requirement, call, state);
        continue;
      }
      if (requirement.kind == core::CheckedRequirementKind::Buffer) {
        checkedBufferCall(requirement, call, state);
        continue;
      }
      if (runtimeRequirement(requirement, call, state))
        continue;
      if (requirement.kind == core::CheckedRequirementKind::Container ||
          requirement.kind ==
              core::CheckedRequirementKind::ContainerSeparated) {
        checkedContainerCall(requirement, call, state);
        continue;
      }
      if (requirement.kind == core::CheckedRequirementKind::Separated) {
        const auto nullInput = [&](const core::SummaryPath &path) {
          if (path.isParam() && path.isRoot() &&
              path.index < call.getNumArgs() &&
              builder.classifyValue(*call.getArg(path.index)).kind ==
                  ValueOrigin::Kind::Null)
            return true;
          const auto ref = builder.resolveSummaryPath(path, call);
          return ref && state.nulls.stateOf(ref->place) == core::Nullness::Null;
        };
        if (nullInput(requirement.path) || nullInput(requirement.other)) {
          obligation(core::SafetyProperty::Aliasing, true, false,
                     "callee requires separated input objects");
          continue;
        }
      }
      if (requirement.kind == core::CheckedRequirementKind::InitializedSpan) {
        obligation(core::SafetyProperty::Bounds,
                   checkedSpanCall(requirement, call, state), false,
                   "callee requires a live initialized same-array byte span");
        continue;
      }
      if (requirement.kind == core::CheckedRequirementKind::SumFits) {
        const auto first = builder.affineFromPath(requirement.begin, call);
        const auto last = builder.affineFromPath(requirement.end, call);
        const auto a =
            first ? checkedByteExpression(*first, state) : std::nullopt;
        const auto b =
            last ? checkedByteExpression(*last, state) : std::nullopt;
        const bool proved = a && b &&
                            operationDoesNotOverflow(core::IntegerOp::Add, *a,
                                                     *b, a->type(), state);
        const auto begin = first ? summaryAffineOf(first) : std::nullopt;
        const auto end = last ? summaryAffineOf(last) : std::nullopt;
        const bool required = !proved && begin && end;
        if (required && recording())
          inferred.checked.require({.kind = requirement.kind,
                                    .path = {},
                                    .other = {},
                                    .begin = *begin,
                                    .end = *end,
                                    .family = {}});
        obligation(core::SafetyProperty::Arithmetic, proved, required,
                   "callee byte interval sum must not overflow");
        continue;
      }
      if (requirement.kind == core::CheckedRequirementKind::UnionMember) {
        const auto ref = builder.resolveSummaryPath(requirement.path, call);
        if (ref)
          checkedUnionRequirement(ref->place, requirement.family, call, state);
        else
          obligation(core::SafetyProperty::Initialization, false, false,
                     "union member requirement cannot be instantiated");
        continue;
      }
      const Expr *pointer = requirement.path.isParam() &&
                                    requirement.path.isRoot() &&
                                    requirement.path.index < call.getNumArgs()
                                ? call.getArg(requirement.path.index)
                                : nullptr;
      if (requirement.kind == core::CheckedRequirementKind::Release &&
          requirement.family == "free" && pointer &&
          builder.classifyValue(*pointer).kind == ValueOrigin::Kind::Null) {
        obligation(core::SafetyProperty::Release, true, false,
                   "free permits a null pointer");
        continue;
      }
      if (requirement.kind == core::CheckedRequirementKind::Valid && pointer &&
          builder.classifyValue(*pointer).kind == ValueOrigin::Kind::Null) {
        obligation(core::SafetyProperty::Validity, false, false,
                   "call requires live non-null storage");
        continue;
      }
      const auto first = builder.affineFromPath(requirement.begin, call);
      const auto last = builder.affineFromPath(requirement.end, call);
      const auto memory =
          first && last
              ? checkedPathMemory(requirement.path, call, *first, *last, state)
              : std::nullopt;
      if (!memory) {
        obligation(core::SafetyProperty::Call, false, false,
                   "checked requirement interval cannot be instantiated");
        continue;
      }
      const auto kind = requirement.kind;
      if (kind == core::CheckedRequirementKind::ObjectType) {
        checkedObjectRequirement(*memory, requirement.family, call, state);
        continue;
      }
      if (kind == core::CheckedRequirementKind::Writable) {
        checkedWrite(*memory, call, state);
        continue;
      }
      if (kind == core::CheckedRequirementKind::Valid) {
        valid(*memory);
        continue;
      }
      bool proved = false;
      auto property = core::SafetyProperty::Bounds;
      if (kind == core::CheckedRequirementKind::Extent) {
        proved = memory->extent && checkedInterval(memory->begin, memory->end,
                                                   *memory->extent, state);
      } else if (kind == core::CheckedRequirementKind::Initialized) {
        property = core::SafetyProperty::Initialization;
        proved = checkedInitialized(*memory, state);
      } else if (kind == core::CheckedRequirementKind::TerminatedWithin) {
        property = core::SafetyProperty::Initialization;
        auto bounded = *memory;
        bounded.extent = memory->end;
        proved = memory->extent &&
                 checkedInterval(memory->begin, memory->end, *memory->extent,
                                 state) &&
                 checkedTerminated(bounded, state);
      } else if (kind == core::CheckedRequirementKind::Terminated) {
        property = core::SafetyProperty::Initialization;
        const auto base =
            checkedPathMemory(requirement.path, call, {}, {}, state);
        const auto witness = checkedWitness(*memory, state);
        if (base && witness) {
          auto prefix = *base;
          const auto through = witness->zero.shifted(1);
          if (through) {
            prefix.end = *through;
            proved = checkedValid(prefix, state) && prefix.extent &&
                     checkedInterval(prefix.begin, prefix.end, *prefix.extent,
                                     state) &&
                     checkedInitialized(prefix, state);
          }
        }
        std::optional<core::Affine> length;
        if (pointer)
          length = stringLengthOf(*pointer, state);
        else if (const auto spatial = spatialRecordAt(
                     memory->holder.value_or(memory->storage), state);
                 spatial && spatial->string && !spatial->string->unterminated)
          length = spatial->string->length;
        if (length)
          if (const auto end = length->shifted(1)) {
            auto bytes = base.value_or(*memory);
            bytes.end = *end;
            proved |=
                checkedValid(bytes, state) && bytes.extent &&
                checkedAtMost(memory->begin, *length, state) &&
                checkedInterval(bytes.begin, bytes.end, *bytes.extent, state) &&
                checkedInitialized(bytes, state);
          }
      } else if (kind == core::CheckedRequirementKind::Release) {
        property = core::SafetyProperty::Release;
        const auto holder = memory->holder.value_or(memory->storage);
        const auto resource = state.resources.recordOf(holder);
        const auto spatial = state.spatial.recordOf(holder);
        const auto nullness = nullnessAt(holder, state);
        proved = requirement.family == "free" && nullness &&
                 nullness->state == core::Nullness::Null;
        if (const auto *buffer = bufferFact(holder, state))
          proved |= requirement.family == "free" && buffer->shape.ownsBacking &&
                    memory->begin == core::Affine::ofConstant(0);
        proved |= resource && !resource->escaped &&
                  !state.moves.recordOf(holder) &&
                  !state.safety->invalidatedPointers.contains(holder) &&
                  resource->family == requirement.family &&
                  (!spatial || spatial->offset.isZero());
      } else if (kind == core::CheckedRequirementKind::Separated) {
        if (const auto firstChain =
                builder.resolveSummaryPath(requirement.path, call))
          if (const auto secondChain =
                  builder.resolveSummaryPath(requirement.other, call);
              secondChain && state.safety->containers.separated(
                                 firstChain->place, secondChain->place)) {
            obligation(core::SafetyProperty::Aliasing, true, false,
                       "callee requires separated input objects");
            continue;
          }

        property = core::SafetyProperty::Aliasing;
        {
          const auto other =
              checkedPathMemory(requirement.other, call, {}, {}, state);
          if (other && other->storage != memory->storage) {
            proved = separate(*memory, *other);
            const auto leftInput = checkedSeparationInput(*memory, state);
            const auto rightInput = checkedSeparationInput(*other, state);
            if (!proved && leftInput && rightInput && leftInput != rightInput) {
              if (recording()) {
                auto exported = requirement;
                exported.path = *leftInput;
                exported.other = *rightInput;
                inferred.checked.require(std::move(exported));
              }
              obligation(property, false, true,
                         "callee requires separated input objects");
              continue;
            }
          }
        }
        obligation(property, proved, false,
                   "callee requires separated input objects");
        continue;
      }
      const bool required =
          (!proved || (memory->input &&
                       (kind == core::CheckedRequirementKind::Extent ||
                        kind == core::CheckedRequirementKind::Release))) &&
          checkedRequire(kind, *memory, call, state, requirement.family);
      obligation(property, proved, required,
                 "callee " + std::string(core::toString(kind)) +
                     " safety precondition must hold");
    }
  }
  captureCheckedPosts(call, *contract, state);
  invalidateBufferCall(call, effects, state);
}

void FunctionDataflow::checkedCallAfter(const CallExpr &call,
                                        const CallEffects *effects,
                                        core::AnalysisState &state) {
  if (const auto *callee = call.getDirectCallee();
      callee && callee->getBuiltinID() != 0 &&
      (callee->getName() == "__builtin_object_size" ||
       callee->getName() == "__builtin_dynamic_object_size"))
    return;
  const auto alternatives = checkedCallAlternatives.find(&call);
  const bool uncheckedAlternative =
      alternatives != checkedCallAlternatives.end() &&
      std::ranges::any_of(alternatives->second, [](const auto &entry) {
        const auto &actual = entry.second;
        return actual.source != SummarySource::Builtin &&
               !actual.summary->checked.computed;
      });
  // Unknown/unsafe calls may replace pointer representations and mutate every
  // reachable byte. Do not allow a trusted call to manufacture later evidence.
  if (uncheckedAlternative || !effects || !effects->summary ||
      (!effects->summary->checked.computed &&
       effects->source != SummarySource::Builtin) ||
      (effects->source != SummarySource::Builtin &&
       effects->summary->checked.obligations.trusted() &&
       std::ranges::any_of(
           effects->summary->checked.obligations.propagation().trusted,
           [](const auto &entry) {
             return entry.reason != "modeled C library contract" &&
                    entry.reason != "compiler object-size query";
           }))) {
    state.safety->unions.invalidateAll();
    state.safety->buffers.values.clear();
    state.safety->buffers.pending.clear();
    state.safety->buffers.sequences.clear();
    state.safety->buffers.pendingSequences.clear();
    state.safety->buffers.storage.clear();
    state.safety->memory.clear();
    for (auto &[storage, type] : state.safety->objectTypes) {
      (void)storage;
      type = "?";
    }
    state.safety->termination.clear();
    state.safety->boundedTermination.clear();
    state.safety->havoc = true;
    for (auto &[place, list] : state.safety->argumentLists) {
      list.phase = core::ArgumentListPhase::Unknown;
      if (options.deferCheckedCalls && checkedDeferredCalls.contains(&call))
        state.safety->deferred.insert(place);
    }
    for (const Expr *arg : call.arguments())
      if (arg->getType()->isPointerType())
        if (const auto memory = checkedMemory(*arg, {}, {}, state)) {
          if (builder.classifyValue(*arg).kind == ValueOrigin::Kind::Borrow)
            state.safety->initialized.erase(memory->storage);
          state.safety->memory.erase(memory->storage);
          state.safety->pointers.erase(memory->storage);
        }
  }
  const auto writes = checkedWrites.find(&call);
  if (writes != checkedWrites.end() ||
      (effects && effects->summary &&
       std::ranges::any_of(effects->summary->effects, [](const auto &entry) {
         return entry.second.written;
       }))) {
    // RFC 0029: a complete helper may change addressed automatic cells while
    // leaving separate byte objects intact. Resolve every actual destination;
    // unknown writes and escaping or consuming effects supply no frame.
    bool localWrites = effects != nullptr && effects->summary &&
                       effects->summary->checked.complete();
    std::set<core::PlaceId> writtenRoots;
    if (localWrites) {
      for (const auto &[path, effect] : effects->summary->effects) {
        if (effect.consumed() || effect.escaped || effect.replaced)
          localWrites = false;
        if (!effect.written)
          continue;
        const auto actual = builder.resolveSummaryPath(path, call);
        if (!actual || !isLocalStorage(actual->place)) {
          localWrites = false;
          break;
        }
        writtenRoots.insert(places.root(actual->place));
      }
      if (writes != checkedWrites.end())
        for (const auto &memory : writes->second) {
          localWrites &= isLocalStorage(memory.storage);
          writtenRoots.insert(places.root(memory.storage));
        }
    }
    std::vector<std::pair<core::PlaceId, core::InitializedRange>> retained;
    bool representedWrites = effects != nullptr && effects->summary &&
                             effects->summary->checked.complete();
    std::set<core::PlaceId> writtenObjects;
    if (representedWrites) {
      for (const auto &[path, effect] : effects->summary->effects) {
        if (!effect.written)
          continue;
        const auto actual = builder.resolveSummaryPath(path, call);
        if (!actual || !actual->element.isWhole()) {
          representedWrites = false;
          break;
        }
        auto storage = places.root(actual->place);
        if (const auto object = places.innermostDeref(actual->place)) {
          const auto holder = places.parent(*object);
          const auto memory =
              holder ? checkedMemoryAt(*holder, {}, {}, state) : std::nullopt;
          if (!memory) {
            representedWrites = false;
            break;
          }
          storage = memory->storage;
        }
        writtenObjects.insert(storage);
      }
      if (writes != checkedWrites.end())
        for (const auto &memory : writes->second)
          writtenObjects.insert(memory.storage);
    }
    if (representedWrites || (localWrites && !writtenRoots.empty()))
      for (const auto &[storage, ranges] : state.safety->memory) {
        bool separate = localWrites && isLocalStorage(storage) &&
                        !writtenRoots.contains(places.root(storage));
        if (localWrites && places.step(storage) == core::PathStep::Deref)
          if (const auto holder = places.parent(storage))
            if (const auto memory = checkedMemoryAt(*holder, {}, {}, state);
                memory && memory->storage == storage && memory->inputPlace &&
                storage == places.deref(*memory->inputPlace) &&
                !state.safety->replacedPointers.contains(*memory->inputPlace) &&
                checkedSeparationInput(*memory, state) &&
                checkedValid(*memory, state))
              separate = true;
        for (const auto &range : ranges)
          if (!range.bytes.empty() && !range.source &&
              (separate || (representedWrites && range.immutableBytes &&
                            !writtenObjects.contains(storage))))
            retained.emplace_back(storage, range);
      }
    for (auto &[data, fact] : state.safety->buffers.values) {
      (void)data;
      fact.shape.terminated = false;
    }
    state.forgetZeroedMemory();
    for (auto &[storage, range] : retained)
      state.safety->initialize(storage, std::move(range));
  }
  if (writes != checkedWrites.end()) {
    const auto *callee = call.getDirectCallee();
    const bool zeroed = (effects != nullptr) &&
                        effects->source == SummarySource::Builtin &&
                        (callee != nullptr) && call.getNumArgs() > 1 &&
                        (callee->getName() == "memset" ||
                         callee->getName() == "__builtin_memset" ||
                         callee->getName() == "__builtin___memset_chk") &&
                        integerConstant(*call.getArg(1), context) == 0;
    for (const auto &memory : writes->second) {
      state.safety->writtenStorage.insert(memory.storage);
      state.safety->initialize(
          memory.storage,
          {.begin = memory.begin, .end = memory.end, .zeroed = zeroed});
    }
    checkedWrites.erase(writes);
  }
  if (effects && effects->summary)
    for (const auto &[path, effect] : effects->summary->effects) {
      if (!effect.written)
        continue;
      const auto place = builder.resolveSummaryPath(path, call);
      if (!place || checkedCallAssignedPointers.contains(place->place))
        continue;
      state.safety->positions.erase(place->place);
      state.safety->pointers.erase(place->place);
      state.safety->replacedPointers.insert(place->place);
    }
  checkedContainersAfterCall(call, effects, state);
  if (effects && effects->summary)
    applyCheckedPosts(call, *effects->summary, state);
}

} // namespace weavec::analysis
