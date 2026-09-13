//===- DataflowUnions.cpp - Checked union member views (RFC 0025) ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"

#include "clang/AST/RecordLayout.h"

using namespace clang;

namespace weavec::analysis {

std::string FunctionDataflow::checkedUnionMember(const FieldDecl &field) {
  const auto *record = field.getParent();
  if (!record->isUnion() || !record->isCompleteDefinition() ||
      field.getName().empty() || field.isBitField())
    return {};
  const auto type = field.getType();
  if ((!type->isScalarType() || type->isComplexType()) ||
      type.isVolatileQualified() || type->isAtomicType())
    return {};
  const auto object = core::ObjectType::parse(
      checkedObjectType(context.getCanonicalTagType(record)));
  const auto value = core::ObjectType::parse(checkedObjectType(type));
  return object && value ? core::UnionMember{.object = *object,
                                             .value = *value,
                                             .name = field.getNameAsString(),
                                             .pointer = type->isPointerType()}
                               .encode()
                         : std::string{};
}

core::PlaceId
FunctionDataflow::checkedUnionStorage(core::PlaceId place,
                                      const core::AnalysisState &state) {
  const auto images = borrowedImages(place, state);
  return images.size() == 1 ? images.front() : place;
}

void FunctionDataflow::checkedUnionSet(core::PlaceId storage,
                                       const FieldDecl &field,
                                       core::AnalysisState &state) {
  if (!options.checkContracts)
    return;
  storage = checkedUnionStorage(storage, state);
  state.safety->unions.invalidate(storage);
  state.safety->unions.set(storage, checkedUnionMember(field));
  const auto holder = builder.fieldPlace(storage, field);
  if (const auto bytes = byteSizeOf(field.getType(), context)) {
    state.safety->initialize(
        holder, {.begin = {}, .end = core::Affine::ofConstant(*bytes)});
    auto root = storage;
    std::int64_t offset = 0;
    bool represented = true;
    while (places.parent(root) && places.step(root) == core::PathStep::Field) {
      const auto *parentField =
          dyn_cast_or_null<FieldDecl>(builder.declFor(root));
      if (!parentField) {
        represented = false;
        break;
      }
      const auto bits = context.getASTRecordLayout(parentField->getParent())
                            .getFieldOffset(parentField->getFieldIndex());
      const auto displacement =
          static_cast<std::int64_t>(bits / context.getCharWidth());
      if (__builtin_add_overflow(offset, displacement, &offset)) {
        represented = false;
        break;
      }
      root = *places.parent(root);
    }
    std::int64_t end = 0;
    if (represented && !__builtin_add_overflow(offset, *bytes, &end))
      state.safety->initialize(root, {.begin = core::Affine::ofConstant(offset),
                                      .end = core::Affine::ofConstant(end)});
  }
  if (field.getType()->isPointerType() &&
      state.safety->unions.members.contains(storage))
    if (const auto memory = checkedMemoryAt(holder, {}, {}, state))
      state.safety->unions.members[storage].front().pointer =
          core::UnionPointer{.holder = holder,
                             .storage = memory->storage,
                             .offset = memory->begin,
                             .extent = memory->extent,
                             .input = memory->inputPlace,
                             .valid = checkedValid(*memory, state)};
}

void FunctionDataflow::checkedUnionRequirement(core::PlaceId storage,
                                               std::string_view member,
                                               const Stmt &at,
                                               core::AnalysisState &state) {
  storage = checkedUnionStorage(storage, state);
  bool proved = false;
  if (const auto found = state.safety->unions.members.find(storage);
      found != state.safety->unions.members.end())
    for (const auto &witness : found->second) {
      auto condition = witness.when;
      if (witness.member == member && pruneGuard(condition, state) &&
          condition.trivial()) {
        proved = true;
        if (witness.pointer) {
          const auto &pointer = *witness.pointer;
          if (!state.safety->invalidatedPointers.contains(pointer.holder) &&
              !state.moves.recordOf(pointer.holder) &&
              !state.resources.isEscaped(pointer.holder)) {
            state.safety->positions[pointer.holder] = {.storage =
                                                           pointer.storage,
                                                       .offset = pointer.offset,
                                                       .extent = pointer.extent,
                                                       .input = pointer.input};
            state.safety->objects[pointer.holder] = pointer.storage;
            if (pointer.valid) {
              state.safety->pointers.insert(pointer.holder);
              state.nulls.set(pointer.holder,
                              {.state = core::Nullness::NonNull,
                               .location = {},
                               .reason = core::NullReason::Declared});
            }
          }
        }
      }
    }
  auto input = stableSummaryPathOf(storage);
  if (!input)
    for (const auto mirror : definiteMirrors(storage, state))
      if (const auto path = stableSummaryPathOf(mirror)) {
        input = path;
        break;
      }
  const auto condition = guardHere(state);
  const auto guard = summaryGuardOf(condition);
  // Entry member evidence cannot be introduced after an overlapping write,
  // even when this is the first member read in the function.
  const bool written =
      !proved && std::ranges::any_of(
                     state.safety->writtenStorage, [&](core::PlaceId place) {
                       place = checkedUnionStorage(place, state);
                       return place == storage ||
                              places.isDescendantOf(place, storage) ||
                              places.isDescendantOf(storage, place);
                     });
  const bool required =
      !proved && !written && core::UnionMember::decode(member) && input &&
      !input->isResult() && state.safety->unions.mayRequire(storage) &&
      !state.isOverwritten(*input) && summaryGuardComplete(condition, guard);
  if (required && recording())
    inferred.checked.require({.kind = core::CheckedRequirementKind::UnionMember,
                              .path = *input,
                              .other = {},
                              .family = std::string(member),
                              .when = guard});
  const bool deferred =
      options.deferCheckedCalls && state.safety->deferred.contains(storage);
  if (deferred && recording())
    inferred.checked.deferred = true;
  safetyObligation(core::SafetyProperty::Initialization,
                   core::safetyOutcome(proved, required || deferred), at,
                   "union member",
                   "read requires an initialized compatible union member");
}

void FunctionDataflow::checkedUnionAccess(const Expr &expr, Role role,
                                          core::AnalysisState &state) {
  if (role != Role::Read && role != Role::ReadWrite && role != Role::Consume)
    return;
  // An identifier reads its own variable, not an overlapping member slot.
  if (isa<DeclRefExpr>(expr.IgnoreParenImpCasts()))
    return;
  const auto *member = dyn_cast<MemberExpr>(expr.IgnoreParenImpCasts());
  if (member) {
    const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
    if (!field || !field->getParent()->isUnion())
      return;
  }
  const auto ref = builder.resolve(expr);
  const auto view = ref ? std::optional(checkedUnionStorage(ref->place, state))
                        : std::nullopt;
  const FieldDecl *field = nullptr;
  if (member)
    field = dyn_cast<FieldDecl>(member->getMemberDecl());
  else if (view)
    field = dyn_cast_or_null<FieldDecl>(builder.declFor(*view));
  if (field &&
      !ASTContext::hasSameUnqualifiedType(expr.getType(), field->getType()))
    field = nullptr;
  if (!field || !field->getParent()->isUnion())
    return;
  const auto storage = view ? places.parent(*view) : std::nullopt;
  if (storage && options.deferCheckedCalls &&
      state.safety->deferred.contains(checkedUnionStorage(*storage, state))) {
    state.safety->deferred.insert(*view);
    if (ref)
      state.safety->deferred.insert(ref->place);
  }
  if (storage)
    checkedUnionRequirement(*storage, checkedUnionMember(*field), expr, state);
  else
    safetyObligation(core::SafetyProperty::Semantics,
                     core::SafetyOutcome::Unresolved, expr, "union member",
                     "union storage cannot be represented");
}

void FunctionDataflow::checkedUnionWrite(
    const Expr &expr, const std::optional<CheckedMemory> &memory,
    core::AnalysisState &state) {
  if (isa<DeclRefExpr>(expr.IgnoreParenImpCasts())) {
    if (!expr.getType()->isRecordType())
      checkedUnionClobber(memory, expr, state);
    return;
  }
  const auto ref = builder.resolve(expr);
  const auto view = ref ? std::optional(checkedUnionStorage(ref->place, state))
                        : std::nullopt;
  const auto *member = dyn_cast<MemberExpr>(expr.IgnoreParenImpCasts());
  const FieldDecl *field = nullptr;
  if (member)
    field = dyn_cast<FieldDecl>(member->getMemberDecl());
  else if (view)
    field = dyn_cast_or_null<FieldDecl>(builder.declFor(*view));
  if (field &&
      !ASTContext::hasSameUnqualifiedType(expr.getType(), field->getType()))
    field = nullptr;
  if (!field || !field->getParent()->isUnion()) {
    if (!expr.getType()->isRecordType())
      checkedUnionClobber(memory, expr, state);
    return;
  }
  const auto parent = view ? places.parent(*view) : std::nullopt;
  if (!parent)
    return;
  const auto storage = checkedUnionStorage(*parent, state);
  // Preserve the newly assigned field while retiring every other union view
  // that this write may overlap. Different indirect names are not disjoint.
  state.safety->unions.members.erase(storage);
  checkedUnionClobber(memory, expr, state);
  std::vector<core::PlaceId> retired;
  for (const auto *sibling : field->getParent()->fields()) {
    if (sibling == field)
      continue;
    for (const auto root : {*parent, storage}) {
      const auto place = builder.fieldPlace(root, *sibling);
      retired.push_back(place);
      const auto below = places.descendants(place);
      retired.insert(retired.end(), below.begin(), below.end());
    }
  }
  std::ranges::sort(retired);
  retired.erase(std::ranges::unique(retired).begin(), retired.end());
  checkLeaks(
      retired,
      [&](core::PlaceId place) {
        return std::ranges::binary_search(retired, place);
      },
      LeakForm::Overwritten, locate(expr), state);
  for (const auto place : retired) {
    snapshotIntegerDependencies(place, nullptr, state);
    snapshotScalar(place, nullptr, state);
    state.dropGuardsOn(place);
    state.forget(place);
  }
  checkedUnionSet(storage, *field, state);
}

void FunctionDataflow::checkedUnionClobber(
    const std::optional<CheckedMemory> &memory, const Stmt &at,
    core::AnalysisState &state) {
  if (memory && checkedAtMost(memory->end, memory->begin, state))
    return;
  std::vector<core::PlaceId> affected;
  for (const auto &[storage, witnesses] : state.safety->unions.members) {
    auto root = storage;
    std::int64_t offset = 0;
    bool represented = true;
    while (places.step(root) == core::PathStep::Field) {
      const auto *field = dyn_cast_or_null<FieldDecl>(builder.declFor(root));
      const auto parent = places.parent(root);
      if (!field || !parent) {
        represented = false;
        break;
      }
      const auto bits = context.getASTRecordLayout(field->getParent())
                            .getFieldOffset(field->getFieldIndex());
      const auto displacement = bits / context.getCharWidth();
      if (displacement > static_cast<std::uint64_t>(INT64_MAX) ||
          __builtin_add_overflow(
              offset, static_cast<std::int64_t>(displacement), &offset)) {
        represented = false;
        break;
      }
      root = *parent;
    }
    if (memory && represented && root == memory->storage &&
        !witnesses.empty()) {
      const auto view = core::UnionMember::decode(witnesses.front().member);
      std::int64_t end = 0;
      if (view && view->object.bytes <= static_cast<std::uint64_t>(INT64_MAX) &&
          !__builtin_add_overflow(
              offset, static_cast<std::int64_t>(view->object.bytes), &end) &&
          (checkedAtMost(memory->end, core::Affine::ofConstant(offset),
                         state) ||
           checkedAtMost(core::Affine::ofConstant(end), memory->begin, state)))
        continue;
    } else if (memory &&
               ((isLocalStorage(root) && isLocalStorage(memory->storage) &&
                 places.root(root) != places.root(memory->storage)) ||
                (isLocalStorage(root) && memory->input))) {
      continue;
    }
    affected.push_back(storage);
  }
  for (const auto storage : affected) {
    const auto retired = places.descendants(storage);
    checkLeaks(
        retired,
        [&](core::PlaceId place) {
          return places.isDescendantOf(place, storage);
        },
        LeakForm::Overwritten, locate(at), state);
    state.safety->unions.invalidate(storage);
    for (const auto place : retired) {
      snapshotIntegerDependencies(place, nullptr, state);
      snapshotScalar(place, nullptr, state);
      state.dropGuardsOn(place);
      state.forget(place);
    }
  }
  if (!memory)
    state.safety->unions.invalidateAll();
}

void FunctionDataflow::checkedUnionCall(const CallExpr &call,
                                        const CallEffects &effects,
                                        core::AnalysisState &state) {
  // This pass only retires existing witnesses. Write history is recorded
  // independently by the ordinary checked call transfer.
  if (state.safety->unions.members.empty())
    return;
  if (const auto writes = checkedWrites.find(&call);
      writes != checkedWrites.end())
    for (const auto &memory : writes->second)
      checkedUnionClobber(memory, call, state);
  for (const auto &[path, effect] : effects.summary->effects) {
    if (!effect.written)
      continue;
    const auto ref = builder.resolveSummaryPath(path, call);
    if (!ref)
      continue;
    const auto storage = checkedUnionStorage(ref->place, state);
    auto parent = std::optional(storage);
    while (parent) {
      if (state.safety->unions.members.contains(*parent)) {
        // A scalar/pointer output can switch the view; the established
        // member is installed only from the checked callee postcondition.
        state.safety->unions.invalidate(*parent);
        break;
      }
      parent = places.parent(*parent);
    }
  }
}

void FunctionDataflow::checkedUnionOutputs(core::CheckedContract &outputs,
                                           const Expr *value,
                                           std::optional<core::Outcome> outcome,
                                           const core::AnalysisState &state) {
  const auto returned = value && value->getType()->isRecordType()
                            ? builder.resolve(*value)
                            : std::nullopt;
  for (const auto &[storage, witnesses] : state.safety->unions.members) {
    auto path = stableSummaryPathOf(storage);
    if (returned && (storage == returned->place ||
                     places.isDescendantOf(storage, returned->place))) {
      path = core::SummaryPath::result();
      for (auto cursor = storage; cursor != returned->place;
           cursor = *places.parent(cursor))
        path->steps.insert(path->steps.begin(),
                           {.step = places.step(cursor),
                            .field = std::string(places.fieldName(cursor))});
    }
    if (!path || path->steps.size() > core::MaxHeapPathDepth ||
        (path->isParam() && path->isRoot()))
      continue;
    for (const auto &witness : witnesses) {
      auto condition = witness.when;
      if (!pruneGuard(condition, state))
        continue;
      const auto guard = summaryGuardOf(condition);
      if (!summaryGuardComplete(condition, guard))
        continue;
      outputs.establish({.kind = core::CheckedRequirementKind::UnionMember,
                         .path = *path,
                         .other = {},
                         .family = witness.member,
                         .when = guard,
                         .on = outcome});
    }
  }
}

void FunctionDataflow::applyCheckedUnionPosts(
    const CallExpr &call, core::AnalysisState &state,
    std::optional<core::PlaceId> result) {
  const auto found = checkedPosts.find(&call);
  if (found == checkedPosts.end())
    return;
  std::map<core::PlaceId, std::vector<core::UnionWitness>> established;
  for (const auto &post : found->second) {
    if (post.unionMember.empty() || post.path.isResult() != result.has_value())
      continue;
    const auto output = result ? builder.resolveBelow(*result, post.path, &call)
                               : std::optional<core::PlaceId>{};
    const auto ref =
        !result ? builder.resolveSummaryPath(post.path, call) : std::nullopt;
    auto storage = output;
    if (!storage && ref)
      storage = ref->place;
    if (!storage)
      continue;
    auto guard = post.range.when;
    if (post.on) {
      const auto fact = scalarFactOf(call, state);
      if (!fact || !fact->implies(core::ValueFact::of(*post.on))) {
        const auto outcome = numericCallResult(call);
        if (!outcome || guard.size() == core::MaxGuardConjuncts)
          continue;
        guard.require(*outcome, core::ValueFact::of(*post.on));
      }
    }
    established[checkedUnionStorage(*storage, state)].push_back(
        {.member = post.unionMember, .when = std::move(guard)});
  }
  for (auto &[storage, witnesses] : established) {
    std::ranges::sort(witnesses);
    witnesses.erase(std::ranges::unique(witnesses).begin(), witnesses.end());
    state.safety->unions.invalidate(storage);
    if (witnesses.size() <= core::MaxUnionAlternatives &&
        state.safety->unions.members.size() < core::MaxUnionObjects)
      state.safety->unions.members[storage] = std::move(witnesses);
  }
}

} // namespace weavec::analysis
