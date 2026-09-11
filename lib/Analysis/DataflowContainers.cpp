//===- DataflowContainers.cpp - Linked-container contracts (RFC 0023) -----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

#include "clang/AST/RecordLayout.h"
#include "clang/Basic/Version.h"

#include <algorithm>
#include <deque>
#include <utility>

using namespace clang;

namespace weavec::analysis {

static QualType containerRecordType(const RecordDecl *record,
                                    const ASTContext &ctx) {
#if CLANG_VERSION_MAJOR >= 23
  return ctx.getCanonicalTagType(record);
#else
  return ctx.getRecordType(record);
#endif
}

static const RecordDecl *containerRecord(QualType type) {
  if (type.isNull())
    return nullptr;
  if (type->isPointerType())
    type = type->getPointeeType();
  const auto *record = type->getAsRecordDecl();
  return record && record->isCompleteDefinition() && !record->isUnion()
             ? record->getDefinition()
             : nullptr;
}

static std::optional<core::ContainerField>
containerField(const FieldDecl &field, const ASTContext &context) {
  if (field.isBitField() || field.getName().empty() ||
      field.getType().isVolatileQualified() ||
      field.getType()->isAtomicType() || field.getType()->isIncompleteType() ||
      field.getType()->isVariablyModifiedType())
    return std::nullopt;
  const auto bytes = context.getTypeSizeInChars(field.getType()).getQuantity();
  const auto bits = context.getASTRecordLayout(field.getParent())
                        .getFieldOffset(field.getFieldIndex());
  if (bytes <= 0 || bits % context.getCharWidth() != 0)
    return std::nullopt;
  return core::ContainerField{.name = field.getNameAsString(),
                              .offset = bits / context.getCharWidth(),
                              .bytes = static_cast<std::uint64_t>(bytes)};
}

const core::ContainerShape *
FunctionDataflow::containerShape(QualType type) const {
  if (containerShapes.empty())
    return nullptr;
  const auto *record = containerRecord(type);
  const auto it = containerShapes.find(record);
  return it == containerShapes.end() ? nullptr : &it->second;
}

void FunctionDataflow::discoverContainers() {
  std::map<const FieldDecl *, unsigned> candidates;
  std::set<const RecordDecl *> releases;
  std::set<const RecordDecl *> writes;
  std::map<const RecordDecl *, std::set<const FieldDecl *>> payloads;
  const auto note = [&](const Expr *expr, unsigned weight) {
    const auto *member =
        expr ? dyn_cast<MemberExpr>(expr->IgnoreParenImpCasts()) : nullptr;
    const auto *field =
        member ? dyn_cast<FieldDecl>(member->getMemberDecl()) : nullptr;
    if (field && field->getType()->isPointerType() &&
        containerRecord(field->getType()) == field->getParent())
      candidates[field] += weight;
  };
  std::vector<const Stmt *> workItems{function.getBody()};
  for (std::size_t i = 0; i < workItems.size() && workItems.size() < 65536;
       ++i) {
    const auto *stmt = workItems[i];
    if (!stmt)
      continue;
    if (const auto *assignment = dyn_cast<BinaryOperator>(stmt);
        assignment && assignment->getOpcode() == BO_Assign) {
      note(assignment->getRHS(), 2);
      note(assignment->getLHS(), 1);
      if (const auto *member =
              dyn_cast<MemberExpr>(assignment->getLHS()->IgnoreParenImpCasts()))
        if (const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl()))
          writes.insert(field->getParent());
      if (const auto *rhs =
              dyn_cast<MemberExpr>(assignment->getRHS()->IgnoreParenImpCasts()))
        if (const auto *field = dyn_cast<FieldDecl>(rhs->getMemberDecl());
            field && field->getType()->isPointerType() &&
            containerRecord(field->getType()) == field->getParent())
          if (const auto lhs = builder.resolve(*assignment->getLHS()); lhs)
            if (const auto base = builder.resolve(*rhs->getBase());
                base && base->place == lhs->place)
              note(rhs, 8);
    }
    if (const auto *decls = dyn_cast<DeclStmt>(stmt))
      for (const auto *decl : decls->decls())
        if (const auto *var = dyn_cast<VarDecl>(decl); var && var->hasInit())
          note(var->getInit(), 2);
    if (const auto *call = dyn_cast<CallExpr>(stmt);
        call && call->getNumArgs() == 1)
      if (const auto *callee = call->getDirectCallee();
          callee && callee->getName() == "free") {
        if (const auto *record = containerRecord(
                call->getArg(0)->IgnoreParenImpCasts()->getType()))
          releases.insert(record);
        if (const auto *member =
                dyn_cast<MemberExpr>(call->getArg(0)->IgnoreParenImpCasts()))
          if (const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl()))
            payloads[field->getParent()].insert(field);
      }
    for (const auto *child : stmt->children())
      workItems.push_back(child);
  }
  std::map<const RecordDecl *, std::pair<const FieldDecl *, unsigned>> chosen;
  for (const auto &[field, score] : candidates) {
    auto &best = chosen[field->getParent()];
    if (score > best.second)
      best = {field, score};
    else if (score == best.second)
      best.first = nullptr;
  }
  for (const auto &[record, selected] : chosen) {
    if (!selected.first)
      continue;
    const auto type = core::ObjectType::parse(
        checkedObjectType(containerRecordType(record, context)));
    const auto link = containerField(*selected.first, context);
    if (!type || !link)
      continue;
    core::ContainerShape shape{.object = *type,
                               .link = *link,
                               .initialized = {},
                               .payloads = {},
                               .family = {},
                               .access = core::ContainerAccess::Read};
    bool valid = true;
    for (const auto *field : record->fields()) {
      const auto descriptor = containerField(*field, context);
      if (!descriptor) {
        valid = false;
        break;
      }
      shape.initialized.push_back(*descriptor);
    }
    if (writes.contains(record))
      shape.access = core::ContainerAccess::Write;
    if (releases.contains(record)) {
      shape.access = core::ContainerAccess::Release;
      shape.family = "free";
      for (const auto *field : payloads[record]) {
        const auto descriptor = containerField(*field, context);
        if (!descriptor || field == selected.first) {
          valid = false;
          break;
        }
        shape.payloads.push_back({.field = *descriptor, .family = "free"});
      }
    }
    std::ranges::sort(shape.initialized);
    std::ranges::sort(shape.payloads);
    if (valid && shape.valid()) {
      containerShapes.emplace(record, shape);
      containerRecords.emplace(shape.object.toString(), record);
    }
  }
}

