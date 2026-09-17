//===- DataflowCallContext.cpp - Capture and install caller identities ----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "IntegerSupport.h"

using namespace clang;

namespace weavec::analysis {

static QualType contextStepType(QualType type, const core::PathElem &step) {
  if (type.isNull())
    return {};
  if (step.step == core::PathStep::Field) {
    const auto *record = type->getAsRecordDecl();
    if (!record)
      return {};
    for (const auto *field : record->fields())
      if (field->getName() == step.field)
        return field->getType();
    return {};
  }
  if (step.step == core::PathStep::Index && !step.field.empty())
    return type;
  if (const auto *array = type->getAsArrayTypeUnsafe())
    return array->getElementType();
  return type->isPointerType() ? type->getPointeeType() : QualType{};
}

std::optional<std::pair<core::PlaceId, QualType>>
FunctionDataflow::contextPlace(const core::SummaryPath &path,
                               const core::AnalysisState &state) {
  const VarDecl *root = nullptr;
  if (path.isParam() && path.index < function.getNumParams())
    root = function.getParamDecl(path.index);
  else if (path.isGlobal())
    root = summaries.globals().declFor(path.index);
  if (!root)
    return std::nullopt;
  core::PlaceId place = builder.placeForVar(*root);
  QualType type = root->getType();
  for (const auto &step : path.steps) {
    const auto parentType = type;
    type = contextStepType(type, step);
    if (type.isNull())
      return std::nullopt;
    switch (step.step) {
    case core::PathStep::Deref:
      place = places.deref(place);
      break;
    case core::PathStep::Field:
      for (const auto *field : parentType->getAsRecordDecl()->fields())
        if (field->getName() == step.field) {
          place = builder.fieldPlace(place, *field);
          break;
        }
      break;
    case core::PathStep::Index:
      if (step.field.empty()) {
        place = places.index(place);
      } else {
        auto selector = core::ArrayIndex::parse(step.field);
        if (!selector)
          return std::nullopt;
        if (selector->symbol) {
          if (*selector->symbol >= function.getNumParams())
            return std::nullopt;
          const auto symbol =
              builder.placeForVar(*function.getParamDecl(*selector->symbol));
          selector->symbol = symbol.value;
          const auto fact = state.scalars.factOf(symbol);
          if (fact && fact->constant) {
            const auto translated = core::ArrayIndex::constant(*fact->constant)
                                        .shifted(selector->offset);
            if (!translated)
              return std::nullopt;
            selector = translated;
          }
        }
        arrayTypes[place] = type;
        place = places.element(place, selector->toString());
      }
      break;
    }
  }
  return std::pair{place, type};
}

void FunctionDataflow::initializeCallContext(core::AnalysisState &state) {
  if (memoryContext.empty())
    return;
  bool valid = memoryContext.valid();
  // Integer entry facts precede selected paths that use those parameters.
  const auto installFact = [&](const core::SummaryPath &path,
                               const core::ValueFact &fact) {
    const auto place = contextPlace(path, state);
    if (!place || (fact.isPointer() != place->second->isPointerType())) {
      valid = false;
      return;
    }
    if (fact.isPointer()) {
      const bool null = fact.classes.contains(core::Outcome::Null);
      state.nulls.set(place->first, {.state = null ? core::Nullness::Null
                                                   : core::Nullness::NonNull,
                                     .location = locate(function.getLocation()),
                                     .reason = core::NullReason::Declared});
      if (null)
        state.resources.markNull(place->first);
    } else {
      state.scalars.set(place->first, fact);
      if (const auto entry = numericEntryValues.find(place->first);
          entry != numericEntryValues.end())
        state.scalars.set(entry->second, fact);
    }
  };
  for (const auto &[path, fact] : memoryContext.facts)
    if (path.isRoot() && !fact.isPointer())
      installFact(path, fact);
  for (const auto &[path, fact] : memoryContext.facts)
    if (!path.isRoot() || fact.isPointer())
      installFact(path, fact);
  for (const auto &path : memoryContext.nonNan) {
    const auto input = contextPlace(path, state);
    if (!state.safety || !input || !input->second->isRealFloatingType() ||
        input->second.isVolatileQualified() || input->second->isAtomicType()) {
      valid = false;
      continue;
    }
    state.safety->nonNan.insert(input->first);
  }
  for (const auto &[path, bytes] : memoryContext.bytes) {
    const auto input = contextPlace(path, state);
    if (!state.safety || !input || !input->second->isPointerType() ||
        !input->second->getPointeeType()->isCharType() ||
        input->second->getPointeeType().isVolatileQualified() ||
        context.getCharWidth() != 8) {
      valid = false;
      continue;
    }
    const auto storage = places.deref(input->first);
    const auto end =
        core::Affine::ofConstant(static_cast<std::int64_t>(bytes.size()));
    // RFC 0029: capturing the payload independently proved this live readable
    // interval. It supplies a minimum accessible extent, not write permission
    // or an exact physical allocation size.
    state.safety->pointers.insert(input->first);
    state.nulls.set(input->first, {.state = core::Nullness::NonNull,
                                   .location = {},
                                   .reason = core::NullReason::Declared});
    state.safety->accessible[storage] = end;
    state.safety->initialize(
        storage,
        {.begin = {},
         .end = end,
         .bytes = bytes,
         .immutableBytes = memoryContext.immutableBytes.contains(path)});
  }
  std::map<core::SummaryPath, core::PlaceId> inputs;
  for (const auto &alias : memoryContext.aliases) {
    const auto a = contextPlace(alias.first, state);
    const auto b = contextPlace(alias.second, state);
    if (!a || !b || !a->second->isPointerType() ||
        !b->second->isPointerType()) {
      valid = false;
      continue;
    }
    inputs[alias.first] = a->first;
    inputs[alias.second] = b->first;
    state.aliases.unite(
        a->first, b->first, alias.offset, core::ElementWitness::whole(),
        core::ElementWitness::whole(), alias.sameShare, !alias.definite);
    if (alias.definite) {
      state.definiteAliases.unite(
          a->first, b->first, alias.offset, core::ElementWitness::whole(),
          core::ElementWitness::whole(), alias.sameShare);
      if (alias.offset.isZero())
        state.pointerFacts.requirePointer(a->first, b->first, true);
    }
  }
  for (const auto &[first, second] : memoryContext.separations) {
    const auto a = contextPlace(first, state);
    const auto b = contextPlace(second, state);
    if (!a || !b || !a->second->isPointerType() ||
        !b->second->isPointerType()) {
      valid = false;
      continue;
    }
    state.distinctObjects.insert(std::minmax(a->first, b->first));
    state.pointerFacts.requirePointer(a->first, b->first, false);
  }
  for (const auto &[first, second] : memoryContext.orders) {
    const auto a = contextPlace(first, state);
    const auto b = contextPlace(second, state);
    const auto bytePointer = [](const auto &input) {
      return input && input->second->isPointerType() &&
             input->second->getPointeeType()->isCharType();
    };
    valid &= bytePointer(a) && bytePointer(b);
  }
  for (const auto &[path, place] : inputs) {
    for (const auto &[anchor, origin] : inputs) {
      (void)anchor;
      if (const auto offset = state.definiteAliases.offsetOf(origin, place)) {
        contextEntryOffsets[path] = *offset;
        break;
      }
    }
  }
  validMemoryContext = valid;
  if (!valid)
    inferred.incomplete.insert("unrepresentable call context input path");
}

std::optional<core::CallContext>
FunctionDataflow::captureCallContext(const CallExpr &call,
                                     const core::FunctionSummary &summary,
                                     core::AnalysisState &state) {
  const bool changesMemory =
      !summary.stores.empty() ||
      std::ranges::any_of(
          summary.numericOutputs,
          [](const auto &entry) { return !entry.first.isResult(); }) ||
      std::ranges::any_of(summary.effects, [](const auto &entry) {
        return entry.second.consumed() || entry.second.written;
      });
  // A constructor case can refine head fields (for example an empty child
  // slot for a singleton), while its general contract remains inductive.
  const bool constructorCase =
      options.checkContracts &&
      std::ranges::any_of(summary.checked.establishes, [](const auto &post) {
        if (post.kind != core::CheckedRequirementKind::ContainerFresh ||
            !post.path.isResult())
          return false;
        const auto shape = core::ContainerShape::decode(post.family);
        return shape && (!shape->children.empty() || !shape->ownership.empty());
      });
  const bool projectionCase =
      options.checkContracts &&
      std::ranges::any_of(summary.checked.establishes, [](const auto &post) {
        return (post.kind == core::CheckedRequirementKind::ContainerTail ||
                post.kind == core::CheckedRequirementKind::ContainerDerived) &&
               post.path.isResult();
      });
  bool checkedCase =
      options.checkContracts && summary.checked.computed &&
      (!summary.checked.complete() || constructorCase || projectionCase);
  const auto *inductiveCallee = call.getDirectCallee();
  const bool completeInduction = inductiveCallee != nullptr &&
                                 summary.checked.complete() &&
                                 summaries.verifiedRecursiveContracts.contains(
                                     inductiveCallee->getCanonicalDecl());
  // Complete induction already covers the input bytes. Keep its established
  // contract instead of nominating extra pointee selectors merely from a
  // payload. Existing constructor, alias and scalar cases remain available.
  const bool byteCandidate =
      !completeInduction && options.checkContracts &&
      summary.checked.computed && state.safety &&
      std::ranges::any_of(state.safety->memory, [](const auto &entry) {
        return std::ranges::any_of(entry.second, [](const auto &range) {
          return !range.bytes.empty();
        });
      });
  if (!changesMemory && !checkedCase && !byteCandidate)
    return std::nullopt;
  const auto owner = callSummaries.find(&call);
  assert(owner != callSummaries.end() && owner->second.get() == &summary &&
         "capture requires the retained immutable call summary");
  const auto &prepared = callFootprints.get(owner->second);
  if (!prepared) {
    reportIncomplete("call context input path limit reached", call);
    return std::nullopt;
  }
  auto footprint = *prepared;
  if (checkedCase || byteCandidate)
    footprint.insert(summary.checked.caseInputs.begin(),
                     summary.checked.caseInputs.end());
  if (options.checkContracts && summary.checked.computed)
    for (const auto &[path, effect] : summary.effects)
      if (effect.written && !path.isResult())
        footprint.insert(path);
  if (state.safety && checkedCase)
    for (const auto &pre : summary.checked.requirements) {
      if (pre.kind != core::CheckedRequirementKind::Container ||
          !pre.path.isParam() || !pre.path.isRoot())
        continue;
      const auto actual = builder.resolveSummaryPath(pre.path, call);
      const auto shape = core::ContainerShape::decode(pre.family);
      if (!actual || !shape || state.safety->containers.find(actual->place))
        continue;
      if (const auto memory = checkedMemoryAt(actual->place, {}, {}, state))
        if (const auto fact = establishContainer(*memory, *shape, state))
          state.safety->containers.set(actual->place, *fact);
    }
  if (state.safety)
    for (unsigned i = 0; i < call.getNumArgs(); ++i)
      if (const auto ref =
              builder.resolveSummaryPath(core::SummaryPath::param(i), call)) {
        if (checkedCase && !state.safety->containers.find(ref->place))
          if (const auto *shape = containerShape(call.getArg(i)->getType()))
            if (const auto memory = checkedMemoryAt(ref->place, {}, {}, state))
              if (const auto fact = establishContainer(*memory, *shape, state))
                state.safety->containers.set(ref->place, *fact);
        if (const auto *fact = state.safety->containers.find(ref->place)) {
          // A terminal opaque object can specialize a helper that releases
          // only its head. These are actual null fields, not new ownership.
          const auto *callee = call.getDirectCallee();
          const auto parameter = callee && i < callee->getNumParams()
                                     ? callee->getParamDecl(i)->getType()
                                     : QualType{};
          const bool recordParameter =
              !parameter.isNull() && parameter->isPointerType() &&
              parameter->getPointeeType()->isRecordType();
          if ((fact->shape.terminal || !fact->shape.emptyLinks.empty()) &&
              (recordParameter || summary.objectViews.contains(
                                      core::SummaryPath::param(i).deref()))) {
            const auto nominate = [&](const std::string &name) {
              if (fact->shape.terminal || fact->shape.emptyLinks.contains(name))
                footprint.insert(
                    core::SummaryPath::param(i).deref().field(name));
            };
            nominate(fact->shape.link.name);
            for (const auto &child : fact->shape.children)
              nominate(child.name);
          }
          for (const auto &[name, condition] : fact->shape.ownership) {
            (void)name;
            footprint.insert(core::SummaryPath::param(i).deref().field(
                condition.field.name));
          }
          for (const auto &name : fact->shape.emptyPayloads)
            footprint.insert(core::SummaryPath::param(i).deref().field(name));
        }
      }
  if (footprint.size() > core::MaxCallContextFacts) {
    reportIncomplete("call context input path limit reached", call);
    return std::nullopt;
  }
  struct Input {
    core::SummaryPath path;
    core::PlaceId place;
    core::PointerOffset offset;
    bool storage = false;
    bool bytePointer = false;
  };
  std::vector<Input> inputs;
  bool unresolved = false;
  bool unrepresentable = false;
  core::CallContext result;
  result.reportDiagnostics = !inUnsafe && memoryContext.reportDiagnostics;
  for (const auto &path : footprint) {
    QualType type;
    const Expr *arg = nullptr;
    if (path.isParam() && path.index < call.getNumArgs()) {
      arg = call.getArg(path.index);
      type = arg->IgnoreParenCasts()->getType();
      // Stripping the null-to-pointer conversion exposes integer literal 0.
      // Keep its converted pointer type when capturing a null selector.
      if (arg->getType()->isPointerType() && !type->isPointerType() &&
          !type->isArrayType())
        type = arg->getType();
      if (const auto *array = type->getAsArrayTypeUnsafe())
        type = context.getPointerType(array->getElementType());
    } else if (path.isGlobal()) {
      if (const auto *decl = summaries.globals().declFor(path.index))
        type = decl->getType();
    }
    for (const auto &step : path.steps)
      type = contextStepType(type, step);
    if (type.isNull()) {
      unrepresentable = true;
      continue;
    }
    if (!type->isPointerType() || type->isFunctionPointerType())
      continue;
    if (arg && path.isRoot()) {
      const auto origin = builder.classifyValue(*arg);
      if (origin.kind == ValueOrigin::Kind::Null) {
        result.facts[path] = core::ValueFact::of(core::Outcome::Null);
        continue;
      }
      if (origin.kind == ValueOrigin::Kind::Borrow && origin.place) {
        inputs.push_back({.path = path,
                          .place = origin.place->place,
                          .offset = origin.offset,
                          .storage = true,
                          .bytePointer = type->getPointeeType()->isCharType()});
        result.facts[path] = core::ValueFact::of(core::Outcome::NonNull);
        continue;
      }
      if (const auto copied = PlaceBuilder::copyOrNull(origin)) {
        inputs.push_back({.path = path,
                          .place = copied->place,
                          .offset = origin.offset,
                          .storage = false,
                          .bytePointer = type->getPointeeType()->isCharType()});
        if (origin.offset.isZero())
          if (const auto fact = state.factOf(copied->place);
              fact && !fact->trivial())
            result.facts[path] = *fact;
        continue;
      }
    }
    if (const auto ref = builder.resolveSummaryPath(path, call)) {
      inputs.push_back({.path = path,
                        .place = ref->place,
                        .offset = {},
                        .storage = false,
                        .bytePointer = type->getPointeeType()->isCharType()});
      if (const auto fact = state.factOf(ref->place); fact && !fact->trivial())
        result.facts[path] = *fact;
    } else {
      unrepresentable = true;
    }
  }
  const auto storageOfInput =
      [&state](const Input &input) -> std::optional<core::PlaceId> {
    if (input.storage)
      return input.place;
    if (state.safety)
      if (const auto position = state.safety->positions.find(input.place);
          position != state.safety->positions.end())
        return position->second.storage;
    const auto loans = state.loans.heldBy(input.place);
    if (loans.size() == 1)
      return loans.front().place;
    return std::nullopt;
  };
  if (inputs.size() > core::MaxCallContextPaths) {
    reportIncomplete("call context input path limit reached", call);
    return std::nullopt;
  }
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    for (std::size_t j = i + 1; j < inputs.size(); ++j) {
      const auto &a = inputs[i];
      const auto &b = inputs[j];
      std::optional<core::PointerOffset> offset;
      bool definite = true;
      bool sameShare = true;
      const auto sa = storageOfInput(a);
      const auto sb = storageOfInput(b);
      const auto position =
          [&](const Input &input) -> std::optional<CheckedMemory> {
        if (!state.safety || input.storage || !input.bytePointer ||
            !input.offset.isZero())
          return std::nullopt;
        const auto memory = checkedMemoryAt(input.place, {}, {}, state);
        if (!memory || !memory->extent ||
            !checkedInterval(memory->begin, memory->end, *memory->extent,
                             state))
          return std::nullopt;
        const bool valid = checkedValid(*memory, state) ||
                           checkedRequire(core::CheckedRequirementKind::Valid,
                                          *memory, call, state);
        return valid ? memory : std::nullopt;
      };
      const auto pa = position(a);
      const auto pb = position(b);
      if (pa && pb && pa->storage == pb->storage) {
        // Same array with a proved order, without inventing a displacement.
        const bool ab = checkedAtMost(pa->begin, pb->begin, state);
        const bool ba = checkedAtMost(pb->begin, pa->begin, state);
        if (ab || ba) {
          if (!result.addAlias(
                  {.first = a.path,
                   .second = b.path,
                   .offset = core::PointerOffset::unknown(),
                   .definite = true,
                   .sameShare = state.aliases.sameShare(a.place, b.place)})) {
            reportIncomplete("call context relationship limit reached", call);
            return std::nullopt;
          }
          if (ab)
            result.orders.emplace(a.path, b.path);
          if (ba)
            result.orders.emplace(b.path, a.path);
          continue;
        }
      }
      if (sa || sb) {
        if (sa && sb &&
            (*sa == *sb || std::ranges::any_of(definiteMirrors(*sa, state),
                                               [&](core::PlaceId place) {
                                                 return place == *sb;
                                               }))) {
          offset = core::PointerOffset::zero();
          // A copied borrow keeps its offset in the spatial record; the
          // loan names storage, not the address within it (RFC 0016).
          if (!a.storage)
            if (const auto spatial = state.spatial.recordOf(a.place))
              *offset = offset->plus(spatial->offset);
          if (!b.storage)
            if (const auto spatial = state.spatial.recordOf(b.place))
              *offset = offset->plus(spatial->offset.negated());
          // Checked cursors can move through helper outputs while the
          // ordinary spatial record still names their entry displacement.
          // Without the checked order above, shared storage supplies no
          // particular address difference or ownership-share identity.
          if (state.safety &&
              ((!a.storage && state.safety->positions.contains(a.place)) ||
               (!b.storage && state.safety->positions.contains(b.place)))) {
            offset = core::PointerOffset::unknown();
            sameShare = state.aliases.sameShare(a.place, b.place);
          }
        }
      } else {
        offset = state.definiteAliases.offsetOf(b.place, a.place);
        if (!offset) {
          offset = state.aliases.offsetOf(b.place, a.place);
          definite = false;
        }
        sameShare = state.aliases.sameShare(a.place, b.place);
      }
      if (!offset) {
        bool distinct = false;
        if (sa && sb) {
          // Separate declared objects cannot overlap; separate cells below
          // arbitrary pointers need stronger object identity evidence.
          const auto ra = places.root(*sa);
          const auto rb = places.root(*sb);
          distinct = ra != rb && !places.innermostDeref(*sa) &&
                     !places.innermostDeref(*sb);
          if (state.safety) {
            const auto inputObject = [&](core::PlaceId storage) {
              return std::ranges::any_of(
                  checkedInputObjects,
                  [&](const auto &entry) { return entry.second == storage; });
            };
            distinct |= (isLocalStorage(*sa) && inputObject(*sb)) ||
                        (isLocalStorage(*sb) && inputObject(*sa));
          }
        }
        const auto ar = state.resources.recordOf(a.place);
        const auto br = state.resources.recordOf(b.place);
        const auto allocated = [](const auto &record) {
          return record && record->origin == core::ResourceOrigin::Allocated &&
                 record->location.isValid();
        };
        if (allocated(ar) && allocated(br) && ar->location != br->location)
          distinct = true;
        if ((sa && !places.innermostDeref(*sa) && allocated(br)) ||
            (sb && !places.innermostDeref(*sb) && allocated(ar)))
          distinct = true;
        for (const auto &[first, second] : state.distinctObjects)
          if ((state.definiteAliases.mayAlias(first, a.place) &&
               state.definiteAliases.mayAlias(second, b.place)) ||
              (state.definiteAliases.mayAlias(second, a.place) &&
               state.definiteAliases.mayAlias(first, b.place)))
            distinct = true;
        if (distinct)
          result.separations.insert(std::minmax(a.path, b.path));
        else
          unresolved = true;
        continue;
      }
      *offset = offset->plus(a.offset).plus(b.offset.negated());
      if (!result.addAlias({.first = a.path,
                            .second = b.path,
                            .offset = *offset,
                            .definite = definite,
                            .sameShare = sameShare})) {
        reportIncomplete("call context relationship limit reached", call);
        return std::nullopt;
      }
    }
  }
  // Unrelated names alone request no alias specialization (RFC 0016).
  // A fully resolved separation context can still prune selected indices.
  const bool selectedInputs =
      std::ranges::any_of(footprint, [](const auto &path) {
        return std::ranges::any_of(path.steps, [](const auto &step) {
          return step.step == core::PathStep::Index && !step.field.empty();
        });
      });
  // Ordinary calls without a usable memory relationship need no scalar
  // capture. RFC 0019's scalar-only specialization is checked-mode work.
  if (!options.checkContracts) {
    if (result.aliases.empty() &&
        (!selectedInputs || inputs.size() < 2 || unresolved || unrepresentable))
      return std::nullopt;
    if (unresolved || unrepresentable) {
      reportIncomplete(unrepresentable
                           ? "unrepresentable call context input path"
                           : "unresolved call alias relationship",
                       call);
      return std::nullopt;
    }
  }
  // Constants and sign/null classes bound branch specialization. The value
  // domain and conversion assumptions are the same as ordinary CFG checking.
  for (unsigned i = 0; i < call.getNumArgs(); ++i) {
    const auto *arg = call.getArg(i);
    if (options.checkContracts && arg->getType()->isRealFloatingType() &&
        checkedFloatingValue(*arg, state))
      result.nonNan.insert(core::SummaryPath::param(i));
    if (!arg->getType()->isIntegerType())
      continue;
    if (const auto fact = scalarFactOf(*arg, state);
        fact &&
        (fact->constant || (fact->integer && fact->integer->constant())))
      result.facts[core::SummaryPath::param(i)] = *fact;
    const auto affine = foldAffine(builder.affineOf(*arg), state);
    if (!affine)
      continue;
    if (affine->isConstant())
      result.facts[core::SummaryPath::param(i)] =
          core::ValueFact::ofConstant(affine->constant);
    else if (affine->scale == 1 && affine->constant == 0)
      if (const auto fact = state.factOf(*affine->place);
          fact && !fact->trivial())
        result.facts[core::SummaryPath::param(i)] = *fact;
  }
  if (byteCandidate && context.getCharWidth() == 8)
    for (const auto &input : inputs) {
      if (!input.bytePointer)
        continue;
      const auto memory = checkedPathMemory(input.path, call, {}, {}, state);
      if (!memory || !memory->extent || !checkedValid(*memory, state))
        continue;
      const auto start = foldAffine(memory->begin, state);
      const auto found = state.safety->memory.find(memory->storage);
      if (!start.isConstant() || found == state.safety->memory.end())
        continue;
      for (const auto &range : found->second) {
        if (range.bytes.empty() || range.source || !range.begin.isConstant() ||
            !range.end.isConstant() || range.begin.constant > start.constant ||
            start.constant >= range.end.constant)
          continue;
        auto when = range.when;
        auto slice = *memory;
        slice.end = range.end;
        if (!pruneGuard(when, state) || !when.trivial() ||
            !checkedInterval(slice.begin, slice.end, *slice.extent, state) ||
            !checkedInitialized(slice, state))
          continue;
        result.bytes[input.path] = range.bytes.substr(
            static_cast<std::size_t>(start.constant - range.begin.constant));
        if (range.immutableBytes)
          result.immutableBytes.insert(input.path);
        break;
      }
    }
  checkedCase |= !result.bytes.empty();
  if (!changesMemory && !checkedCase)
    return std::nullopt;
  for (const auto &path : footprint) {
    if (const auto ref = builder.resolveSummaryPath(path, call)) {
      if (const auto fact = state.factOf(ref->place); fact && !fact->trivial())
        result.facts[path] = *fact;
      if (checkedZeroInteger(ref->place, state))
        result.facts[path] = core::ValueFact::ofConstant(0);
    }
    // A copied pointer field may identify an initialized local scalar without
    // a separately materialized scalar fact under the field's dereference.
    // Capture only an exact whole scalar of the same target C type. Merely
    // sharing an allocation or a byte representation does not give its value.
    if (!options.checkContracts || result.facts.contains(path) ||
        path.steps.empty() || path.steps.back().step != core::PathStep::Deref ||
        !path.isParam() || path.index >= call.getNumArgs())
      continue;
    auto type = call.getArg(path.index)->getType();
    for (const auto &step : path.steps)
      type = contextStepType(type, step);
    if (type.isNull() || !type->isIntegerType() || type.isVolatileQualified() ||
        type->isAtomicType())
      continue;
    auto pointer = path;
    pointer.steps.truncate(pointer.steps.size() - 1);
    const auto bytes = core::Affine::ofConstant(
        context.getTypeSizeInChars(type).getQuantity());
    const auto memory = checkedPathMemory(pointer, call, {}, bytes, state);
    if (!memory || memory->begin != core::Affine::ofConstant(0) ||
        memory->end != bytes || !memory->extent ||
        !checkedInterval(memory->begin, memory->end, *memory->extent, state) ||
        !checkedValid(*memory, state) || !checkedInitialized(*memory, state))
      continue;
    const auto *decl =
        dyn_cast_or_null<ValueDecl>(builder.declFor(memory->storage));
    if (!decl || decl->getType().isVolatileQualified() ||
        decl->getType()->isAtomicType() ||
        !ASTContext::hasSameUnqualifiedType(type, decl->getType()))
      continue;
    if (const auto fact = state.scalars.factOf(memory->storage);
        fact && !fact->trivial())
      result.facts[path] = *fact;
  }
  // RFC 0029: capture and guard checking use the same current head evidence.
  for (const auto &path : footprint)
    if (const auto ref = builder.resolveSummaryPath(path, call))
      if (const auto fact = containerValueFact(ref->place, state))
        result.facts[path] = *fact;
  const bool completeConsumption =
      summary.checked.complete() &&
      std::ranges::any_of(summary.checked.establishes, [](const auto &post) {
        return post.kind == core::CheckedRequirementKind::ContainerConsumed;
      });
  const bool recursiveContext =
      state.safety && !state.safety->containers.all().empty();
  // A complete inductive destructor needs no bounded scalar recheck. Alias
  // contexts still take the existing path; its generic premises are checked
  // against the actual caller before applying the consumption guarantee.
  const bool checkedScalars =
      !completeConsumption && options.checkContracts &&
      summary.checked.computed &&
      (!result.bytes.empty() || !result.nonNan.empty() ||
       std::ranges::any_of(result.facts, [&](const auto &entry) {
         return checkedCase || (recursiveContext && !entry.first.isRoot())
                    ? !entry.second.trivial()
                    : !entry.second.isPointer() &&
                          entry.second.constant.has_value();
       }));
  if (result.aliases.empty() && !checkedScalars &&
      (!selectedInputs || inputs.size() < 2 || unresolved || unrepresentable))
    return std::nullopt;
  if ((unresolved || unrepresentable) && !checkedScalars) {
    reportIncomplete(unrepresentable ? "unrepresentable call context input path"
                                     : "unresolved call alias relationship",
                     call);
    return std::nullopt;
  }
  if (result.empty())
    return std::nullopt;
  if (!result.valid()) {
    reportIncomplete("call context relationship limit reached", call);
    return std::nullopt;
  }
  return result;
}

core::PointerOffset
FunctionDataflow::contextOffsetOf(core::PlaceId place,
                                  const core::AnalysisState &state) {
  auto path = builder.summaryPathOf(place);
  if (const auto input = state.incoming.find(place);
      input != state.incoming.end() && input->second.path)
    path = input->second.path;
  if (path)
    if (const auto it = contextEntryOffsets.find(*path);
        it != contextEntryOffsets.end())
      return it->second;
  return core::PointerOffset::zero();
}

} // namespace weavec::analysis
