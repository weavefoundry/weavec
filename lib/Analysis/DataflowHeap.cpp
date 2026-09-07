//===- DataflowHeap.cpp - Interprocedural heap postconditions -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

#include "llvm/Support/Casting.h"

#include <algorithm>
#include <deque>

namespace weavec::analysis {

/// Heap postconditions name incoming values explicitly. A test of a cell
/// this function has already overwritten is a post-state fact, not a
/// precondition on its incoming value (RFC 0013). Containing-pointer guards
/// are reconstructed when the graph is materialized.
static void keepInputGuard(core::ValueSource &value,
                           const core::AnalysisState &state) {
  const auto replaced = [&](const core::SummaryPath &path) {
    return state.isOverwritten(path) ||
           std::ranges::any_of(
               state.stored, [&path](const core::SummaryPath &stored) {
                 return stored == path || stored.isProperPrefixOf(path);
               });
  };
  for (auto it = value.when.pointers.begin();
       it != value.when.pointers.end();) {
    if (replaced(it->first.first) || replaced(it->first.second))
      it = value.when.pointers.erase(it);
    else
      ++it;
  }
  for (auto it = value.when.conditions.begin();
       it != value.when.conditions.end();) {
    const auto &path = it->first;
    if (state.isOverwritten(path) ||
        std::ranges::any_of(
            state.stored, [&path](const core::SummaryPath &stored) {
              return stored == path || stored.isProperPrefixOf(path);
            }))
      it = value.when.conditions.erase(it);
    else
      ++it;
  }
}

core::PathGuard
FunctionDataflow::heapEntryGuard(const core::PlaceGuard &guard,
                                 const core::AnalysisState &state) {
  core::PathGuard result;
  const auto inputPath =
      [&](core::PlaceId place) -> std::optional<core::SummaryPath> {
    if (const auto it = snapshotInputPaths.find(place);
        it != snapshotInputPaths.end())
      return it->second;
    if (const auto it = state.incoming.find(place);
        it != state.incoming.end() &&
        it->second.kind == core::ValueSource::Kind::Copy &&
        it->second.offset.isZero())
      return it->second.path;
    const auto path = stableSummaryPathOf(place);
    return path && !state.isOverwritten(*path) ? path : std::nullopt;
  };
  for (const auto &[pair, equal] : guard.pointers) {
    const auto a = inputPath(pair.first);
    const auto b = inputPath(pair.second);
    if (a && b)
      result.requirePointer(*a, *b, equal);
  }
  for (const auto &[place, fact] : guard.conditions) {
    if (const auto input = snapshotInputPaths.find(place);
        input != snapshotInputPaths.end()) {
      result.require(input->second, fact);
      continue;
    }
    if (const auto entry = state.incoming.find(place);
        entry != state.incoming.end() &&
        entry->second.kind == core::ValueSource::Kind::Copy &&
        entry->second.path && entry->second.offset.isZero()) {
      result.require(*entry->second.path, fact);
      continue;
    }
    const auto path = stableSummaryPathOf(place);
    if (!path || writtenScalarPaths.contains(*path))
      continue;
    if (std::ranges::any_of(
            state.stored, [&](const core::SummaryPath &written) {
              return written == *path || written.isProperPrefixOf(*path);
            }))
      continue;
    result.require(*path, fact);
  }
  return result;
}

core::PathGuard
FunctionDataflow::heapWriteGuard(core::PlaceId place,
                                 const core::AnalysisState &state) {
  core::ValueSource value;
  // handleAssign marked its target overwritten but has not written it yet.
  value.when = heapEntryGuard(guardHere(state), state);
  for (auto current = std::optional(place); current;
       current = places.parent(*current)) {
    if (!state.definiteHeapWrites.contains(*current))
      continue;
    const auto it = state.heapWriteGuards.find(*current);
    if (it == state.heapWriteGuards.end())
      continue;
    // A predecessor that did not publish the object drops this must-fact.
    value.when.conjoin(it->second);
  }
  return value.when;
}

std::vector<core::PlaceId>
FunctionDataflow::definiteMirrors(core::PlaceId place,
                                  const core::AnalysisState &state) {
  const auto parent = places.parent(place);
  if (!parent)
    return {place};
  std::set<core::PlaceId> result;
  for (const core::PlaceId base : definiteMirrors(*parent, state)) {
    switch (places.step(place)) {
    case core::PathStep::Field:
      result.insert(places.field(base, places.fieldName(place)));
      break;
    case core::PathStep::Index:
      result.insert(places.isElement(place)
                        ? places.element(base, places.fieldName(place))
                        : places.index(base));
      break;
    case core::PathStep::Deref:
      result.insert(places.deref(base));
      for (const auto &[alias, edge] : state.definiteAliases.edgesFrom(base)) {
        if (places.isDescendantOf(alias, base) ||
            places.depth(alias) >= PlaceBuilder::MaxPlaceDepth)
          continue;
        if (edge.exact()) {
          result.insert(places.deref(alias));
        } else if (edge.offset.isField() && edge.offset.negative) {
          auto mirror = places.deref(alias);
          for (const auto &field : PlaceBuilder::fieldsOfOffset(edge.offset))
            mirror = places.field(mirror, field);
          if (places.depth(mirror) <= PlaceBuilder::MaxPlaceDepth)
            result.insert(mirror);
        }
      }
      break;
    }
  }
  return {result.begin(), result.end()};
}

static void copyHeapCell(core::PlaceId source, core::PlaceId target,
                         core::AnalysisState &state) {
  if (source == target)
    return;
  state.forget(target);
  state.kinds[target] = state.kindOf(source);
  if (const auto it = state.objectViews.find(source);
      it != state.objectViews.end())
    state.objectViews[target] = it->second;
  if (const auto it = state.callTargets.find(source);
      it != state.callTargets.end())
    state.callTargets[target] = it->second;
  if (const auto it = state.heapWriteGuards.find(source);
      it != state.heapWriteGuards.end())
    state.heapWriteGuards[target] = it->second;
  if (state.definiteHeapWrites.contains(source))
    state.definiteHeapWrites.insert(target);
  if (state.heapLocalObjects.contains(source))
    state.heapLocalObjects.insert(target);
  if (state.incompleteHeap.contains(source))
    state.incompleteHeap.insert(target);
  if (const auto resource = state.resources.recordOf(source))
    state.resources.hold(target, *resource);
  if (state.resources.isNull(source))
    state.resources.markNull(target);
  if (const auto null = state.nulls.recordOf(source))
    state.nulls.set(target, *null);
  if (const auto spatial = state.spatial.recordOf(source))
    state.spatial.set(target, *spatial);
  if (const auto fact = state.scalars.factOf(source))
    state.scalars.set(target, *fact);
  if (const auto raw = state.raw.rawAt(source))
    state.raw.markRaw(target, *raw);
  if (const auto input = state.incoming.find(source);
      input != state.incoming.end())
    state.incoming[target] = input->second;
  state.pointerFacts.copyPointer(source, target);
  state.loans.copyHolder(source, target);
  if (const auto moved = state.moves.recordOf(source))
    state.moves.markMoved(target, moved->reason, moved->location,
                          moved->via.value_or(source), moved->element,
                          moved->family, moved->ownValue, moved->guard);
}

void FunctionDataflow::mirrorHeapWrite(core::PlaceId place,
                                       core::AnalysisState &state) {
  if (places.isBase(place))
    return;
  const auto mirrors = definiteMirrors(place, state);
  // A write copies the value and its held loans, not every loan against
  // every spelling of the object's storage. Replaying mirrorSubtree here
  // multiplies equivalent loans in cyclic object graphs (RFC 0013).
  for (const core::PlaceId mirror : mirrors) {
    if (mirror == place || places.depth(mirror) > PlaceBuilder::MaxPlaceDepth ||
        places.isDescendantOf(place, mirror) ||
        places.isDescendantOf(mirror, place))
      continue;
    const auto children = places.descendants(place);
    forgetBelow(mirror, state);
    copyHeapCell(place, mirror, state);
    state.aliases.unite(mirror, place);
    state.definiteAliases.unite(mirror, place);
    for (const core::PlaceId child : children) {
      const std::size_t depth =
          places.depth(mirror) + places.depth(child) - places.depth(place);
      if (depth > PlaceBuilder::MaxPlaceDepth) {
        state.incompleteHeap.insert(mirror);
        continue;
      }
      if (state.kindOf(child) == core::OwnershipKind::Unknown &&
          !state.resources.holds(child) && !state.nulls.recordOf(child) &&
          !state.spatial.has(child) && !state.scalars.factOf(child) &&
          !state.raw.isRaw(child) && !state.moves.recordOf(child) &&
          state.loans.heldBy(child).empty())
        continue;
      const auto target = places.translate(child, place, mirror);
      copyHeapCell(child, target, state);
    }
  }
}

core::HeapDescription
FunctionDataflow::describeHeap(core::PlaceId root, bool pointer,
                               const core::AnalysisState &state,
                               const clang::Expr *at) {
  core::HeapDescription graph;
  graph.incomplete = state.incompleteHeap.contains(root);
  struct Object {
    core::PlaceId place;
    core::SummaryPath path;
    bool pointer;
  };
  std::deque<Object> work;
  std::map<core::PlaceId, core::ValueSource> represented;
  const auto remember = [&](core::PlaceId place,
                            const core::SummaryPath &path) {
    for (const auto cell : definiteMirrors(place, state)) {
      represented.try_emplace(cell, core::ValueSource::copy(path));
      for (const auto &[alias, edge] : state.definiteAliases.edgesFrom(cell)) {
        represented.try_emplace(alias,
                                core::ValueSource::copyAt(path, edge.offset));
      }
    }
  };
  remember(root, core::SummaryPath::result());
  work.push_back(Object{
      .place = root, .path = core::SummaryPath::result(), .pointer = pointer});

  while (!work.empty()) {
    const Object object = std::move(work.front());
    graph.incomplete |= state.incompleteHeap.contains(object.place);
    work.pop_front();
    if (const auto null = nullnessAt(object.place, state);
        object.pointer && null && null->state == core::Nullness::Null)
      continue;
    std::vector<core::PlaceId> names{object.place};
    if (object.pointer) {
      for (const auto &[alias, edge] :
           state.definiteAliases.edgesFrom(object.place)) {
        if (edge.exact())
          names.push_back(alias);
      }
    }
    // A path is selected once, preferring the returned name's own facts.
    // Exact aliases supply fields initialized before or after publication.
    std::map<core::SummaryPath, core::PlaceId> fields;
    for (const core::PlaceId name : names) {
      for (const core::PlaceId field : places.descendants(name)) {
        std::vector<core::PlaceId> steps;
        for (core::PlaceId node = field; node != name;
             node = *places.parent(node))
          steps.push_back(node);
        std::ranges::reverse(steps);
        const auto derefs = std::ranges::count_if(steps, [&](core::PlaceId p) {
          return places.step(p) == core::PathStep::Deref;
        });
        if (derefs != (object.pointer ? 1 : 0) ||
            (object.pointer &&
             places.step(steps.front()) != core::PathStep::Deref))
          continue;
        const auto *decl =
            llvm::dyn_cast_if_present<clang::ValueDecl>(builder.declFor(field));
        const bool typedPointer =
            decl != nullptr && decl->getType()->isPointerType();
        if (!typedPointer && !state.resources.holds(field) &&
            state.kindOf(field) == core::OwnershipKind::Unknown &&
            !state.raw.isRaw(field) && state.loans.heldBy(field).empty() &&
            !state.nulls.recordOf(field))
          continue;
        core::SummaryPath path = object.path;
        for (const core::PlaceId step : steps) {
          switch (places.step(step)) {
          case core::PathStep::Deref:
            path = path.deref();
            break;
          case core::PathStep::Field:
            path = path.field(places.fieldName(step));
            break;
          case core::PathStep::Index:
            if (places.isElement(step)) {
              const auto selector = summaryArrayIndex(places.fieldName(step));
              if (!selector) {
                graph.incomplete = true;
                path = path.indexed();
              } else {
                path = path.indexed(*selector);
              }
            } else {
              path = path.indexed();
            }
            break;
          }
        }
        if (path.steps.size() > PlaceBuilder::MaxPlaceDepth) {
          graph.incomplete = true;
          continue;
        }
        fields.try_emplace(std::move(path), field);
      }
    }
    for (const auto &[path, field] : fields) {
      if (graph.fields.size() >= core::MaxHeapFields) {
        graph.incomplete = true;
        graph.normalize();
        return graph;
      }
      const auto null = nullnessAt(field, state);
      if (null && null->state == core::Nullness::Null) {
        auto nil = core::ValueSource::null();
        if (const auto when = state.heapWriteGuards.find(field);
            when != state.heapWriteGuards.end())
          nil.when = when->second;
        graph.addField(core::Store{.dest = path, .value = nil});
        continue;
      }
      core::ValueSource value;
      auto known = represented.find(field);
      if (known == represented.end()) {
        for (const auto mirror : definiteMirrors(field, state)) {
          known = represented.find(mirror);
          if (known != represented.end())
            break;
        }
      }
      std::optional<core::ValueSource> external;
      if (at != nullptr) {
        // A returned record can contain an object published through another
        // output of this same call. Reuse its graph instead of allocating
        // a second object under the record's field.
        for (const auto mirror : definiteMirrors(field, state)) {
          for (const auto &[alias, edge] :
               state.definiteAliases.edgesFrom(mirror)) {
            const auto outputPath = stableSummaryPathOf(alias);
            if (!outputPath || !state.stored.contains(*outputPath))
              continue;
            const auto output = inferred.heap.find(*outputPath);
            if (output == inferred.heap.end() ||
                !std::ranges::any_of(
                    output->second.fields, [](const core::Store &cell) {
                      return cell.dest.isRoot() && !cell.value.post;
                    }))
              continue;
            external =
                core::ValueSource::copyAt(*outputPath, edge.offset.negated());
            external->post = true;
            break;
          }
          if (external)
            break;
        }
      }
      if (known != represented.end()) {
        value = known->second;
        value.post = true;
      } else if (external) {
        value = *external;
      } else {
        ValueOrigin origin;
        origin.kind = ValueOrigin::Kind::Copy;
        origin.place = PlaceRef{.place = field,
                                .derefs = {},
                                .derefExprs = {},
                                .derefElements = {},
                                .element = {}};
        value = sourceOf(origin, state, true);
        // A published allocation is escaped for local leak accounting, but
        // is still the very allocation the output hands to its receiver.
        const auto resource = state.resources.recordOf(field);
        if (resource && resource->origin == core::ResourceOrigin::Allocated &&
            !findMoved(field, state) && !state.raw.isRaw(field) &&
            !state.incoming.contains(field)) {
          const auto spatial = state.spatial.recordOf(field);
          const auto guard = value.when;
          value = core::ValueSource::freshAt(
              resource->family,
              spatial ? spatial->offset : core::PointerOffset::zero(),
              spatial ? summaryAffineOf(foldAffine(spatial->extent, state))
                      : std::nullopt);
          value.when = guard;
        }
        if (const auto spatial = state.spatial.recordOf(field);
            spatial && spatial->string) {
          value.stringLength =
              summaryAffineOf(foldAffine(spatial->string->length, state));
          value.unterminated = spatial->string->unterminated;
        }
        remember(field, path);
        // Incoming aliases already carry their caller's reachable state.
        // Only new allocations need an exported description of their own.
        if (value.isFresh())
          work.push_back(Object{.place = field, .path = path, .pointer = true});
      }
      // A local borrow escaping inside an object has the same lifetime
      // obligation as a directly returned pointer (RFC 0013).
      if (at != nullptr) {
        for (const core::Loan &loan : state.loans.heldBy(field)) {
          if (!lifetimes.outlives(loan.lifetime, callerLifetime)) {
            reportLifetimeTooShort(field, loan.place, *at, /*returned=*/true);
            break;
          }
        }
      }
      keepInputGuard(value, state);
      if (const auto when = state.heapWriteGuards.find(field);
          when != state.heapWriteGuards.end())
        value.when = when->second;
      const bool reference = value.post;
      auto nil = core::ValueSource::null();
      nil.when = value.when;
      graph.addField(core::Store{.dest = path, .value = std::move(value)});
      if (!reference && null && null->state == core::Nullness::MaybeNull)
        graph.addField(core::Store{.dest = path, .value = nil});
    }
  }
  graph.normalize();
  return graph;
}

void FunctionDataflow::recordHeapResult(const ValueOrigin &origin,
                                        const clang::Expr &at,
                                        const core::AnalysisState &state) {
  if (origin.kind == ValueOrigin::Kind::Null)
    return;
  core::HeapDescription graph;
  if (origin.place) {
    recordArrayResult(origin.place->place, state);
    if (const auto null = nullnessAt(origin.place->place, state);
        null && null->state == core::Nullness::Null)
      return;
    const auto resource = state.resources.recordOf(origin.place->place);
    if (resource && resource->origin == core::ResourceOrigin::Allocated &&
        !state.incoming.contains(origin.place->place))
      graph = describeHeap(origin.place->place, true, state, &at);
  } else if (origin.call != nullptr) {
    // A forwarding return has no destination place. Translate the callee's
    // graph's incoming paths and affine expressions into this interface.
    if (const auto effects = classifyCall(*origin.call, summaries)) {
      const auto it = effects->summary->heap.find(core::SummaryPath::result());
      if (it != effects->summary->heap.end()) {
        graph.incomplete = it->second.incomplete;
        for (const core::Store &field : it->second.fields) {
          core::ValueSource value = field.value;
          if (!value.post) {
            const auto translated = builder.originFromSource(
                value, *origin.call, *effects->summary, true);
            value = translated ? sourceOf(*translated, state, true)
                               : core::ValueSource::unknown();
            if (translated) {
              value.stringLength =
                  summaryAffineOf(foldAffine(translated->stringLength, state));
              value.unterminated = translated->unterminated;
            }
          }
          graph.addField(core::Store{.dest = field.dest, .value = value});
        }
      }
    }
  }
  const auto [it, added] =
      inferred.heap.try_emplace(core::SummaryPath::result(), graph);
  if (!added)
    it->second.join(graph);
}

bool FunctionDataflow::isHeapOutputPath(const core::SummaryPath &path) const {
  for (const auto &[root, graph] : inferred.heap) {
    if (root.isResult() || (root != path && !root.isProperPrefixOf(path)))
      continue;
    for (const auto &field : graph.fields) {
      auto absolute = root;
      absolute.steps.insert(absolute.steps.end(), field.dest.steps.begin(),
                            field.dest.steps.end());
      if (absolute == path && !field.value.post)
        return true;
    }
  }
  return false;
}

void FunctionDataflow::recordHeapOutputs(const core::AnalysisState &state) {
  recordArrayOutputs(state);
  std::map<core::PlaceId, core::ValueSource> outputObjects;
  std::map<core::SummaryPath, core::PlaceId> outputPlaces;
  for (std::size_t i = 0; i < places.size(); ++i) {
    const core::PlaceId candidate{static_cast<std::uint32_t>(i)};
    if (const auto path = builder.summaryPathOf(candidate);
        path && state.stored.contains(*path))
      outputPlaces.try_emplace(*path, candidate);
  }
  // Final snapshots are deliberately separate from `stores`, whose union
  // includes intermediate assignments (RFC 0013, Producing a description).
  for (const core::SummaryPath &path : state.stored) {
    if (path.isResult() || (path.isParam() && !path.hasDeref()))
      continue;
    const auto found = outputPlaces.find(path);
    if (found == outputPlaces.end())
      continue;
    const core::PlaceId place = found->second;
    core::HeapDescription graph;
    const auto owned = state.resources.recordOf(place);
    if (owned && owned->origin == core::ResourceOrigin::Allocated &&
        !state.incoming.contains(place))
      graph = describeHeap(place, true, state, nullptr);
    ValueOrigin origin;
    origin.kind = ValueOrigin::Kind::Copy;
    origin.place = PlaceRef{.place = place,
                            .derefs = {},
                            .derefExprs = {},
                            .derefElements = {},
                            .element = {}};
    auto value = sourceOf(origin, state, true);
    // Legacy stores name interface cells. A final heap value may use such
    // a path as an entry identity only if it has not already been written.
    if (value.path && !value.post && state.stored.contains(*value.path) &&
        !state.incoming.contains(place))
      value = core::ValueSource::unknown();
    const auto null = nullnessAt(place, state);
    if (null && null->state == core::Nullness::Null) {
      value = core::ValueSource::null();
    } else if (const auto resource = state.resources.recordOf(place);
               resource &&
               resource->origin == core::ResourceOrigin::Allocated &&
               !findMoved(place, state) && !state.incoming.contains(place)) {
      const auto spatial = state.spatial.recordOf(place);
      value = core::ValueSource::freshAt(
          resource->family,
          spatial ? spatial->offset : core::PointerOffset::zero(),
          spatial ? summaryAffineOf(foldAffine(spatial->extent, state))
                  : std::nullopt);
      value.when = summaryGuardOf(resource->guard);
      if (spatial && spatial->string) {
        value.stringLength =
            summaryAffineOf(foldAffine(spatial->string->length, state));
        value.unterminated = spatial->string->unterminated;
      }
    }
    // Introduce one object for several output cells that hold it.
    if (!value.isNull() && !findMoved(place, state)) {
      if (const auto previous = outputObjects.find(place);
          previous != outputObjects.end()) {
        value = previous->second;
        value.post = true;
      } else {
        outputObjects.emplace(place, core::ValueSource::copy(path));
        for (const auto &[alias, edge] :
             state.definiteAliases.edgesFrom(place)) {
          outputObjects.try_emplace(
              alias, core::ValueSource::copyAt(path, edge.offset));
        }
      }
    }
    // A final snapshot describes what the write left behind, under the
    // conditions at that write. A later traversal can finish with a null
    // iterator; that exit fact must not make an earlier unconditional
    // publication conditional on an empty input list (RFC 0013).
    value.when = {};
    if (const auto when = state.heapWriteGuards.find(place);
        when != state.heapWriteGuards.end())
      value.when = when->second;
    // A reference shares the canonical output's children, too. Describing
    // them as fresh here would allocate those objects a second time.
    if (value.post)
      graph.fields.clear();
    graph.addField(
        core::Store{.dest = core::SummaryPath::result(), .value = value});
    if (!value.post && null && null->state == core::Nullness::MaybeNull) {
      auto nil = core::ValueSource::null();
      nil.when = value.when;
      graph.addField(
          core::Store{.dest = core::SummaryPath::result(), .value = nil});
    }
    const auto [it, added] = inferred.heap.try_emplace(path, graph);
    if (!added)
      it->second.join(graph);
  }
}

void FunctionDataflow::copyHeapValue(core::PlaceId source, core::PlaceId target,
                                     core::AnalysisState &state) {
  const auto children = places.descendants(source);
  const auto identities = state.definiteAliases;
  forgetBelow(target, state);
  copyHeapCell(source, target, state);
  state.aliases.unite(target, source);
  state.definiteAliases.unite(target, source);
  std::map<core::PlaceId, core::PlaceId> copied{{source, target}};
  for (const auto child : children) {
    if (places.depth(target) + places.depth(child) - places.depth(source) >
        PlaceBuilder::MaxPlaceDepth) {
      state.incompleteHeap.insert(target);
      continue;
    }
    if (state.kindOf(child) == core::OwnershipKind::Unknown &&
        !state.resources.holds(child) && !state.nulls.recordOf(child) &&
        !state.spatial.has(child) && !state.scalars.factOf(child) &&
        !state.raw.isRaw(child) && !state.moves.recordOf(child) &&
        !state.callTargets.contains(child) && !state.incoming.contains(child) &&
        state.loans.heldBy(child).empty())
      continue;
    if (copied.size() > core::MaxHeapFields) {
      state.incompleteHeap.insert(target);
      break;
    }
    const auto mirror = places.translate(child, source, target);
    copyHeapCell(child, mirror, state);
    copied.emplace(child, mirror);
  }
  for (const auto &[a, b] : identities.pairs()) {
    if (copied.contains(a) && copied.contains(b)) {
      const auto offset = *identities.offsetOf(b, a);
      state.definiteAliases.unite(copied.at(a), copied.at(b), offset);
      state.aliases.unite(copied.at(a), copied.at(b), offset);
    }
  }
}

void FunctionDataflow::captureHeapInputs(const clang::CallExpr &call,
                                         const core::FunctionSummary &summary,
                                         core::AnalysisState &state) {
  if (materializingHeap || summary.heap.empty())
    return;
  std::set<core::SummaryPath> written = summary.storeDestinations();
  for (const auto &[root, graph] : summary.heap) {
    if (root.isResult())
      continue;
    for (const auto &field : graph.fields) {
      auto path = root;
      path.steps.insert(path.steps.end(), field.dest.steps.begin(),
                        field.dest.steps.end());
      written.insert(std::move(path));
    }
  }
  const auto mayWrite = [&written](core::SummaryPath path) {
    for (;;) {
      if (written.contains(path))
        return true;
      if (path.steps.empty())
        return false;
      path.steps.pop_back();
    }
  };
  std::set<core::SummaryPath> inputs;
  std::set<core::SummaryPath> values;
  const auto captureValue = [&](const core::SummaryPath &path) {
    inputs.insert(path);
    values.insert(path);
  };
  const auto capturePointers = [&](const core::PathGuard &guard) {
    for (const auto &[pair, equal] : guard.pointers) {
      (void)equal;
      // Capture both sides together to retain their relation after writes.
      if (mayWrite(pair.first) || mayWrite(pair.second)) {
        captureValue(pair.first);
        captureValue(pair.second);
      }
    }
  };
  for (const auto &[path, effect] : summary.effects)
    capturePointers(effect.when);
  for (const auto &[outcome, effects] : summary.outcomes)
    for (const auto &[path, effect] : effects)
      capturePointers(effect.when);
  for (const auto &store : summary.stores)
    capturePointers(store.value.when);
  for (const auto &value : summary.returns)
    capturePointers(value.when);
  for (const auto &[root, graph] : summary.heap) {
    for (const core::Store &field : graph.fields) {
      capturePointers(field.value.when);
      const auto &value = field.value;
      for (const auto &[path, fact] : value.when.conditions) {
        if (mayWrite(path))
          inputs.insert(path);
      }
      if (value.kind != core::ValueSource::Kind::Copy || value.post ||
          !value.path)
        continue;
      // Unchanged arguments can be read directly. A written input cell
      // must retain its entry identity across all of this call's stores.
      if (mayWrite(*value.path))
        captureValue(*value.path);
    }
  }
  for (const auto &value : summary.returns) {
    for (const auto &[path, fact] : value.when.conditions) {
      if (mayWrite(path))
        inputs.insert(path);
    }
    if (value.kind == core::ValueSource::Kind::Copy && value.path &&
        !value.post && mayWrite(*value.path))
      captureValue(*value.path);
  }
  if (!summary.storesOn.empty()) {
    for (const core::Store &store : summary.stores) {
      if (store.dest.isResult())
        continue;
      const auto ref = builder.resolveSummaryPath(store.dest, call);
      if (ref && ref->element.isWhole() &&
          (state.resources.holds(ref->place) ||
           state.nulls.recordOf(ref->place) || state.spatial.has(ref->place) ||
           state.moves.recordOf(ref->place) || state.raw.isRaw(ref->place) ||
           !state.loans.heldBy(ref->place).empty() ||
           state.aliases.members(ref->place).size() > 1))
        captureValue(store.dest);
    }
  }
  materializingHeap = true;
  for (const auto &path : inputs) {
    const auto ref = builder.resolveSummaryPath(path, call);
    if (!ref)
      continue;
    const auto key = std::pair{&call, path};
    auto it = heapInputs.find(key);
    if (it == heapInputs.end())
      it = heapInputs
               .emplace(key,
                        places.create("incoming(" + nameOf(ref->place) + ")"))
               .first;
    pointerSnapshots.insert(it->second);
    snapshotInputPaths.erase(it->second);
    if (const auto inputPath = stableSummaryPathOf(ref->place);
        inputPath &&
        !std::ranges::any_of(state.stored,
                             [&](const core::SummaryPath &written) {
                               return written == *inputPath ||
                                      written.isProperPrefixOf(*inputPath);
                             }) &&
        !state.isOverwritten(*inputPath))
      snapshotInputPaths[it->second] = *inputPath;
    if (const auto result = summary.heap.find(core::SummaryPath::result());
        result != summary.heap.end() &&
        std::ranges::any_of(
            result->second.fields, [&](const core::Store &field) {
              return !field.value.post && field.value.path == path;
            }))
      resultHeapInputs.insert(it->second);
    if (std::ranges::any_of(summary.returns,
                            [&](const core::ValueSource &value) {
                              return !value.post && value.path == path;
                            }))
      resultHeapInputs.insert(it->second);
    heapInputEscaped[key] = state.resources.isEscaped(ref->place);
    if (!values.contains(path)) {
      // Guards need only the tested scalar/null fact. Copying an array's
      // children just to remember whether its pointer was null would add
      // value aliases with no corresponding program copy.
      forgetBelow(it->second, state);
      state.forget(it->second);
      state.kinds[it->second] = state.kindOf(ref->place);
      if (const auto fact = state.scalars.factOf(ref->place))
        state.scalars.set(it->second, *fact);
      if (const auto null = state.nulls.recordOf(ref->place))
        state.nulls.set(it->second, *null);
      if (state.resources.isNull(ref->place))
        state.resources.markNull(it->second);
      continue;
    }
    copyHeapValue(ref->place, it->second, state);
    state.heapInputEscapes[it->second] = heapInputEscaped[key];
    if (const auto entry = snapshotInputPaths.find(it->second);
        entry != snapshotInputPaths.end())
      state.incoming[it->second] = core::ValueSource::copy(entry->second);
  }
  materializingHeap = false;
}

void FunctionDataflow::retireHeapInputs(core::AnalysisState &state) {
  auto live = resultHeapInputs;
  const auto retain = [&](const core::PendingOutcome &outcome) {
    for (const auto &store : outcome.stores) {
      if (store.oldValue)
        live.insert(*store.oldValue);
    }
  };
  for (const auto &[holder, outcome] : state.pending)
    retain(outcome);
  if (lastCall)
    retain(lastCall->pending);
  // Entry snapshots are implementation temporaries, not additional program
  // owners. Keeping settled calls' aliases alive would form a clique of all
  // old outputs in a loop (RFC 0013, Boundedness and performance).
  for (const auto input : pointerSnapshots) {
    if (live.contains(input) || state.arrayRanges.contains(input) ||
        !state.kinds.contains(input))
      continue;
    forgetBelow(input, state);
    state.forget(input);
  }
}

void FunctionDataflow::restoreHeapInput(
    const core::PendingOutcome::PendingStore &store,
    core::AnalysisState &state) {
  if (!store.oldValue)
    return;
  const auto source = *store.oldValue;
  const auto dest = store.dest;
  copyHeapValue(source, dest, state);
  if (auto resource = state.resources.recordOf(dest)) {
    resource->escaped = store.oldValueEscaped;
    state.resources.hold(dest, *resource);
  }
  mirrorHeapWrite(dest, state);
}

std::optional<ValueOrigin>
FunctionDataflow::heapOrigin(const core::ValueSource &value,
                             const clang::CallExpr &call,
                             const core::FunctionSummary &summary) {
  auto origin = builder.originFromSource(value, call, summary, true);
  if (!origin)
    return std::nullopt;
  if (value.kind == core::ValueSource::Kind::Copy && value.path &&
      !value.post &&
      (!summary.effectOf(*value.path).moved ||
       summary.effectOf(*value.path).freed)) {
    const auto it = heapInputs.find(std::pair{&call, *value.path});
    if (it != heapInputs.end()) {
      origin->kind = ValueOrigin::Kind::Copy;
      origin->place = PlaceRef{.place = it->second,
                               .derefs = {},
                               .derefExprs = {},
                               .derefElements = {},
                               .element = {}};
    }
  }
  // A guard in the description refers to entry values, even after an
  // earlier root or field assignment in this same call changed its cell.
  for (const auto &[path, fact] : value.when.conditions) {
    const auto it = heapInputs.find(std::pair{&call, path});
    if (it == heapInputs.end())
      continue;
    if (const auto ref = builder.resolveSummaryPath(path, call)) {
      origin->guard.conditions.erase(ref->place);
      origin->guard.require(it->second, fact);
    }
  }
  return origin;
}

void FunctionDataflow::applyHeap(core::PlaceId dest,
                                 const core::HeapDescription &graph,
                                 const clang::CallExpr &call,
                                 const core::FunctionSummary &summary,
                                 core::AnalysisState &state) {
  if (const auto null = nullnessAt(dest, state);
      null && null->state == core::Nullness::Null)
    return;
  if (graph.incomplete)
    state.incompleteHeap.insert(dest);
  const bool wasMaterializing = materializingHeap;
  materializingHeap = true;
  std::map<core::SummaryPath, std::vector<core::ValueSource>> fields;
  for (const core::Store &field : graph.fields) {
    if (!field.dest.isRoot())
      fields[field.dest].push_back(field.value);
  }
  // Fresh objects precede references. Canonical references always name a
  // root or a non-reference field, so two passes suffice even for cycles.
  for (const bool references : {false, true}) {
    for (const auto &[path, values] : fields) {
      const bool hasReference =
          std::ranges::any_of(values, &core::ValueSource::post);
      if (references != hasReference)
        continue;
      if (places.depth(dest) + path.steps.size() >
          PlaceBuilder::MaxPlaceDepth) {
        state.incompleteHeap.insert(dest);
        continue;
      }
      const auto field = builder.resolveBelow(dest, path, &call);
      if (!field)
        continue;
      std::vector<ValueOrigin> alternatives;
      std::optional<core::Affine> commonExtent;
      bool firstExtent = true;
      bool extentAgrees = true;
      for (const core::ValueSource &value : values) {
        std::optional<ValueOrigin> origin;
        if (value.post && value.path && value.path->isResult()) {
          if (const auto target =
                  builder.resolveBelow(dest, *value.path, &call)) {
            origin = ValueOrigin{};
            origin->kind = ValueOrigin::Kind::Copy;
            origin->place = PlaceRef{.place = *target,
                                     .derefs = {},
                                     .derefExprs = {},
                                     .derefElements = {},
                                     .element = {}};
            origin->offset = value.offset;
            if (const auto guard = builder.translateGuard(value.when, call))
              origin->guard = *guard;
            for (const auto &[inputPath, fact] : value.when.conditions) {
              const auto input = heapInputs.find(std::pair{&call, inputPath});
              if (input == heapInputs.end())
                continue;
              if (const auto ref =
                      builder.resolveSummaryPath(inputPath, call)) {
                origin->guard.conditions.erase(ref->place);
                origin->guard.require(input->second, fact);
              }
            }
          }
        } else {
          origin = heapOrigin(value, call, summary);
        }
        if (!origin || !pruneOrigin(*origin, state))
          continue;
        if (origin->kind != ValueOrigin::Kind::Null) {
          std::optional<core::Affine> extent =
              foldAffine(origin->extent, state);
          if (origin->kind == ValueOrigin::Kind::Copy && origin->place) {
            if (const auto record =
                    state.spatial.recordOf(origin->place->place))
              extent = record->extent;
          }
          if (!firstExtent && commonExtent != extent)
            extentAgrees = false;
          commonExtent = extent;
          firstExtent = false;
        }
        // The child exists only if all containing pointers exist. A null
        // test retracts the resource, avoiding phantom failure-path leaks.
        for (core::PlaceId node = *field; node != dest;) {
          const auto parent = places.parent(node);
          if (!parent)
            break;
          if (places.step(node) == core::PathStep::Deref)
            origin->guard.require(*parent,
                                  core::ValueFact::of(core::Outcome::NonNull));
          node = *parent;
        }
        alternatives.push_back(std::move(*origin));
      }
      if (alternatives.empty())
        continue;
      ValueOrigin origin;
      if (alternatives.size() == 1) {
        origin = std::move(alternatives.front());
      } else {
        origin.kind = ValueOrigin::Kind::Conditional;
        origin.alternatives = std::move(alternatives);
      }
      applyPointerAssign(*field, origin, call, false, state);
      if (!extentAgrees) {
        if (auto record = state.spatial.recordOf(*field)) {
          record->extent.reset();
          state.spatial.set(*field, *record);
        }
      }
      noteCalleeStore(*field, call, state);
    }
  }
  materializingHeap = wasMaterializing;
}

void FunctionDataflow::applyHeapValue(core::PlaceId dest,
                                      const ValueOrigin &origin,
                                      core::AnalysisState &state) {
  if (materializingHeap)
    return;
  if (origin.call != nullptr) {
    applyHeapResult(dest, *origin.call, state);
    return;
  }
  if (origin.kind != ValueOrigin::Kind::Conditional)
    return;
  std::optional<core::AnalysisState> joined;
  for (const auto &alternative : origin.alternatives) {
    if (alternative.kind == ValueOrigin::Kind::Null)
      continue;
    auto branch = state;
    applyHeapValue(dest, alternative, branch);
    if (joined)
      joined->join(branch, &places);
    else
      joined = std::move(branch);
  }
  if (joined)
    state = std::move(*joined);
}

void FunctionDataflow::applyHeapResult(core::PlaceId dest,
                                       const clang::CallExpr &call,
                                       core::AnalysisState &state) {
  if (materializingHeap)
    return;
  applyArrayReallocation(dest, call, state);
  const auto effects = classifyCall(call, summaries);
  if (!effects)
    return;
  if (!call.getType()->isRecordType() && !effects->summary->returns.empty() &&
      std::ranges::all_of(
          effects->summary->returns, [](const core::ValueSource &value) {
            return value.isNull() ||
                   (value.post && value.path && !value.path->isResult());
          }))
    return;
  const auto it = effects->summary->heap.find(core::SummaryPath::result());
  if (it != effects->summary->heap.end())
    applyHeap(dest, it->second, call, *effects->summary, state);
  applyArrayResult(dest, call, state);
}

void FunctionDataflow::applyHeapOutputs(const clang::CallExpr &call,
                                        const core::FunctionSummary &summary,
                                        core::AnalysisState &state) {
  if (materializingHeap)
    return;
  std::set<core::SummaryPath> applied;
  for (const auto &[root, graph] : summary.heap) {
    if (root.isResult())
      continue;
    core::HeapDescription remaining;
    remaining.incomplete = graph.incomplete;
    for (const auto &field : graph.fields) {
      if (field.dest.isRoot())
        continue;
      auto absolute = root;
      absolute.steps.insert(absolute.steps.end(), field.dest.steps.begin(),
                            field.dest.steps.end());
      if (!applied.contains(absolute))
        remaining.addField(field);
    }
    if (const auto dest = builder.resolveSummaryPath(root, call))
      applyHeap(dest->place, remaining, call, summary, state);
    for (const auto &field : graph.fields) {
      auto absolute = root;
      absolute.steps.insert(absolute.steps.end(), field.dest.steps.begin(),
                            field.dest.steps.end());
      applied.insert(std::move(absolute));
    }
  }
}

} // namespace weavec::analysis