core::ContainerFact
FunctionDataflow::containerInput(core::PlaceId holder,
                                 const core::SummaryPath &path,
                                 const core::ContainerShape &shape) {
  const auto [entry, inserted] =
      containerInputIds.try_emplace(std::pair{holder, shape.encode()});
  if (inserted)
    entry->second = places.create("container entry witness");
  const auto input = entry->second;
  containerInputs[input] = path;
  containerInputShapes[input] = shape;
  return {.shape = shape,
          .members = {input},
          .inputs = {input},
          .allocationCompatible = true};
}

void FunctionDataflow::refineContainers(core::AnalysisState &state) {
  std::vector<std::pair<core::PlaceId, core::ContainerFact>> refined;
  for (const auto &[holder, fact] : state.safety->containers.all())
    if (!fact.empty)
      if (const auto nullness = nullnessAt(holder, state);
          nullness && nullness->state == core::Nullness::Null) {
        auto empty = fact;
        empty.empty = true;
        empty.members.clear();
        empty.tailOf.reset();
        empty.releasedPayloads.clear();
        empty.shape.terminal = true;
        refined.emplace_back(holder, std::move(empty));
      }
  for (auto &[holder, fact] : refined)
    state.safety->containers.set(holder, std::move(fact));
  if (state.safety->containers.limited() && recording())
    inferred.checked.limited = true;
}

