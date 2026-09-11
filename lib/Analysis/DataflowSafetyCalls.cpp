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
  checkedCallAssignedPointers.clear();
  checkedWrites.erase(&call);
  checkedPosts.erase(&call);
  checkedPositionPosts.erase(&call);
  checkedProgressPosts.erase(&call);
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
    if ((localA && (right.input || liveAllocation(b) || literalB)) ||
        (localB && (left.input || liveAllocation(a) || literalA)) ||
        (literalA && liveAllocation(b)) || (literalB && liveAllocation(a)))
      return true;
    if ((!left.input && liveAllocation(a) && right.input) ||
        (!right.input && liveAllocation(b) && left.input))
      return true;
    const auto freshObject = [&](core::PlaceId storage) {
      return std::ranges::any_of(checkedObjects, [&](const auto &entry) {
        return entry.second == storage;
      });
    };
    if ((freshObject(left.storage) && right.input) ||
        (freshObject(right.storage) && left.input))
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
    bool releasable = origin.kind == ValueOrigin::Kind::Null;
    if (memory) {
      const auto holder = memory->holder.value_or(memory->storage);
      const auto resource = state.resources.recordOf(holder);
      releasable |= resource && resource->family == "free" &&
                    !resource->escaped && !state.moves.recordOf(holder) &&
                    memory->begin == core::Affine::ofConstant(0);
    }
    const bool required = memory && !releasable &&
                          checkedRequire(core::CheckedRequirementKind::Release,
                                         *memory, call, state, "free");
    obligation(core::SafetyProperty::Release, releasable, required,
               "reallocation requires a live allocation base");
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
    const auto origin = builder.classifyValue(*call.getArg(0));
    bool released = origin.kind == ValueOrigin::Kind::Null && name == "free";
    bool required = false;
    if (origin.place) {
      const auto place = origin.place->place;
      const auto nullness = nullnessAt(place, state);
      released |=
          name == "free" && nullness && nullness->state == core::Nullness::Null;
      const auto resource = state.resources.recordOf(place);
      const auto spatial = state.spatial.recordOf(place);
      released |=
          resource && !resource->escaped && !state.moves.recordOf(place) &&
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
    bool source = true;
    if (name != "memset")
      source = interval(1, *bytes, true, false);
    interval(0, *bytes, false, source);
    if (name == "memcpy") {
      const auto left = checkedMemory(*call.getArg(0), {}, *bytes, state);
      const auto right = checkedMemory(*call.getArg(1), {}, *bytes, state);
      const bool separated = left && right && separate(*left, *right);
      const bool required =
          !separated && left && right && left->input && right->input;
      if (required && recording())
        inferred.checked.require(
            {.kind = core::CheckedRequirementKind::Separated,
             .path = *left->input,
             .other = *right->input,
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
    if (options.deferCheckedCalls && callee && !callee->hasBody() && !builtin) {
      checkedDeferredCalls.insert(&call);
      if (recording())
        inferred.checked.deferred = true;
      for (const auto *argument : call.arguments())
        if (argument->getType()->isPointerType())
          if (const auto memory = checkedMemory(*argument, {}, {}, state))
            state.safety->deferred.insert(memory->storage);
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
    core::NullTracker nulls;
    core::ScalarTracker scalars;
    core::PlaceGuard numericConditions;
    core::PlaceGuard pointerFacts;
    std::set<core::PlaceId> pointers;
    std::map<core::PlaceId, core::Affine> accessible;
    std::map<core::PlaceId, std::string> objectTypes;

    explicit CheckedRequirementState(const core::AnalysisState &state)
        : moves(state.moves), resources(state.resources), nulls(state.nulls),
          scalars(state.scalars), numericConditions(state.numericConditions),
          pointerFacts(state.pointerFacts), pointers(state.safety->pointers),
          accessible(state.safety->accessible),
          objectTypes(state.safety->objectTypes) {}

    void restore(core::AnalysisState &state) {
      state.moves = std::move(moves);
      state.resources = std::move(resources);
      state.nulls = std::move(nulls);
      state.scalars = std::move(scalars);
      state.numericConditions = std::move(numericConditions);
      state.pointerFacts = std::move(pointerFacts);
      state.safety->pointers = std::move(pointers);
      state.safety->accessible = std::move(accessible);
      state.safety->objectTypes = std::move(objectTypes);
    }
  };

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
        proved |= resource && !resource->escaped &&
                  !state.moves.recordOf(holder) &&
                  resource->family == requirement.family &&
                  (!spatial || spatial->offset.isZero());
      } else if (kind == core::CheckedRequirementKind::Separated) {
        property = core::SafetyProperty::Aliasing;
        {
          const auto other =
              checkedPathMemory(requirement.other, call, {}, {}, state);
          if (other && other->storage != memory->storage) {
            proved = separate(*memory, *other);
            if (!proved && memory->input && other->input) {
              if (recording()) {
                auto exported = requirement;
                exported.path = *memory->input;
                exported.other = *other->input;
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
           effects->summary->checked.obligations.entries(),
           [](const auto &entry) {
             return entry.second.outcome == core::SafetyOutcome::Trusted &&
                    entry.second.reason != "modeled C library contract" &&
                    entry.second.reason != "compiler object-size query";
           }))) {
    state.safety->memory.clear();
    for (auto &[storage, type] : state.safety->objectTypes) {
      (void)storage;
      type = "?";
    }
    state.safety->termination.clear();
    state.safety->havoc = true;
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
       })))
    state.forgetZeroedMemory();
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
  if (effects && effects->summary)
    applyCheckedPosts(call, *effects->summary, state);
}

} // namespace weavec::analysis
