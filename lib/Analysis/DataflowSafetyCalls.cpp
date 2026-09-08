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

using namespace clang;

namespace weavec::analysis {

// RFC 0018: derive a flat source key rather than recursively escaping the
// previous caller's identity. Repeated source obligations still join by their
// weakest outcome, while the separate call chain retains bounded provenance.
static std::string checkedCallSubject(const std::string &callee,
                                      const core::SafetyObligation &entry) {
  const core::SafetyObligation origin{
      .property = core::SafetyProperty::Call,
      .location = entry.calls.empty() ? entry.location : entry.calls.back(),
      .function = callee,
      .subject = entry.reason,
      .reason = {},
      .calls = {}};
  return origin.identity();
}

void FunctionDataflow::checkedCall(const CallExpr &call,
                                   const CallEffects *effects,
                                   core::AnalysisState &state) {
  checkedWrites.erase(&call);
  const auto *callee = call.getDirectCallee();
  std::string name = callee ? callee->getNameAsString() : "indirect call";
  if (callee && callee->getBuiltinID() != 0) {
    if (name == "__builtin___memcpy_chk" || name == "__builtin_memcpy")
      name = "memcpy";
    if (name == "__builtin___memmove_chk" || name == "__builtin_memmove")
      name = "memmove";
    if (name == "__builtin___memset_chk" || name == "__builtin_memset")
      name = "memset";
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
  const auto valid = [&](const Expr &pointer, const CheckedMemory &memory) {
    const auto origin = builder.classifyValue(pointer);
    bool proved = origin.kind == ValueOrigin::Kind::Borrow;
    if (origin.kind == ValueOrigin::Kind::Copy && origin.place) {
      const auto place = origin.place->place;
      const auto nullness = nullnessAt(place, state);
      proved = state.safety->pointers.contains(place) &&
               !state.moves.recordOf(place) &&
               !state.resources.isEscaped(place) && nullness &&
               nullness->state == core::Nullness::NonNull;
    }
    const bool required =
        (memory.input || state.safety->deferred.contains(memory.storage)) &&
        checkedRequire(core::CheckedRequirementKind::Valid, memory, call,
                       state);
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
    bool complete = valid(*call.getArg(arg), *memory);
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
  if (builtin && (name == "malloc" || name == "calloc" || name == "abort" ||
                  name == "exit" || name == "_Exit")) {
    safetyObligation(core::SafetyProperty::Call, core::SafetyOutcome::Trusted,
                     call, name, "modeled C library contract");
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
      bool separated = false;
      if (left && right && left->storage != right->storage) {
        const auto a = state.resources.recordOf(left->storage);
        const auto b = state.resources.recordOf(right->storage);
        separated =
            ((builder.varForPlace(left->storage) != nullptr) &&
             (builder.varForPlace(right->storage) != nullptr) &&
             !builder.varForPlace(left->storage)->getType()->isPointerType() &&
             !builder.varForPlace(right->storage)
                  ->getType()
                  ->isPointerType()) ||
            (liveAllocation(a) && liveAllocation(b) &&
             a->location != b->location);
      }
      obligation(core::SafetyProperty::Aliasing, separated, false,
                 "memcpy objects must be disjoint");
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
  if (!contract->complete() && !pendingExternal) {
    bool explained = false;
    for (const auto &[key, entry] : contract->obligations.entries()) {
      (void)key;
      if (entry.outcome < core::SafetyOutcome::Unresolved)
        continue;
      auto calls = entry.calls;
      calls.insert(calls.begin(), entry.location);
      safetyObligation(
          core::SafetyProperty::Call, core::SafetyOutcome::Unresolved, call,
          checkedCallSubject(name, entry), entry.reason, std::move(calls));
      explained = true;
    }
    if (!explained)
      obligation(core::SafetyProperty::Call, false, false,
                 "callee checked contract is incomplete");
  } else {
    obligation(core::SafetyProperty::Call, true, false,
               "callee has a complete checked contract");
  }
  for (const auto &[key, entry] : contract->obligations.entries()) {
    (void)key;
    if (entry.outcome == core::SafetyOutcome::Trusted) {
      auto calls = entry.calls;
      calls.insert(calls.begin(), entry.location);
      safetyObligation(core::SafetyProperty::Call, core::SafetyOutcome::Trusted,
                       call, checkedCallSubject(name, entry), entry.reason,
                       std::move(calls));
    }
  }
  for (const auto &requirement : contract->requirements) {
    if (!requirement.path.isParam() || !requirement.path.isRoot() ||
        requirement.path.index >= call.getNumArgs()) {
      obligation(core::SafetyProperty::Call, false, false,
                 "checked requirement path cannot be instantiated");
      continue;
    }
    const auto &pointer = *call.getArg(requirement.path.index);
    if (requirement.kind == core::CheckedRequirementKind::Release &&
        requirement.family == "free" &&
        builder.classifyValue(pointer).kind == ValueOrigin::Kind::Null) {
      obligation(core::SafetyProperty::Release, true, false,
                 "free permits a null pointer");
      continue;
    }
    const auto first = builder.affineFromPath(requirement.begin, call);
    const auto last = builder.affineFromPath(requirement.end, call);
    const auto memory = first && last
                            ? checkedMemory(pointer, *first, *last, state)
                            : std::nullopt;
    if (!memory) {
      obligation(core::SafetyProperty::Call, false, false,
                 "checked requirement interval cannot be instantiated");
      continue;
    }
    const auto kind = requirement.kind;
    if (kind == core::CheckedRequirementKind::Writable) {
      checkedWrite(*memory, call, state);
      continue;
    }
    if (kind == core::CheckedRequirementKind::Valid) {
      valid(pointer, *memory);
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
    } else if (kind == core::CheckedRequirementKind::Release) {
      property = core::SafetyProperty::Release;
      const auto origin = builder.classifyValue(pointer);
      proved = origin.kind == ValueOrigin::Kind::Null &&
               requirement.family == "free";
      const auto resource = state.resources.recordOf(memory->storage);
      const auto spatial = state.spatial.recordOf(memory->storage);
      proved |= resource && !resource->escaped &&
                !state.moves.recordOf(memory->storage) &&
                resource->family == requirement.family &&
                origin.offset.isZero() &&
                (!spatial || spatial->offset.isZero());
    } else if (kind == core::CheckedRequirementKind::Separated) {
      property = core::SafetyProperty::Aliasing;
      if (requirement.other.isParam() && requirement.other.isRoot() &&
          requirement.other.index < call.getNumArgs()) {
        const auto other =
            checkedMemory(*call.getArg(requirement.other.index), {}, {}, state);
        if (other && other->storage != memory->storage) {
          const auto a = state.resources.recordOf(memory->storage);
          const auto b = state.resources.recordOf(other->storage);
          proved = ((builder.varForPlace(memory->storage) != nullptr) &&
                    (builder.varForPlace(other->storage) != nullptr) &&
                    !builder.varForPlace(memory->storage)
                         ->getType()
                         ->isPointerType() &&
                    !builder.varForPlace(other->storage)
                         ->getType()
                         ->isPointerType()) ||
                   (liveAllocation(a) && liveAllocation(b) &&
                    a->location != b->location);
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
        (!proved ||
         (memory->input && (kind == core::CheckedRequirementKind::Extent ||
                            kind == core::CheckedRequirementKind::Release))) &&
        checkedRequire(kind, *memory, call, state, requirement.family);
    obligation(property, proved, required,
               "callee safety precondition must hold");
  }
  if (contract->complete())
    for (const auto &post : contract->establishes) {
      if (post.kind != core::CheckedRequirementKind::Initialized ||
          !post.path.isParam() || !post.path.isRoot() ||
          post.path.index >= call.getNumArgs())
        continue;
      const auto first = builder.affineFromPath(post.begin, call);
      const auto last = builder.affineFromPath(post.end, call);
      if (first && last)
        if (const auto memory = checkedMemory(*call.getArg(post.path.index),
                                              *first, *last, state))
          checkedWrites[&call].push_back(*memory);
    }
}

void FunctionDataflow::checkedCallAfter(const CallExpr &call,
                                        const CallEffects *effects,
                                        core::AnalysisState &state) {
  if (const auto *callee = call.getDirectCallee();
      callee && callee->getBuiltinID() != 0 &&
      (callee->getName() == "__builtin_object_size" ||
       callee->getName() == "__builtin_dynamic_object_size"))
    return;
  // Unknown/unsafe calls may replace pointer representations and mutate every
  // reachable byte. Do not allow a trusted call to manufacture later evidence.
  if (!effects || !effects->summary ||
      (!effects->summary->checked.computed &&
       effects->source != SummarySource::Builtin) ||
      (effects->summary && effects->summary->checked.obligations.trusted() &&
       effects->source != SummarySource::Builtin)) {
    state.safety->memory.clear();
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
  if (writes != checkedWrites.end()) {
    for (const auto &memory : writes->second)
      state.safety->initialize(memory.storage,
                               {.begin = memory.begin, .end = memory.end});
    checkedWrites.erase(writes);
  }
}

} // namespace weavec::analysis