void FunctionDataflow::initializeContainers(core::AnalysisState &state) {
  for (const auto *param : function.parameters()) {
    if (!param->getType()->isPointerType() || getAnnotations(*param).raw)
      continue;
    const auto *shape = containerShape(param->getType());
    if (!shape)
      continue;
    const auto place = builder.placeForVar(*param);
    state.safety->containers.set(
        place,
        containerInput(place,
                       core::SummaryPath::param(param->getFunctionScopeIndex()),
                       *shape));
  }
}

std::optional<core::ContainerFact>
FunctionDataflow::strengthenContainer(const core::ContainerFact &fact,
                                      const core::ContainerShape &required) {
  if (fact.entails(required))
    return fact;
  if (!fact.allocationCompatible || !fact.releasedPayloads.empty() ||
      (fact.inputs.empty() && !fact.localAllocation) ||
      fact.shape.object != required.object ||
      fact.shape.link != required.link ||
      fact.shape.payloads != required.payloads ||
      (required.access == core::ContainerAccess::Release &&
       required.family != "free"))
    return std::nullopt;
  auto result = fact;
  result.shape.access = required.access;
  result.shape.family = required.family;
  if (!result.shape.entails(required))
    return std::nullopt;
  result.inputs.clear();
  for (const auto input : fact.inputs) {
    const auto source = containerInputs.find(input);
    const auto descriptor = containerInputShapes.find(input);
    if (source == containerInputs.end() ||
        descriptor == containerInputShapes.end() ||
        descriptor->second.object != required.object ||
        descriptor->second.link != required.link ||
        descriptor->second.payloads != required.payloads)
      return std::nullopt;
    auto premise = descriptor->second;
    premise.access = required.access;
    premise.family = required.family;
    auto witness = containerInput(input, source->second, premise);
    result.inputs.insert(witness.inputs.begin(), witness.inputs.end());
  }
  return result;
}

std::optional<core::ContainerFact>
FunctionDataflow::containerAt(core::PlaceId holder,
                              const core::ContainerShape &shape,
                              core::AnalysisState &state, bool allowInput) {
  if (state.nulls.stateOf(holder) == core::Nullness::Null)
    return core::ContainerFact{
        .shape = shape, .members = {}, .inputs = {}, .empty = true};
  if (state.moves.recordOf(holder) || state.raw.isRaw(holder))
    return std::nullopt;
  if (const auto *fact = state.safety->containers.find(holder))
    if (const auto sufficient = strengthenContainer(*fact, shape)) {
      state.safety->containers.set(holder, *sufficient);
      return sufficient;
    }
  if (places.step(holder) == core::PathStep::Field) {
    const auto parent = places.parent(holder);
    const auto pointer = parent && places.step(*parent) == core::PathStep::Deref
                             ? places.parent(*parent)
                             : std::nullopt;
    if (pointer && places.fieldName(holder) == shape.link.name)
      if (const auto *fact = state.safety->containers.find(*pointer);
          fact && fact->entails(shape)) {
        auto tail = *fact;
        tail.suffix = true;
        tail.tailOf = *pointer;
        tail.empty = fact->shape.terminal;
        if (tail.empty)
          tail.members.clear();
        tail.releasedPayloads.clear();
        return tail;
      }
  }
  if (allowInput && !state.safety->containers.blocked(holder) &&
      !state.safety->havoc && !state.safety->replacedPointers.contains(holder))
    if (const auto path = stableSummaryPathOf(holder);
        path && path->isParam()) {
      auto input = containerInput(holder, *path, shape);
      state.safety->containers.set(holder, input);
      snapshotContainerOutput(holder, state);
      return input;
    }
  const auto memory = checkedMemoryAt(holder, {}, {}, state);
  return memory ? establishContainer(*memory, shape, state) : std::nullopt;
}

