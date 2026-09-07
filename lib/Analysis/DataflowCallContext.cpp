//===- DataflowCallContext.cpp - Capture and install caller identities ----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

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
                               core::AnalysisState &state) {
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
    type = contextStepType(type, step);
    if (type.isNull())
      return std::nullopt;
    switch (step.step) {
    case core::PathStep::Deref:
      place = places.deref(place);
      break;
    case core::PathStep::Field:
      place = places.field(place, step.field);
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
    }
  };
  for (const auto &[path, fact] : memoryContext.facts)
    if (path.isRoot() && !fact.isPointer())
      installFact(path, fact);
  for (const auto &[path, fact] : memoryContext.facts)
    if (!path.isRoot() || fact.isPointer())
      installFact(path, fact);
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
        return entry.second.consumed();
      });
  if (!changesMemory)
    return std::nullopt;
  const auto footprint = core::callMemoryFootprint(summary);
  if (footprint.size() > core::MaxCallContextFacts) {
    reportIncomplete("call context input path limit reached", call);
    return std::nullopt;
  }
  struct Input {
    core::SummaryPath path;
    core::PlaceId place;
    core::PointerOffset offset;
    bool storage = false;
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
                          .storage = true});
        result.facts[path] = core::ValueFact::of(core::Outcome::NonNull);
        continue;
      }
      if (const auto copied = PlaceBuilder::copyOrNull(origin)) {
        inputs.push_back(
            {.path = path, .place = copied->place, .offset = origin.offset});
        if (origin.offset.isZero())
          if (const auto fact = state.factOf(copied->place);
              fact && !fact->trivial())
            result.facts[path] = *fact;
        continue;
      }
    }
    if (const auto ref = builder.resolveSummaryPath(path, call)) {
      inputs.push_back({.path = path, .place = ref->place, .offset = {}});
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
  if (result.aliases.empty() &&
      (!selectedInputs || inputs.size() < 2 || unresolved || unrepresentable))
    return std::nullopt;
  if (unresolved || unrepresentable) {
    reportIncomplete(unrepresentable ? "unrepresentable call context input path"
                                     : "unresolved call alias relationship",
                     call);
    return std::nullopt;
  }
  // Constants and sign/null classes bound branch specialization. The value
  // domain and conversion assumptions are the same as ordinary CFG checking.
  for (unsigned i = 0; i < call.getNumArgs(); ++i) {
    const auto *arg = call.getArg(i);
    if (!arg->getType()->isIntegerType())
      continue;
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
  for (const auto &path : footprint) {
    if (const auto ref = builder.resolveSummaryPath(path, call))
      if (const auto fact = state.factOf(ref->place); fact && !fact->trivial())
        result.facts[path] = *fact;
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
