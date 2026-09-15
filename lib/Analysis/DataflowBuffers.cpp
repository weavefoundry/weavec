//===- DataflowBuffers.cpp - Contiguous invariants (RFC 0026) ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "AffineSupport.h"
#include "Dataflow.h"
#include "IntegerSupport.h"

#include "clang/AST/RecordLayout.h"
#include "clang/Basic/Version.h"

#include <algorithm>
#include <ranges>

using namespace clang;
namespace weavec::analysis {

std::optional<core::BufferShape>
FunctionDataflow::discoverBufferShape(const RecordDecl &record) {
  return summaries.bufferShape(
      record, [&]() -> std::optional<core::BufferShape> {
        if (!record.isCompleteDefinition() || record.isUnion())
          return std::nullopt;
        const FieldDecl *data = nullptr;
        std::vector<const FieldDecl *> counts;
        for (const auto *field : record.fields()) {
          const auto type = field->getType();
          if (field->isBitField() || field->getName().empty() ||
              type.isVolatileQualified() || type->isAtomicType())
            return std::nullopt;
          if (type->isPointerType() && !type->isFunctionPointerType()) {
            if (data)
              return std::nullopt;
            data = field;
          } else if (type->isUnsignedIntegerType() && !type->isBooleanType()) {
            counts.push_back(field);
          }
        }
        if (!data || counts.size() != 2)
          return std::nullopt;
        const auto element = data->getType()->getPointeeType();
        const auto unit = byteSizeOf(element, context);
        if (!unit || *unit <= 0 || !element->isScalarType() ||
            element->isFunctionType())
          return std::nullopt;
#if CLANG_VERSION_MAJOR >= 23
        const auto type = context.getCanonicalTagType(&record);
#else
    const auto type = context.getRecordType(&record);
#endif
        const auto objectType =
            core::ObjectType::parse(checkedObjectType(type));
        if (!objectType)
          return std::nullopt;
        const auto field = [&](const FieldDecl &decl) {
          return core::ContainerField{
              .name = decl.getNameAsString(),
              .offset = context.getASTRecordLayout(&record).getFieldOffset(
                            decl.getFieldIndex()) /
                        context.getCharWidth(),
              .bytes = static_cast<std::uint64_t>(
                  context.getTypeSizeInChars(decl.getType()).getQuantity())};
        };
        // Look for the count used to select cells of this record's data field.
        // Discovery is shared across the TU's definitions so constructors and
        // reserve helpers agree with indexing helpers. This nominates a
        // relation; callers still have to establish its physical and
        // initialized storage.
        std::array<unsigned, 2> indexed{};
        std::vector<const Stmt *> usage;
        for (const auto *decl : context.getTranslationUnitDecl()->decls())
          if (const auto *definition = dyn_cast<FunctionDecl>(decl);
              definition && definition->doesThisDeclarationHaveABody())
            usage.push_back(definition->getBody());
        for (std::size_t i = 0; i < usage.size() && usage.size() <= 65536;
             ++i) {
          if (!usage[i])
            continue;
          if (const auto *index = dyn_cast<ArraySubscriptExpr>(usage[i])) {
            const auto *base =
                dyn_cast<MemberExpr>(index->getBase()->IgnoreParenImpCasts());
            if (base && base->getMemberDecl() == data) {
              const Expr *selector = index->getIdx()->IgnoreParenImpCasts();
              if (const auto *adjustment = dyn_cast<UnaryOperator>(selector))
                selector = adjustment->getSubExpr()->IgnoreParenImpCasts();
              if (const auto *member = dyn_cast<MemberExpr>(selector))
                for (unsigned c = 0; c < 2; ++c)
                  if (member->getMemberDecl() == counts[c])
                    ++indexed.at(c);
            }
          }
          for (const auto *child : usage[i]->children())
            usage.push_back(child);
        }
        if (indexed[1] > indexed[0])
          std::swap(counts[0], counts[1]);
        core::BufferShape shape{.object = *objectType,
                                .data = field(*data),
                                .length = field(*counts[0]),
                                .capacity = field(*counts[1]),
                                .elementBytes =
                                    static_cast<std::uint64_t>(*unit),
                                .pointerElements = element->isPointerType()};
        return shape.valid() ? std::optional(shape) : std::nullopt;
      });
}

void FunctionDataflow::registerBuffer(core::PlaceId object,
                                      const RecordDecl &record) {
  if (bufferObjects.contains(object))
    return;
  if (!record.isCompleteDefinition() || record.isUnion())
    return;
  if (bufferObjects.size() >= core::MaxBufferFacts) {
    inferred.checked.limited = true;
    inferred.incomplete.insert("buffer instance limit reached");
    return;
  }
  auto found = bufferShapes.find(&record);
  if (found == bufferShapes.end()) {
    if (bufferShapes.size() >= core::MaxBufferShapes) {
      inferred.checked.limited = true;
      inferred.incomplete.insert("buffer shape limit reached");
      return;
    }
    const auto shape = discoverBufferShape(record);
    if (!shape)
      return;
    found = bufferShapes.emplace(&record, *shape).first;
  }
  for (const auto *field : record.fields())
    (void)builder.fieldPlace(object, *field);
  bufferObjects[object] = found->second;
}

void FunctionDataflow::discoverBuffers() {
  // Nomination must not intern unused places for every ordinary C pointer or
  // record. Those places enlarge all subsequent alias and heap traversals.
  const auto containsBuffer = [&](auto &&self, QualType type,
                                  unsigned depth) -> bool {
    const auto *record = type->getAsRecordDecl();
    if (!record || !record->isCompleteDefinition() || record->isUnion() ||
        depth >= core::MaxHeapPathDepth)
      return false;
    if (discoverBufferShape(*record))
      return true;
    return std::ranges::any_of(record->fields(), [&](const auto *field) {
      return field->getType()->isRecordType() &&
             self(self, field->getType(), depth + 1);
    });
  };
  const auto fields = [&](auto &&self, core::PlaceId place, QualType type,
                          unsigned depth) -> void {
    const auto *record = type->getAsRecordDecl();
    if (!record || !record->isCompleteDefinition() || record->isUnion() ||
        depth >= core::MaxHeapPathDepth)
      return;
    registerBuffer(place, *record);
    for (const auto *field : record->fields())
      if (field->getType()->isRecordType() &&
          containsBuffer(containsBuffer, field->getType(), depth + 1))
        self(self, builder.fieldPlace(place, *field), field->getType(),
             depth + 1);
  };
  const auto add = [&](const VarDecl &var) {
    const auto type = var.getType()->isPointerType()
                          ? var.getType()->getPointeeType()
                          : var.getType();
    if (!containsBuffer(containsBuffer, type, 0))
      return;
    auto place = builder.placeForVar(var);
    if (var.getType()->isPointerType())
      place = places.deref(place);
    fields(fields, place, type, 0);
  };
  for (const auto *param : function.parameters())
    add(*param);
  std::vector<const Stmt *> work{function.getBody()};
  for (std::size_t i = 0; i < work.size() && work.size() <= 65536; ++i) {
    if (!work[i])
      continue;
    if (const auto *decls = dyn_cast<DeclStmt>(work[i]))
      for (const auto *decl : decls->decls())
        if (const auto *var = dyn_cast<VarDecl>(decl))
          add(*var);
    for (const auto *child : work[i]->children())
      work.push_back(child);
  }
}

const core::BufferFact *
FunctionDataflow::bufferFact(core::PlaceId data,
                             const core::AnalysisState &state) {
  if (!state.safety || state.safety->invalidatedPointers.contains(data) ||
      state.moves.recordOf(data) || state.raw.isRaw(data))
    return nullptr;
  const auto found = state.safety->buffers.values.find(data);
  return found == state.safety->buffers.values.end() ? nullptr : &found->second;
}

void FunctionDataflow::initializeBuffers(core::AnalysisState &state) {
  for (const auto &[object, shape] : bufferObjects) {
    const auto path = builder.summaryPathOf(object);
    if (!path || !path->isParam() || path->steps.empty() ||
        path->steps.front().step != core::PathStep::Deref ||
        !std::ranges::all_of(path->steps | std::views::drop(1),
                             [](const auto &step) {
                               return step.step == core::PathStep::Field;
                             }))
      continue;
    const auto data = places.field(object, shape.data.name);
    const auto length = places.field(object, shape.length.name);
    const auto capacity = places.field(object, shape.capacity.name);
    // Do not demand an initialized input container from a constructor. This
    // is only a candidate filter: declining an entry premise grants no facts.
    std::optional<Role> firstUse;
    const auto visit = [&](auto &&self, const Stmt *statement) -> void {
      if (!statement || firstUse)
        return;
      if (const auto *member = dyn_cast<MemberExpr>(statement)) {
        const auto ref = builder.resolve(*member);
        if (ref && (ref->place == data || ref->place == length ||
                    ref->place == capacity)) {
          const auto role = roles.find(member);
          firstUse = role == roles.end() ? Role::Read : role->second;
          return;
        }
      }
      for (const auto *child : statement->children())
        self(self, child);
    };
    visit(visit, function.getBody());
    // Nominate a forwarded premise when its callee has a proved contract.
    // Carrying predicates through already-incomplete calls adds work without
    // making those calls complete. Direct count accesses nominate
    // independently.
    std::optional<core::BufferShape> forwarded;
    const auto forwards = [&](auto &&self, const Stmt *statement) -> void {
      if (!statement)
        return;
      if (const auto *call = dyn_cast<CallExpr>(statement))
        if (const auto effects = classifyCall(*call, summaries);
            effects && effects->summary && effects->summary->checked.complete())
          for (const auto &requirement :
               effects->summary->checked.requirements) {
            if (requirement.kind != core::CheckedRequirementKind::Buffer)
              continue;
            const auto ref =
                builder.resolveSummaryPath(requirement.path, *call, true);
            const auto targetShape =
                core::BufferShape::decode(requirement.family);
            if (ref && ref->place == object && targetShape &&
                targetShape->sameLayoutAs(shape)) {
              if (!forwarded)
                forwarded = shape;
              forwarded->ownsBacking |= targetShape->ownsBacking;
              forwarded->ownsElements |= targetShape->ownsElements;
              forwarded->terminated |= targetShape->terminated;
            }
          }
      for (const auto *child : statement->children())
        self(self, child);
    };
    forwards(forwards, function.getBody());
    if ((!firstUse && !forwarded) || firstUse == Role::Write)
      continue;
    bool readsCount = false;
    const auto inspectCounts = [&](auto &&self, const Stmt *statement) -> void {
      if (!statement)
        return;
      if (const auto *member = dyn_cast<MemberExpr>(statement)) {
        const auto ref = builder.resolve(*member);
        const auto role = roles.find(member);
        if (ref && (ref->place == length || ref->place == capacity) &&
            (role == roles.end() || role->second == Role::Read ||
             role->second == Role::ReadWrite))
          readsCount = true;
      }
      for (const auto *child : statement->children())
        self(self, child);
    };
    inspectCounts(inspectCounts, function.getBody());
    // A destructor or pointer-only accessor does not need the stronger
    // relation between length and capacity (e.g. after a steal).
    if (!readsCount && !forwarded)
      continue;
    auto premise = forwarded.value_or(shape);
    const auto consumes = [&](auto &&self, const Stmt *statement) -> bool {
      if (!statement)
        return false;
      if (const auto *call = dyn_cast<CallExpr>(statement))
        if (const auto effects = classifyCall(*call, summaries);
            effects && effects->summary)
          for (const auto &[effectPath, effect] : effects->summary->effects) {
            if (!effect.consumed() ||
                (!effect.family.empty() && effect.family != "free"))
              continue;
            const auto ref =
                builder.resolveSummaryPath(effectPath, *call, true);
            if (ref && ref->place == data)
              return true;
          }
      return std::ranges::any_of(statement->children(), [&](const auto *child) {
        return self(self, child);
      });
    };
    premise.ownsBacking |= consumes(consumes, function.getBody());
    if (shape.pointerElements)
      for (const auto &[loop, cleanup] : arrayCleanupLoops) {
        (void)loop;
        const auto ref = builder.resolve(*cleanup.element->getBase());
        const auto count = builder.affineOf(*cleanup.count);
        if (ref && ref->place == data && count == core::Affine::ofPlace(length))
          premise.ownsElements = true;
      }
    checkedInputObjects[data] = places.deref(data);
    bufferEntryBackings.insert(data);
    state.safety->buffers.set(data, {.shape = premise,
                                     .object = object,
                                     .length = length,
                                     .capacity = capacity,
                                     .initialized = true,
                                     .entryBacking = data});
    if (shape.pointerElements) {
      auto [entry, inserted] = bufferEntries.try_emplace(data);
      if (inserted) {
        entry->second.origin = places.create("buffer entry sequence");
        entry->second.length = places.create("buffer entry length");
        snapshotPlaces.insert(entry->second.length);
        auto countPath = *path;
        countPath.steps.pushBack(
            {.step = core::PathStep::Field, .field = shape.length.name});
        snapshotInputPaths[entry->second.length] = countPath;
      }
      state.relations.learn(entry->second.length, core::Relation::Equal,
                            length);
      state.relations.learnAtLeast(entry->second.length, 0);
      state.safety->buffers.sequences[data] = entry->second;
    }
    inferred.checked.require({.kind = core::CheckedRequirementKind::Buffer,
                              .path = *path,
                              .other = {},
                              .family = premise.encode()});
    for (const auto place : {length, capacity}) {
      const auto *decl = dyn_cast_or_null<ValueDecl>(builder.declFor(place));
      if (decl)
        if (const auto type = integerTypeOf(*decl, context))
          if (!state.scalars.factOf(place))
            state.scalars.set(place, core::ValueFact::ofInteger(
                                         core::IntegerRange::full(*type)));
    }
    // Nominate append endpoints before the grow/no-grow join. Nomination
    // creates no assertion; each incoming edge must prove its own bound.
    const auto *lengthDecl =
        dyn_cast_or_null<ValueDecl>(builder.declFor(length));
    const auto lengthType =
        lengthDecl ? integerTypeOf(*lengthDecl, context) : std::nullopt;
    if (lengthType)
      for (const auto *param : function.parameters()) {
        const auto type = integerTypeOf(*param, context);
        if (!type || *type != *lengthType || type->isSigned)
          continue;
        const auto count = builder.placeForVar(*param);
        const auto stored = state.numericValues.find(count);
        const auto value = stored == state.numericValues.end()
                               ? NumericExpression::input(count, *type)
                               : stored->second;
        if (const auto sum = NumericExpression::operation(
                core::IntegerOp::Add, NumericExpression::input(length, *type),
                value)) {
          (void)internIntegerExpression(*sum, state);
          // Keep the symbolic candidate even when a call case currently
          // knows its value. Both branches can then retain the same bound.
          if (!expressionPlaces.contains(*sum)) {
            const auto symbol = places.create("buffer append endpoint");
            expressionPlaces.emplace(*sum, symbol);
            numericExpressions.emplace(symbol, *sum);
            const auto evaluated = evaluateNumericExpression(*sum, state);
            if (!evaluated.mayBeInvalid)
              state.scalars.set(symbol,
                                core::ValueFact::ofInteger(evaluated.values));
          }
        }
      }
    if (const auto entry = bufferEntries.find(data);
        entry != bufferEntries.end())
      if (const auto count = state.scalars.factOf(length))
        state.scalars.set(entry->second.length, *count);
  }
  materializeBuffers(state);
}

void FunctionDataflow::materializeBuffers(core::AnalysisState &state) {
  // Every buffer fact and conditional-validity position originates from a
  // registered buffer object (including imported call arguments). Registration
  // lasts for this function run, even after that object's facts are retired.
  // Ordinary pointer-heavy functions therefore need no buffer-domain scan.
  if (bufferObjects.empty())
    return;
  auto &buffers = state.safety->buffers;
  const auto trim = [&](auto &records) {
    while (records.size() > core::MaxBufferFacts) {
      records.erase(std::prev(records.end()));
      buffers.limited = true;
    }
  };
  trim(buffers.sequences);
  trim(buffers.pending);
  trim(buffers.pendingSequences);
  trim(buffers.bounds);
  inferred.checked.limited |= buffers.limited;
  if (buffers.limited)
    inferred.incomplete.insert("buffer fact limit reached");
  std::vector<std::pair<core::PlaceId, core::BufferFact>> activated;
  for (auto &[data, posts] : buffers.pending)
    std::erase_if(posts, [&](const auto &post) {
      auto guard = post.when;
      if (!pruneGuard(guard, state))
        return true;
      if (!guard.trivial())
        return false;
      activated.emplace_back(data, post.fact);
      return true;
    });
  std::erase_if(buffers.pending,
                [](const auto &entry) { return entry.second.empty(); });
  for (const auto &[data, fact] : activated) {
    state.safety->invalidatedPointers.erase(data);
    state.moves.reinitialize(data);
    buffers.set(data, fact);
  }
  materializeBufferSequences(state);
  hasBufferPositions |= !buffers.values.empty();
  if (hasBufferPositions)
    for (const auto &[holder, position] : state.safety->positions)
      if (position.validWhenNonempty && position.extent &&
          !state.safety->invalidatedPointers.contains(holder) &&
          !state.moves.recordOf(holder) && !state.resources.isEscaped(holder) &&
          checkedAtMost(core::Affine::ofConstant(1), *position.extent, state)) {
        state.safety->pointers.insert(holder);
        state.nulls.set(holder, {.state = core::Nullness::NonNull,
                                 .location = {},
                                 .reason = core::NullReason::Declared});
      }
  for (const auto &[data, entries] : state.safety->buffers.bounds) {
    if (!bufferFact(data, state))
      continue;
    for (const auto &bound : entries) {
      auto when = bound.when;
      if (!pruneGuard(when, state) || !when.trivial())
        continue;
      const auto minimum = foldAffine(bound.minimum, state);
      if (!minimum.place)
        state.relations.learnAtLeast(bound.capacity, minimum.constant);
      else if (minimum.scale == 1)
        state.relations.learn(bound.capacity, core::Relation::GreaterEqual,
                              *minimum.place, minimum.constant);
    }
  }
  for (const auto &[data, fact] : state.safety->buffers.values) {
    if (!bufferFact(data, state))
      continue;
    state.relations.learnAtLeast(fact.length, 0);
    state.relations.learnAtLeast(fact.capacity, 0);
    state.relations.learn(fact.length, core::Relation::LessEqual,
                          fact.capacity);
    if (const auto *decl =
            dyn_cast_or_null<ValueDecl>(builder.declFor(fact.length)))
      if (const auto type = integerTypeOf(*decl, context)) {
        state.scalars.set(fact.length,
                          core::ValueFact::ofInteger(
                              integerRangeAt(fact.length, *type, state)));
        if (const auto entry = bufferEntries.find(data);
            entry != bufferEntries.end())
          state.scalars.set(entry->second.length,
                            core::ValueFact::ofInteger(integerRangeAt(
                                entry->second.length, *type, state)));
      }
    if (checkedAtMost(core::Affine::ofPlace(fact.length, 1, 1),
                      core::Affine::ofPlace(fact.capacity), state))
      state.relations.learn(fact.length, core::Relation::Less, fact.capacity);
    if (const auto *decl =
            dyn_cast_or_null<ValueDecl>(builder.declFor(fact.capacity)))
      if (const auto type = integerTypeOf(*decl, context)) {
        const auto size = integerTypeOf(context.getSizeType(), context);
        if (size) {
          const auto upper =
              std::min(type->mask(), size->mask() / fact.shape.elementBytes);
          const auto bound = core::IntegerRange::between(
              core::IntegerValue::ofBits(*type, 0),
              core::IntegerValue::ofBits(*type, upper));
          const auto actual = state.scalars.factOf(fact.capacity);
          state.scalars.set(
              fact.capacity,
              core::ValueFact::ofInteger(
                  actual ? actual->inType(*type).intersect(bound) : bound));
        }
      }
    const auto unit = static_cast<std::int64_t>(fact.shape.elementBytes);
    // Canonicalize current parameter bounds before joining a reserve-success
    // edge (whose bound uses an entry snapshot) with the no-growth edge.
    for (const auto *param : function.parameters()) {
      if (!param->getType()->isUnsignedIntegerType())
        continue;
      const auto place = builder.placeForVar(*param);
      if (checkedAtMost(core::Affine::ofPlace(place),
                        core::Affine::ofPlace(fact.capacity), state))
        state.relations.learn(place, core::Relation::LessEqual, fact.capacity);
    }
    if (bufferEntryBackings.contains(data))
      for (const auto &[symbol, expression] : numericExpressions) {
        if (!expression.dependsOn(fact.length))
          continue;
        const auto sum = core::Affine::ofPlace(symbol);
        const auto cap = core::Affine::ofPlace(fact.capacity);
        if (checkedAtMost(sum, cap, state))
          state.relations.learn(symbol, core::Relation::LessEqual,
                                fact.capacity);
        if (checkedAtMost(core::Affine::ofPlace(symbol, 1, 1), cap, state))
          state.relations.learn(symbol, core::Relation::Less, fact.capacity);
      }
    const auto extent = core::Affine::ofPlace(fact.capacity, unit);
    const auto bytes = core::Affine::ofPlace(fact.length, unit);
    const auto storage = state.safety->objects.contains(data)
                             ? state.safety->objects.at(data)
                             : places.deref(data);
    if (!state.safety->positions.contains(data)) {
      state.safety->positions[data] = {.storage = storage,
                                       .offset = {},
                                       .extent = extent,
                                       .input = {},
                                       .validWhenNonempty = true};
    } else {
      auto &position = state.safety->positions.at(data);
      if (position.extent && checkedAtMost(*position.extent, extent, state) &&
          checkedAtMost(extent, *position.extent, state))
        position.validWhenNonempty = true;
    }
    if (fact.initialized)
      state.safety->initialize(storage, {.begin = {}, .end = bytes});
    if (fact.shape.terminated) {
      state.relations.learn(fact.length, core::Relation::Less, fact.capacity);
      const auto through = core::Affine::ofPlace(fact.length, 1, 1);
      state.safety->initialize(storage, {.begin = {}, .end = through});
      state.safety->initialize(
          storage, {.begin = bytes, .end = through, .zeroed = true});
      const core::TerminationWitness witness{
          .begin = {}, .zero = bytes, .input = {}, .when = {}};
      auto &witnesses = state.safety->termination[storage];
      if (std::ranges::find(witnesses, witness) == witnesses.end())
        witnesses.push_back(witness);
    }
    if (fact.nonNull ||
        checkedAtMost(core::Affine::ofConstant(1),
                      core::Affine::ofPlace(fact.capacity), state)) {
      state.safety->pointers.insert(data);
      state.nulls.set(data, {.state = core::Nullness::NonNull,
                             .location = {},
                             .reason = core::NullReason::Declared});
    }
    auto spatial = state.spatial.recordOf(data).value_or(core::SpatialRecord{});
    // Keep stronger allocation-time bounds. Between `data = replacement`
    // and `capacity = new_capacity`, the old advertised capacity can be
    // smaller than the new allocation; folding must not erase its size.
    if (!spatial.extent) {
      spatial.extent = extent;
      state.spatial.set(data, spatial);
    }
  }
}

void FunctionDataflow::normalizeBuffers(core::AnalysisState &state) {
  if (bufferObjects.empty() || state.safety->havoc)
    return;
  for (const auto &[object, shape] : bufferObjects) {
    const auto data = places.field(object, shape.data.name);
    if (const auto current = state.safety->buffers.values.find(data);
        current != state.safety->buffers.values.end() &&
        current->second.initialized)
      continue;
    if (state.moves.recordOf(data) || state.raw.isRaw(data) ||
        state.safety->invalidatedPointers.contains(data)) {
      continue;
    }
    const auto length = places.field(object, shape.length.name);
    const auto capacity = places.field(object, shape.capacity.name);
    const auto len = foldAffine(core::Affine::ofPlace(length), state);
    const auto cap = foldAffine(core::Affine::ofPlace(capacity), state);
    if (!checkedAtMost({}, len, state) || !checkedAtMost(len, cap, state)) {
      continue;
    }
    if (cap.isConstant() && cap.constant == 0 &&
        state.nulls.stateOf(data) == core::Nullness::Null) {
      auto established = shape;
      established.ownsBacking = true;
      established.ownsElements = shape.pointerElements;
      state.safety->buffers.set(data, {.shape = established,
                                       .object = object,
                                       .length = length,
                                       .capacity = capacity,
                                       .initialized = true});
      continue;
    }
    const auto unit = static_cast<std::int64_t>(shape.elementBytes);
    const auto capacityBytes = cap.times(unit);
    const auto lengthBytes = len.times(unit);
    if (!capacityBytes || !lengthBytes)
      continue;
    const auto memory = checkedMemoryAt(data, {}, *capacityBytes, state);
    const auto backing = state.safety->buffers.storage.find(data);
    if (backing != state.safety->buffers.storage.end()) {
      auto established = backing->second;
      established.shape.ownsElements =
          shape.pointerElements && len.isConstant() && len.constant == 0;
      established.initialized =
          lengthBytes->isConstant() && lengthBytes->constant == 0;
      if (memory) {
        auto prefix = *memory;
        prefix.end = *lengthBytes;
        established.initialized |= checkedInitialized(prefix, state);
      }
      state.safety->buffers.set(data, established);
      continue;
    }
    if (!memory || memory->begin != core::Affine::ofConstant(0) ||
        !memory->extent || !checkedValid(*memory, state) ||
        !checkedInterval({}, *capacityBytes, *memory->extent, state) ||
        checkedWritePermission(*memory, state) != true) {
      continue;
    }
    auto prefix = *memory;
    prefix.end = *lengthBytes;
    auto established = shape;
    const auto resource = state.resources.recordOf(data);
    established.ownsBacking =
        resource && resource->family == "free" && !resource->escaped;
    established.ownsElements =
        shape.pointerElements && len.isConstant() && len.constant == 0;
    state.safety->buffers.set(
        data, {.shape = established,
               .object = object,
               .length = length,
               .capacity = capacity,
               .initialized = checkedInitialized(prefix, state),
               .nonNull = true,
               .entryBacking = bufferEntryBackings.contains(data) && resource &&
                                       resource->origin ==
                                           core::ResourceOrigin::Allocated &&
                                       !resource->escaped
                                   ? std::optional(data)
                                   : std::nullopt});
  }
  // Termination is a separately established capability. Neither length nor
  // initialized bytes alone supply the zero at the logical endpoint.
  for (auto &[data, fact] : state.safety->buffers.values) {
    if (!fact.initialized || fact.shape.terminated ||
        fact.shape.pointerElements || fact.shape.elementBytes != 1 ||
        !bufferFact(data, state))
      continue;
    const auto len = core::Affine::ofPlace(fact.length);
    const auto end = core::Affine::ofPlace(fact.length, 1, 1);
    if (!checkedAtMost(end, core::Affine::ofPlace(fact.capacity), state))
      continue;
    const auto memory = checkedMemoryAt(data, {}, end, state);
    if (!memory || !checkedInitialized(*memory, state))
      continue;
    if (const auto witness = checkedWitness(*memory, state);
        witness && checkedAtMost(witness->zero, len, state) &&
        checkedAtMost(len, witness->zero, state))
      fact.shape.terminated = true;
  }
  materializeBuffers(state);
}

void FunctionDataflow::invalidateBufferWrite(const Expr &written,
                                             core::AnalysisState &state) {
  if (state.safety->buffers.values.empty() &&
      state.safety->buffers.storage.empty() &&
      state.safety->buffers.pending.empty())
    return;
  const auto ref = builder.resolve(written);
  if (!ref) {
    state.safety->buffers.values.clear();
    state.safety->buffers.pending.clear();
    state.safety->buffers.sequences.clear();
    state.safety->buffers.pendingSequences.clear();
    state.safety->buffers.storage.clear();
    return;
  }
  const auto mirrors = definiteMirrors(ref->place, state);
  for (auto &[data, posts] : state.safety->buffers.pending)
    std::erase_if(posts, [&](const auto &post) {
      if (places.isDescendantOf(ref->place, places.deref(data)))
        return true;
      return std::ranges::any_of(
          std::array{data, post.fact.object, post.fact.length,
                     post.fact.capacity},
          [&](const auto place) {
            return place == ref->place ||
                   places.isDescendantOf(place, ref->place) ||
                   std::ranges::find(mirrors, place) != mirrors.end();
          });
    });
  std::vector<core::PlaceId> retire;
  for (const auto &[data, fact] : state.safety->buffers.values) {
    if (fact.shape.ownsElements &&
        foldAffine(core::Affine::ofPlace(fact.length), state) !=
            core::Affine::ofConstant(0)) {
      bool touches = places.isDescendantOf(ref->place, places.deref(data));
      for (const auto place : {data, fact.length, fact.capacity, fact.object})
        touches |= place == ref->place ||
                   places.isDescendantOf(place, ref->place) ||
                   std::ranges::find(mirrors, place) != mirrors.end();
      if (touches)
        safetyObligation(core::SafetyProperty::Resource,
                         core::SafetyOutcome::Unresolved, written,
                         "buffer ownership",
                         "owned buffer elements require complete release or "
                         "transfer before container mutation");
    }
    for (const auto place : {data, fact.length, fact.capacity, fact.object})
      if (place == ref->place || places.isDescendantOf(place, ref->place) ||
          std::ranges::find(mirrors, place) != mirrors.end())
        retire.push_back(data);
  }
  std::erase_if(state.safety->buffers.storage, [&](const auto &entry) {
    const auto &[data, fact] = entry;
    return std::ranges::any_of(
        std::array{data, fact.capacity, fact.object}, [&](const auto place) {
          return place == ref->place ||
                 places.isDescendantOf(place, ref->place) ||
                 std::ranges::find(mirrors, place) != mirrors.end();
        });
  });
  for (const auto data : retire)
    state.safety->buffers.values.erase(data);
}

void FunctionDataflow::bufferElementWrite(const BinaryOperator &assignment,
                                          core::AnalysisState &state) {
  const auto *element =
      dyn_cast<ArraySubscriptExpr>(assignment.getLHS()->IgnoreParenImpCasts());
  if (!element)
    return;
  const auto base = builder.resolve(*element->getBase());
  if (!base) {
    state.safety->buffers.sequences.clear();
    return;
  }
  if (auto fact = state.safety->buffers.values.find(base->place);
      fact != state.safety->buffers.values.end())
    fact->second.shape.ownsElements = false;
  state.safety->buffers.pendingSequences.erase(base->place);
  const auto found = state.safety->buffers.sequences.find(base->place);
  if (found == state.safety->buffers.sequences.end())
    return;
  const auto index = foldAffine(builder.affineOf(*element->getIdx()), state);
  const auto endpoint = core::Affine::ofPlace(found->second.length);
  const auto value = element->getType()->isPointerType()
                         ? builder.resolvePointerValue(*assignment.getRHS())
                         : std::nullopt;
  if (!value || !index || found->second.appended ||
      !checkedAtMost(*index, endpoint, state) ||
      !checkedAtMost(endpoint, *index, state)) {
    state.safety->buffers.sequences.erase(found);
    return;
  }
  found->second.appended = value->place;
}

void FunctionDataflow::materializeBufferSequences(core::AnalysisState &state) {
  std::vector<std::pair<core::PlaceId, core::BufferSequencePost>> ready;
  for (auto &[data, entries] : state.safety->buffers.pendingSequences)
    std::erase_if(entries, [&](const auto &post) {
      auto guard = post.when;
      if (!pruneGuard(guard, state))
        return true;
      if (!guard.trivial())
        return false;
      ready.emplace_back(data, post);
      return true;
    });
  for (const auto &[data, post] : ready) {
    if (!bufferFact(data, state))
      continue;
    if (post.sequence)
      state.safety->buffers.sequences[data] = *post.sequence;
    else
      state.safety->buffers.sequences.erase(data);
    auto &fact = state.safety->buffers.values.at(data);
    fact.shape.ownsElements |=
        post.ownsPrefix && (!post.appended || post.ownsValue);
    if (!post.appended)
      continue;
    const auto site = bufferSequenceCalls.find(post.event);
    if (site == bufferSequenceCalls.end())
      continue;
    const auto index = foldAffine(post.index, state);
    if (index.isConstant() && index.constant >= 0) {
      const auto cell =
          places.element(places.deref(data),
                         core::ArrayIndex::constant(index.constant).toString());
      ValueOrigin origin;
      if (post.borrowed || post.value) {
        origin.kind =
            post.borrowed ? ValueOrigin::Kind::Borrow : ValueOrigin::Kind::Copy;
        origin.place = PlaceRef{.place = post.borrowed.value_or(
                                    post.value.value_or(core::PlaceId{})),
                                .derefs = {},
                                .element = {}};
      } else if (post.nullValue) {
        origin.kind = ValueOrigin::Kind::Null;
      }
      if (origin.kind != ValueOrigin::Kind::Opaque)
        applyPointerAssign(cell, origin, *site->second, false, state);
    }
    if (post.ownsPrefix && post.ownsValue && post.value) {
      state.resources.escape(*post.value);
      for (const auto alias : state.aliases.members(*post.value))
        state.resources.escape(alias);
    }
  }
}
} // namespace weavec::analysis