std::optional<core::ContainerFact> FunctionDataflow::captureContainer(
    core::PlaceId dest, const ValueOrigin &origin, core::AnalysisState &state) {
  if (containerShapes.empty() && state.safety->containers.all().empty())
    return std::nullopt;
  const auto *decl = dyn_cast_or_null<ValueDecl>(builder.declFor(dest));
  const auto *shape = decl ? containerShape(decl->getType()) : nullptr;
  if (!shape && origin.kind == ValueOrigin::Kind::Null)
    if (const auto *previous = state.safety->containers.find(dest))
      shape = &previous->shape;
  if (!shape && origin.place) {
    const auto source = origin.place->place;
    if (const auto *sourceDecl =
            dyn_cast_or_null<ValueDecl>(builder.declFor(source)))
      shape = containerShape(sourceDecl->getType());
    if (!shape && places.step(source) == core::PathStep::Field) {
      const auto object = places.parent(source);
      const auto pointer =
          object && places.step(*object) == core::PathStep::Deref
              ? places.parent(*object)
              : std::nullopt;
      if (pointer)
        if (const auto *fact = state.safety->containers.find(*pointer);
            fact && places.fieldName(source) == fact->shape.link.name)
          shape = &fact->shape;
    }
  }
  if (origin.kind == ValueOrigin::Kind::Null && shape)
    return core::ContainerFact{
        .shape = *shape, .members = {}, .inputs = {}, .empty = true};
  if (origin.kind != ValueOrigin::Kind::Copy || !origin.place ||
      !origin.offset.isZero())
    return std::nullopt;
  if (state.moves.recordOf(origin.place->place) ||
      state.raw.isRaw(origin.place->place))
    return std::nullopt;
  if (const auto *fact = state.safety->containers.find(origin.place->place))
    return *fact;
  if (shape)
    return containerAt(origin.place->place, *shape, state, true);
  return std::nullopt;
}

bool FunctionDataflow::requireContainer(const core::ContainerFact &fact,
                                        const Stmt &at,
                                        core::AnalysisState &state) {
  (void)state;
  for (const auto input : fact.inputs) {
    const auto path = containerInputs.find(input);
    if (path == containerInputs.end())
      return false;
    if (recording())
      inferred.checked.require(
          {.kind = core::CheckedRequirementKind::Container,
           .path = path->second,
           .other = {},
           .begin = {},
           .end = {},
           .family = containerInputShapes.at(input).encode()});
  }
  safetyObligation(
      core::SafetyProperty::Semantics,
      core::safetyOutcome(fact.inputs.empty(), !fact.inputs.empty()), at,
      "container", "linked access requires an established container chain");
  return true;
}

bool FunctionDataflow::checkedContainerAccess(const Expr &expr, bool read,
                                              bool write,
                                              core::AnalysisState &state) {
  if (state.safety->containers.all().empty())
    return false;
  const auto *member = dyn_cast<MemberExpr>(expr.IgnoreParenImpCasts());
  if (!member || !member->isArrow())
    return false;
  const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
  const auto holder = builder.resolvePointerValue(*member->getBase());
  if (!field || !holder || !holder->element.isWhole())
    return false;
  const auto *fact = state.safety->containers.find(holder->place);
  if (!fact || fact->empty || state.moves.recordOf(holder->place) ||
      state.raw.isRaw(holder->place) ||
      state.nulls.stateOf(holder->place) != core::Nullness::NonNull)
    return false;
  const auto descriptor = containerField(*field, context);
  if (!descriptor ||
      std::ranges::find(fact->shape.initialized, *descriptor) ==
          fact->shape.initialized.end() ||
      (write && fact->shape.access == core::ContainerAccess::Read) ||
      checkedObjectType(member->getBase()->getType()->getPointeeType()) !=
          fact->shape.object.toString())
    return false;
  if (!requireContainer(*fact, expr, state))
    return false;
  const auto outcome =
      core::safetyOutcome(fact->inputs.empty(), !fact->inputs.empty());
  safetyObligation(core::SafetyProperty::Bounds, outcome, expr,
                   "container field", "container node field fits its object");
  safetyObligation(core::SafetyProperty::Validity, outcome, expr,
                   "container node",
                   "container cursor identifies live node storage");
  if (read)
    safetyObligation(core::SafetyProperty::Initialization, outcome, expr,
                     "container field", "container node field is initialized");
  if (write)
    safetyObligation(core::SafetyProperty::Validity, outcome, expr,
                     "container write", "container node storage is writable");
  return true;
}

std::optional<core::ContainerFact>
FunctionDataflow::establishContainer(const CheckedMemory &memory,
                                     const core::ContainerShape &shape,
                                     core::AnalysisState &state) {
  const RecordDecl *record = nullptr;
  if (memory.holder)
    if (const auto *decl =
            dyn_cast_or_null<ValueDecl>(builder.declFor(*memory.holder)))
      record = containerRecord(decl->getType());
  if (!record)
    if (const auto *decl =
            dyn_cast_or_null<ValueDecl>(builder.declFor(memory.storage)))
      record = containerRecord(decl->getType());
  if (!record)
    if (const auto found = containerRecords.find(shape.object.toString());
        found != containerRecords.end())
      record = found->second;
  if (!record || checkedObjectType(containerRecordType(record, context)) !=
                     shape.object.toString())
    return std::nullopt;
  std::map<std::string, const FieldDecl *> fields;
  for (const auto *field : record->fields())
    fields[field->getNameAsString()] = field;
  const auto matchesField = [&](const core::ContainerField &field) {
    const auto actual = fields.find(field.name);
    return actual != fields.end() &&
           containerField(*actual->second, context) == field;
  };
  if (!matchesField(shape.link) ||
      !std::ranges::all_of(shape.initialized, matchesField))
    return std::nullopt;
  core::ContainerGraph graph;
  bool owned = true;
  bool local = true;
  std::deque<CheckedMemory> workItems{memory};
  while (!workItems.empty()) {
    const auto current = workItems.front();
    workItems.pop_front();
    if (graph.nodes.contains(current.storage))
      continue;
    if (graph.nodes.size() >= core::MaxContainerNodes ||
        !checkedValid(current, state) ||
        current.begin != core::Affine::ofConstant(0) || !current.extent ||
        !checkedInterval({},
                         core::Affine::ofConstant(
                             static_cast<std::int64_t>(shape.object.bytes)),
                         *current.extent, state))
      return std::nullopt;
    const auto *objectDecl =
        dyn_cast_or_null<ValueDecl>(builder.declFor(current.storage));
    if (objectDecl) {
      if (checkedObjectType(objectDecl->getType()) != shape.object.toString() ||
          std::cmp_less(context.getDeclAlign(objectDecl).getQuantity(),
                        shape.object.alignment))
        return std::nullopt;
    } else {
      const auto object = state.safety->objectTypes.find(current.storage);
      if (object == state.safety->objectTypes.end() ||
          object->second != shape.object.toString())
        return std::nullopt;
    }
    auto whole = current;
    whole.end =
        core::Affine::ofConstant(static_cast<std::int64_t>(shape.object.bytes));
    core::ContainerNode node{
        .object = shape.object,
        .initialized = {},
        .next = {},
        .payloads = {},
        .family = {},
        .live = true,
        .writable = checkedWritePermission(whole, state).value_or(false)};
    if (current.holder)
      if (const auto resource = state.resources.recordOf(*current.holder)) {
        node.family = resource->family;
        node.allocationBase =
            !resource->escaped &&
            resource->origin == core::ResourceOrigin::Allocated;
      }
    owned &= node.allocationBase && node.family == "free";
    local &= std::ranges::any_of(checkedObjects, [&](const auto &entry) {
      return entry.second == current.storage;
    });
    for (const auto &field : shape.initialized) {
      auto part = current;
      part.begin =
          core::Affine::ofConstant(static_cast<std::int64_t>(field.offset));
      part.end = core::Affine::ofConstant(
          static_cast<std::int64_t>(field.offset + field.bytes));
      if (checkedInitialized(part, state))
        node.initialized.insert(field);
    }
    const auto *storageDecl =
        dyn_cast_or_null<ValueDecl>(builder.declFor(current.storage));
    auto parent =
        current.holder ? places.deref(*current.holder) : current.storage;
    if (storageDecl && containerRecord(storageDecl->getType()) &&
        !storageDecl->getType()->isPointerType())
      parent = current.storage;
    const auto link = fields.find(shape.link.name);
    if (link == fields.end())
      return std::nullopt;
    const auto cell = builder.fieldPlace(parent, *link->second);
    if (state.nulls.stateOf(cell) == core::Nullness::Null) {
      node.next = core::ContainerEdge::null();
    } else if (const auto next = checkedMemoryAt(cell, {}, {}, state)) {
      node.next = core::ContainerEdge::to(next->storage);
      workItems.push_back(*next);
    }
    for (const auto &payload : shape.payloads) {
      const auto field = fields.find(payload.field.name);
      if (field == fields.end())
        return std::nullopt;
      const auto payloadCell = builder.fieldPlace(parent, *field->second);
      if (state.nulls.stateOf(payloadCell) == core::Nullness::Null) {
        node.payloads[payload.field.name] = core::ContainerEdge::null();
        continue;
      }
      const auto resource = state.resources.recordOf(payloadCell);
      const auto bytes = checkedMemoryAt(payloadCell, {}, {}, state);
      if (!bytes || !state.safety->pointers.contains(payloadCell) ||
          state.moves.recordOf(payloadCell) || !resource || resource->escaped ||
          resource->origin != core::ResourceOrigin::Allocated ||
          resource->family != payload.family ||
          bytes->begin != core::Affine::ofConstant(0))
        return std::nullopt;
      node.payloads[payload.field.name] =
          core::ContainerEdge::to(bytes->storage);
      if (!graph.nodes.contains(bytes->storage))
        graph.nodes.emplace(bytes->storage,
                            core::ContainerNode{.object = {},
                                                .initialized = {},
                                                .next = {},
                                                .payloads = {},
                                                .family = resource->family,
                                                .live = true,
                                                .writable = true,
                                                .allocationBase = true});
    }
    graph.nodes.insert_or_assign(current.storage, std::move(node));
  }
  const auto proof =
      graph.prove(core::ContainerEdge::to(memory.storage), shape);
  if (!proof.complete())
    return std::nullopt;
  auto members = proof.members;
  members.insert(proof.payloads.begin(), proof.payloads.end());
  auto established = shape;
  bool freshCapability =
      owned && local && shape.access == core::ContainerAccess::Release;
  if (owned && local && shape.payloads.empty()) {
    auto releasable = shape;
    releasable.access = core::ContainerAccess::Release;
    releasable.family = "free";
    if (graph.prove(core::ContainerEdge::to(memory.storage), releasable)
            .complete()) {
      established = std::move(releasable);
      freshCapability = true;
    }
  }
  return core::ContainerFact{.shape = std::move(established),
                             .members = std::move(members),
                             .inputs = {},
                             .allocationCompatible = freshCapability,
                             .localAllocation = freshCapability};
}

void FunctionDataflow::checkedContainerCall(
    const core::CheckedRequirement &requirement, const CallExpr &call,
    core::AnalysisState &state) {
  if (requirement.kind == core::CheckedRequirementKind::ContainerSeparated) {
    const auto empty = [&](const core::SummaryPath &path) {
      if (path.isParam() && path.isRoot() && path.index < call.getNumArgs() &&
          builder.classifyValue(*call.getArg(path.index)).kind ==
              ValueOrigin::Kind::Null)
        return true;
      const auto arguments = containerArguments.find(&call);
      if (arguments == containerArguments.end())
        return false;
      const auto input = arguments->second.find(path);
      return input != arguments->second.end() && input->second.empty;
    };
    const auto first = builder.resolveSummaryPath(requirement.path, call);
    const auto second = builder.resolveSummaryPath(requirement.other, call);
    const bool proved =
        empty(requirement.path) || empty(requirement.other) ||
        (first && second &&
         separateContainers(first->place, second->place, call, state));
    safetyObligation(core::SafetyProperty::Aliasing,
                     proved ? core::SafetyOutcome::Proven
                            : core::SafetyOutcome::Unresolved,
                     call, "container separation",
                     "callee requires disjoint container footprints");
    return;
  }
  const auto shape = core::ContainerShape::decode(requirement.family);
  if (!shape)
    return;
  std::optional<core::ContainerFact> fact;
  if (requirement.path.isParam() && requirement.path.isRoot() &&
      requirement.path.index < call.getNumArgs()) {
    const auto *argument = call.getArg(requirement.path.index);
    const auto origin = builder.classifyValue(*argument);
    if (const auto *nested = dyn_cast<CallExpr>(argument->IgnoreParenCasts()))
      if (const auto posts = containerPosts.find(nested);
          posts != containerPosts.end())
        for (const auto &post : posts->second)
          if (post.path.isResult() && post.path.isRoot() &&
              (!post.on || post.on == core::Outcome::NonNull))
            if (const auto sufficient = strengthenContainer(post.fact, *shape))
              fact = *sufficient;
    // A nested call's checked postcondition was captured before outer effects.
    if (!fact) {
      if (origin.kind == ValueOrigin::Kind::Null)
        fact = core::ContainerFact{
            .shape = *shape, .members = {}, .inputs = {}, .empty = true};
      else if (origin.kind == ValueOrigin::Kind::Copy && origin.place &&
               origin.offset.isZero())
        fact = containerAt(origin.place->place, *shape, state, true);
      else if (const auto memory = checkedMemory(*argument, {}, {}, state))
        fact = establishContainer(*memory, *shape, state);
    }
  } else if (const auto place =
                 builder.resolveSummaryPath(requirement.path, call)) {
    fact = containerAt(place->place, *shape, state, true);
  }
  const bool proved = fact && requireContainer(*fact, call, state);
  if (proved) {
    containerArguments[&call][requirement.path] = *fact;
    if (const auto holder = builder.resolveSummaryPath(requirement.path, call))
      state.safety->containers.set(holder->place, *fact);
  }
  safetyObligation(
      core::SafetyProperty::Call,
      proved ? core::safetyOutcome(fact->inputs.empty(), !fact->inputs.empty())
             : core::SafetyOutcome::Unresolved,
      call, "container argument",
      "callee container chain precondition must hold");
}

bool FunctionDataflow::checkedContainerRelease(const CallExpr &call,
                                               core::AnalysisState &state) {
  const auto origin = builder.classifyValue(*call.getArg(0));
  if (!origin.place || !origin.offset.isZero())
    return false;
  const auto holder = origin.place->place;
  if (places.step(holder) == core::PathStep::Field) {
    const auto parent = places.parent(holder);
    const auto pointer = parent && places.step(*parent) == core::PathStep::Deref
                             ? places.parent(*parent)
                             : std::nullopt;
    const auto *head =
        pointer ? state.safety->containers.find(*pointer) : nullptr;
    if (head && !head->empty &&
        head->shape.access == core::ContainerAccess::Release &&
        state.nulls.stateOf(*pointer) == core::Nullness::NonNull &&
        !head->releasedPayloads.contains(
            std::string(places.fieldName(holder))) &&
        !state.moves.recordOf(holder))
      for (const auto &payload : head->shape.payloads)
        if (payload.field.name == places.fieldName(holder) &&
            payload.family == "free") {
          if (!requireContainer(*head, call, state))
            return false;
          auto consumed = *head;
          consumed.releasedPayloads.insert(payload.field.name);
          state.safety->containers.set(*pointer, std::move(consumed));
          containerPayloadReleases.insert_or_assign(&call, *pointer);
          safetyObligation(
              core::SafetyProperty::Release, core::SafetyOutcome::Required,
              call, "container payload",
              "container payload has a separate matching release capability");
          return true;
        }
  }
  const auto *fact = state.safety->containers.find(holder);
  if (!fact || fact->shape.access != core::ContainerAccess::Release ||
      fact->shape.family != "free" || state.moves.recordOf(holder))
    return false;
  if (!requireContainer(*fact, call, state))
    return false;
  containerReleases.insert_or_assign(&call, holder);
  safetyObligation(
      core::SafetyProperty::Release,
      core::safetyOutcome(fact->inputs.empty(), !fact->inputs.empty()), call,
      "container node",
      "container node has the matching allocation release capability");
  return true;
}

} // namespace weavec::analysis
