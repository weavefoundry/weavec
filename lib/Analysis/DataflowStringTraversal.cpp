//===- DataflowStringTraversal.cpp - Terminated scans (RFC 0021) --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"
#include "IntegerSupport.h"

#include <utility>

using namespace clang;

namespace weavec::analysis {

void FunctionDataflow::checkedStringLength(const CallExpr &call,
                                           core::AnalysisState &state) {
  if (call.getNumArgs() != 1 || state.safety->havoc)
    return;
  const auto result = builder.legacyAffineOf(call);
  if (!result || !result->place)
    return;
  // A later call may observe changed bytes. Saved results keep their old
  // value, while this call's result cannot reuse a previous first-zero proof.
  snapshotIntegerDependencies(*result->place, &call, state);
  snapshotScalar(*result->place, &call, state);
  state.dropGuardsOn(*result->place);
  state.numericValues.erase(*result->place);
  if (stringLengthOf(*call.getArg(0), state))
    return;
  auto memory = checkedMemory(*call.getArg(0), {}, {}, state);
  const auto type = integerTypeOf(call.getType(), context);
  if (!memory || !memory->extent || !result || !result->place || !type)
    return;
  std::optional<core::Affine> upper;
  if (const auto witness = checkedWitness(*memory, state))
    upper = witness->zero.shifted(1);
  if (!upper)
    if (const auto found =
            state.safety->boundedTermination.find(memory->storage);
        found != state.safety->boundedTermination.end())
      for (const auto &fact : found->second) {
        auto when = fact.when;
        if (pruneGuard(when, state) && when.trivial() &&
            checkedAtMost(fact.begin, memory->begin, state) &&
            checkedAtMost(memory->begin, fact.begin, state)) {
          upper = fact.end;
          break;
        }
      }
  bool required = false;
  if (!upper && !state.safety->writtenStorage.contains(memory->storage)) {
    // A buffer's capacity nominates an explicit bounded-string premise; it
    // supplies no initialized contents or zero byte by itself (RFC 0029).
    for (const auto &[data, buffer] : state.safety->buffers.values) {
      if (buffer.shape.reader || buffer.shape.pointerElements ||
          buffer.shape.elementBytes != 1 || !buffer.entryBacking ||
          memory->storage != places.deref(*buffer.entryBacking) ||
          state.safety->replacedPointers.contains(data))
        continue;
      auto input = *memory;
      input.input = stableSummaryPathOf(*buffer.entryBacking);
      input.end = core::Affine::ofPlace(buffer.capacity);
      if (input.input &&
          checkedRequire(core::CheckedRequirementKind::TerminatedWithin, input,
                         call, state)) {
        upper = input.end;
        required = true;
        break;
      }
    }
  }
  if (!upper || !checkedValid(*memory, state) ||
      (!required &&
       !checkedInterval(memory->begin, *upper, *memory->extent, state)))
    return;
  const auto begin = checkedByteExpression(memory->begin, state);
  if (!begin)
    return;
  auto first = checkedFirstZeros.find(&call);
  if (first == checkedFirstZeros.end()) {
    if (checkedFirstZeros.size() >= core::MaxTraversalVariables)
      return;
    first = checkedFirstZeros.emplace(&call, places.create("first zero")).first;
  }
  const auto zero = first->second;
  if (begin->dependsOn(zero) || upper->place == zero)
    return;
  snapshotIntegerDependencies(zero, &call, state);
  snapshotScalar(zero, &call, state);
  state.dropGuardsOn(zero);
  state.relations.forget(zero);
  state.numericValues.erase(zero);
  state.scalars.set(zero,
                    core::ValueFact::ofInteger(core::IntegerRange::between(
                        core::IntegerValue::ofBits(*type, 0),
                        core::IntegerValue::ofBits(*type, type->mask() - 1))));
  const auto bound = [&](const core::Affine &value, bool lower) {
    const auto folded = foldAffine(value, state);
    if (folded.isConstant()) {
      if (lower)
        state.relations.learnAtLeast(zero, folded.constant);
      else if (folded.constant != INT64_MIN)
        state.relations.learnAtMost(zero, folded.constant - 1);
    } else if (folded.scale == 1) {
      state.relations.learn(
          zero, lower ? core::Relation::GreaterEqual : core::Relation::Less,
          *folded.place, folded.constant);
    }
  };
  bound(memory->begin, true);
  bound(*upper, false);
  const auto start = begin->converted(*type);
  const auto length = start ? NumericExpression::operation(
                                  core::IntegerOp::Subtract,
                                  NumericExpression::input(zero, *type), *start)
                            : std::nullopt;
  if (!length)
    return;
  state.numericValues.insert_or_assign(*result->place, *length);
  const auto through = core::Affine::ofPlace(zero, 1, 1);
  state.safety->initialize(memory->storage,
                           {.begin = memory->begin, .end = through});
  state.safety->initialize(
      memory->storage,
      {.begin = core::Affine::ofPlace(zero), .end = through, .zeroed = true});
  safetyObligation(
      core::SafetyProperty::Initialization,
      required ? core::SafetyOutcome::Required : core::SafetyOutcome::Proven,
      call, "bounded string",
      "string length ends at the first initialized zero within its bound");
}

void FunctionDataflow::collectCheckedStrings(const Stmt &stmt) {
  std::vector<const Stmt *> pendingNodes{&stmt};
  for (std::size_t i = 0;
       i < pendingNodes.size() && pendingNodes.size() <= 65536; ++i) {
    const auto *node = pendingNodes[i];
    if (!node)
      continue;
    const Expr *condition = nullptr;
    if (const auto *whileLoop = dyn_cast<WhileStmt>(node))
      condition = whileLoop->getCond();
    else if (const auto *forLoop = dyn_cast<ForStmt>(node))
      condition = forLoop->getCond();
    else if (const auto *doLoop = dyn_cast<DoStmt>(node))
      condition = doLoop->getCond();
    std::vector<const Expr *> reads;
    if (condition)
      reads.push_back(condition);
    for (std::size_t j = 0; j < reads.size() && reads.size() <= 4096; ++j) {
      const auto *read = reads[j] ? reads[j]->IgnoreParenImpCasts() : nullptr;
      if (!read)
        continue;
      const Expr *base = nullptr;
      if (const auto *index = dyn_cast<ArraySubscriptExpr>(read);
          index && index->getType()->isCharType())
        base = index->getBase();
      if (const auto *deref = dyn_cast<UnaryOperator>(read);
          deref && deref->getOpcode() == UO_Deref &&
          deref->getType()->isCharType())
        base = deref->getSubExpr();
      if (base)
        if (const auto ref = builder.resolvePointerValue(*base))
          if (const auto path = builder.summaryPathOf(ref->place);
              path && (path->isParam() || path->isGlobal()))
            checkedStringInputs.insert(ref->place);
      if (const auto *binary = dyn_cast<BinaryOperator>(read)) {
        if (binary->isLogicalOp()) {
          reads.push_back(binary->getLHS());
          reads.push_back(binary->getRHS());
        } else if (binary->isEqualityOp()) {
          if (integerConstant(*binary->getLHS(), context) == 0)
            reads.push_back(binary->getRHS());
          if (integerConstant(*binary->getRHS(), context) == 0)
            reads.push_back(binary->getLHS());
        }
      }
      if (const auto *negation = dyn_cast<UnaryOperator>(read);
          negation && negation->getOpcode() == UO_LNot)
        reads.push_back(negation->getSubExpr());
    }
    for (const auto *child : node->children())
      pendingNodes.push_back(child);
  }
  if (checkedStringInputs.size() > core::MaxTraversalVariables) {
    inferred.checked.limited = true;
    inferred.incomplete.insert("traversal variable limit reached");
    checkedStringInputs.clear();
  }
}

void FunctionDataflow::initializeCheckedStrings(core::AnalysisState &state) {
  const auto type = integerTypeOf(context.getSizeType(), context);
  if (!type)
    return;
  for (const auto input : checkedStringInputs) {
    const auto memory = checkedMemoryAt(input, {}, {}, state);
    if (!memory || !memory->input ||
        foldAffine(memory->begin, state) != core::Affine::ofConstant(0))
      continue;
    const auto zero = places.create("terminator(entry " + nameOf(input) + ")");
    checkedTerminatorInputs[zero] = input;
    checkedWitnessMinimum[zero] = 0;
    state.scalars.set(
        zero, core::ValueFact::ofInteger(core::IntegerRange::between(
                  core::IntegerValue::ofBits(*type, 0),
                  core::IntegerValue::ofBits(*type, type->mask() - 1))));
    state.safety->termination[memory->storage].push_back(
        {.begin = {},
         .zero = core::Affine::ofPlace(zero),
         .input = input,
         .when = {}});
    const auto extent = core::Affine::ofPlace(zero, 1, 1);
    state.safety->accessible[memory->storage] = extent;
    if (const auto position = state.safety->positions.find(input);
        position != state.safety->positions.end())
      position->second.extent = extent;
  }
}

std::optional<core::TerminationWitness>
FunctionDataflow::checkedWitness(const CheckedMemory &memory,
                                 const core::AnalysisState &state) {
  if (const auto found = state.safety->termination.find(memory.storage);
      found != state.safety->termination.end())
    for (auto witness : found->second) {
      if (pruneGuard(witness.when, state) && witness.when.trivial() &&
          checkedAtMost(witness.begin, memory.begin, state) &&
          checkedAtMost(memory.begin, witness.zero, state))
        return witness;
    }
  // Existing exact string facts and explicit zero stores can introduce a
  // witness, but only after the entire prefix is proved initialized.
  std::vector<core::Affine> candidates;
  if (memory.holder)
    if (const auto spatial = spatialRecordAt(*memory.holder, state);
        spatial && spatial->offset.isZero() && spatial->string &&
        !spatial->string->unterminated && spatial->string->length) {
      const auto origin = checkedMemoryAt(*memory.holder, {}, {}, state);
      if (origin && origin->storage == memory.storage &&
          checkedAtMost({}, origin->begin, state) &&
          checkedAtMost(origin->begin, {}, state))
        candidates.push_back(*spatial->string->length);
    }
  if (memory.pointer)
    if (const auto length = stringLengthOf(*memory.pointer, state)) {
      const auto base = checkedMemory(*memory.pointer, {}, {}, state);
      const auto zero = base && base->storage == memory.storage
                            ? sumOf(base->begin, *length)
                            : std::nullopt;
      if (zero)
        candidates.push_back(*zero);
    }
  if (const auto found = state.safety->memory.find(memory.storage);
      found != state.safety->memory.end())
    for (const auto &range : found->second) {
      auto when = range.when;
      if (!range.zeroed || range.source || !pruneGuard(when, state) ||
          !when.trivial())
        continue;
      auto point = range.begin;
      if (checkedAtMost(point, memory.begin, state))
        point = memory.begin;
      const auto end = point.shifted(1);
      if (end && checkedInterval(point, *end, range.end, state))
        candidates.push_back(point);
    }
  for (const auto &point : candidates) {
    const auto through = point.shifted(1);
    if (!through || !memory.extent ||
        !checkedAtMost(memory.begin, point, state) ||
        !checkedInterval(memory.begin, *through, *memory.extent, state))
      continue;
    auto prefix = memory;
    prefix.end = *through;
    // checkedInitialized consults the stored witnesses only, so this does
    // not use the candidate being verified to prove itself.
    if (checkedInitialized(prefix, state)) {
      auto begin = foldAffine(memory.begin, state);
      prefix.begin = {};
      if (checkedInitialized(prefix, state))
        begin = {};
      return core::TerminationWitness{
          .begin = begin, .zero = point, .input = {}, .when = {}};
    }
  }
  return std::nullopt;
}

void FunctionDataflow::prepareCheckedStringInputs(
    const CallExpr &call, const core::CheckedContract &contract,
    core::AnalysisState &state) {
  const auto type = integerTypeOf(context.getSizeType(), context);
  if (!type || state.safety->havoc)
    return;
  for (const auto &requirement : contract.requirements) {
    if (requirement.kind != core::CheckedRequirementKind::Terminated)
      continue;
    const auto memory =
        checkedPathMemory(requirement.path, call, {}, {}, state);
    if (!memory || !memory->input || !memory->inputPlace)
      continue;
    if (!state.safety->termination.contains(memory->storage)) {
      // A newly required witness describes entry bytes only while no write
      // has changed them. Never recreate an invalidated entry terminator.
      if (state.safety->writtenStorage.contains(memory->storage))
        continue;
      const auto offset = foldAffine(memory->begin, state);
      if (offset != core::Affine::ofConstant(0))
        continue;
      const auto input = *memory->inputPlace;
      auto existing =
          std::ranges::find_if(checkedTerminatorInputs, [&](const auto &entry) {
            return entry.second == input;
          });
      const auto zero =
          existing == checkedTerminatorInputs.end()
              ? places.create("terminator(entry " + nameOf(input) + ")")
              : existing->first;
      checkedTerminatorInputs[zero] = input;
      checkedWitnessMinimum.try_emplace(zero, 0);
      state.scalars.set(
          zero, core::ValueFact::ofInteger(core::IntegerRange::between(
                    core::IntegerValue::ofBits(*type, 0),
                    core::IntegerValue::ofBits(*type, type->mask() - 1))));
      state.safety->termination[memory->storage].push_back(
          {.begin = {},
           .zero = core::Affine::ofPlace(zero),
           .input = input,
           .when = {}});
      state.safety->accessible[memory->storage] =
          core::Affine::ofPlace(zero, 1, 1);
    }
    checkedStringUse(*memory, call, state);
  }
}

void FunctionDataflow::checkedStringBound(const CheckedMemory &memory,
                                          const Stmt &at,
                                          core::AnalysisState &state) {
  const auto found = state.safety->termination.find(memory.storage);
  if (found == state.safety->termination.end())
    return;
  for (const auto &witness : found->second) {
    if (!witness.input || !witness.zero.place || !witness.when.trivial() ||
        checkedAtMost(memory.begin, witness.zero, state))
      continue;
    const auto point = foldAffine(memory.begin, state);
    if (!point.isConstant() || point.constant < 0 ||
        std::cmp_greater(point.constant, core::MaxTraversalIterations))
      continue;
    // A fixed initial skip needs a witness beyond the skipped prefix. This
    // is a sufficient entry requirement, never a claim about a local buffer.
    state.relations.learnAtLeast(*witness.zero.place, point.constant);
    auto &minimum = checkedWitnessMinimum[*witness.zero.place];
    minimum = std::max(minimum, point.constant);
    checkedStringUse(memory, at, state);
  }
}

void FunctionDataflow::checkedStringUse(const CheckedMemory &memory,
                                        const Stmt &at,
                                        core::AnalysisState &state) {
  const auto found = state.safety->termination.find(memory.storage);
  if (found == state.safety->termination.end())
    return;
  for (const auto &witness : found->second) {
    if (!witness.input || !witness.zero.place)
      continue;
    auto when = witness.when;
    if (!pruneGuard(when, state) || !when.trivial())
      continue;
    const auto path = builder.summaryPathOf(*witness.input);
    if (!path)
      continue;
    if (const auto through = witness.zero.shifted(1))
      state.safety->initialize(memory.storage,
                               {.begin = witness.begin, .end = *through});
    if (recording())
      inferred.checked.require(
          {.kind = core::CheckedRequirementKind::Terminated,
           .path = *path,
           .other = {},
           .begin = core::PathAffine::ofConstant(
               checkedWitnessMinimum[*witness.zero.place]),
           .end = {},
           .family = {},
           .when = summaryGuardOf(guardHere(state))});
    safetyObligation(core::SafetyProperty::Initialization,
                     core::SafetyOutcome::Required, at, "terminated prefix",
                     "traversal requires an initialized terminated prefix");
  }
}

void FunctionDataflow::checkedStringCondition(const Expr &expr,
                                              BinaryOperatorKind op,
                                              const Expr *other, bool holds,
                                              core::AnalysisState &state) {
  const auto *value = expr.IgnoreParenImpCasts();
  if (!value->getType()->isCharType() ||
      value->getType().isVolatileQualified() ||
      value->getType()->isAtomicType() || !PlaceBuilder::isPlaceExpr(*value))
    return;
  const auto memory = checkedLvalue(*value, state);
  if (!memory || !checkedValid(*memory, state))
    return;
  const auto byteType = integerTypeOf(value->getType(), context);
  const auto comparedType = integerTypeOf(expr.getType(), context);
  const auto operation = integerOpOf(op);
  const auto comparedValue =
      other ? integerRangeOf(*other, state)
            : std::optional(core::IntegerRangeEvaluation{
                  .values =
                      core::IntegerRange::singleton(core::IntegerValue::ofBits(
                          comparedType.value_or(core::BooleanType), 0))});
  const auto contents = checkedByteContents(*memory, state);
  if (contents && byteType && comparedType && operation && comparedValue &&
      !comparedValue->mayBeInvalid && comparedValue->values.constant()) {
    std::optional<std::int64_t> first;
    std::optional<std::int64_t> last;
    for (std::size_t i = 0; i < contents->second.size(); ++i) {
      const auto byte =
          core::IntegerValue::ofBits(
              *byteType, static_cast<unsigned char>(contents->second[i]))
              .converted(*comparedType);
      const auto test = core::evaluateInteger(
          *operation, byte, *comparedValue->values.constant());
      if (!test.value)
        return;
      if ((test.value->bits != 0) != holds)
        continue;
      const auto index = contents->first + static_cast<std::int64_t>(i);
      if (!first)
        first = index;
      last = index;
    }
    if (!first) {
      edgeInfeasible = true;
      return;
    }
    if (memory->begin.place && memory->begin.scale == 1) {
      std::int64_t lower = 0;
      std::int64_t upper = 0;
      if (!__builtin_sub_overflow(*first, memory->begin.constant, &lower) &&
          !__builtin_sub_overflow(*last, memory->begin.constant, &upper)) {
        if (*first > contents->first)
          state.relations.learnAtLeast(*memory->begin.place, lower);
        if (*last < contents->first +
                        static_cast<std::int64_t>(contents->second.size()) - 1)
          state.relations.learnAtMost(*memory->begin.place, upper);
      }
    }
  }
  const auto witness = checkedWitness(*memory, state);
  if (!witness)
    return;
  bool nonzero = other == nullptr && holds;
  if (other)
    if (const auto constant = integerConstant(*other, context)) {
      if ((op == BO_EQ && holds) || (op == BO_NE && !holds))
        nonzero = *constant != 0;
      if ((op == BO_NE && holds) || (op == BO_EQ && !holds))
        nonzero = *constant == 0;
    }
  auto &stored = state.safety->termination[memory->storage];
  if (std::ranges::find(stored, *witness) == stored.end() &&
      stored.size() < core::MaxInitializedRanges)
    stored.push_back(*witness);
  if (!nonzero || !checkedInitialized(*memory, state))
    return;
  const auto coordinate = memory->begin;
  const auto zero = witness->zero;
  if (checkedAtMost(zero, coordinate, state)) {
    // checkedWitness already proved coordinate <= zero. Reading a nonzero
    // byte at that same current zero is impossible, not a negative offset
    // that should flow into loop widening (RFC 0029).
    edgeInfeasible = true;
    return;
  }
  if (coordinate.place && zero.place && coordinate.scale == 1 &&
      zero.scale == 1) {
    std::int64_t shift = 0;
    if (!__builtin_sub_overflow(zero.constant, coordinate.constant, &shift))
      state.relations.learn(*coordinate.place, core::Relation::Less,
                            *zero.place, shift);
  } else if (zero.place && zero.scale == 1 && coordinate.isConstant()) {
    std::int64_t lower = 0;
    if (!__builtin_sub_overflow(coordinate.constant, zero.constant, &lower) &&
        !__builtin_add_overflow(lower, std::int64_t{1}, &lower))
      state.relations.learnAtLeast(*zero.place, lower);
  } else if (coordinate.place && coordinate.scale == 1 && zero.isConstant()) {
    std::int64_t upper = 0;
    if (!__builtin_sub_overflow(zero.constant, coordinate.constant, &upper) &&
        !__builtin_sub_overflow(upper, std::int64_t{1}, &upper))
      state.relations.learnAtMost(*coordinate.place, upper);
  }
}

void FunctionDataflow::checkedStringWrite(
    const std::optional<CheckedMemory> &memory, bool zeroed,
    core::AnalysisState &state, bool numericText) {
  for (auto &[data, fact] : state.safety->buffers.values) {
    (void)data;
    fact.shape.terminated = false;
  }
  for (auto &[data, posts] : state.safety->buffers.pending) {
    (void)data;
    for (auto &post : posts)
      if (!memory || !isLocalStorage(memory->storage) ||
          !post.fact.shape.ownsBacking)
        post.fact.shape.terminated = false;
  }
  auto witnesses = std::move(state.safety->termination);
  // RFC 0029: preserve only proved frames of zero-initialized storage. A
  // concrete field store must not erase calloc's untouched sibling fields.
  std::vector<std::pair<core::PlaceId, core::InitializedRange>> zeros;
  if (memory) {
    const auto concrete = [&](core::PlaceId storage) {
      return isLocalStorage(storage) ||
             std::ranges::any_of(checkedObjects, [&](const auto &entry) {
               return entry.second == storage;
             });
    };
    const auto *writtenVariable =
        builder.varForPlace(places.root(memory->storage));
    const bool privateScalar =
        isLocalStorage(memory->storage) && writtenVariable != nullptr &&
        writtenVariable->hasLocalStorage() &&
        writtenVariable->getType()->isScalarType() &&
        !addressTaken.contains(writtenVariable->getCanonicalDecl());
    const auto writtenInput = checkedSeparationInput(*memory, state);
    for (const auto &[storage, ranges] : state.safety->memory) {
      std::optional<core::SummaryPath> preservedInput;
      bool liveEntry = false;
      if (storage != memory->storage &&
          places.step(storage) == core::PathStep::Deref)
        if (const auto holder = places.parent(storage))
          if (const auto input = checkedMemoryAt(*holder, {}, {}, state);
              input && input->storage == storage) {
            preservedInput = checkedSeparationInput(*input, state);
            liveEntry =
                preservedInput && input->inputPlace &&
                storage == places.deref(*input->inputPlace) &&
                !state.safety->replacedPointers.contains(*input->inputPlace) &&
                checkedValid(*input, state);
          }
      for (const auto &range : ranges) {
        if ((!range.zeroed && !range.numericText && range.bytes.empty()) ||
            range.source)
          continue;
        if (storage == memory->storage) {
          if (numericText && (range.numericText || range.zeroed)) {
            auto content = range;
            content.zeroed = false;
            content.numericText = true;
            content.bytes.clear();
            content.immutableBytes = false;
            zeros.emplace_back(storage, std::move(content));
          }
          if (checkedAtMost(memory->end, range.begin, state) ||
              checkedAtMost(range.end, memory->begin, state)) {
            zeros.emplace_back(storage, range);
          } else {
            for (auto part :
                 range.outsideWrite(foldAffine(memory->begin, state),
                                    foldAffine(memory->end, state)))
              zeros.emplace_back(storage, std::move(part));
          }
        } else if (range.immutableBytes || privateScalar ||
                   (liveEntry && concrete(memory->storage) &&
                    checkedValid(*memory, state)) ||
                   (concrete(storage) && concrete(memory->storage) &&
                    places.root(storage) != places.root(memory->storage))) {
          zeros.emplace_back(storage, range);
        } else if (writtenInput && preservedInput &&
                   writtenInput != preservedInput) {
          // Conditional proof, not separation inferred from different names:
          // each caller must establish that the unchanged entry objects are
          // disjoint before this retained zero can be used.
          if (recording())
            inferred.checked.require(
                {.kind = core::CheckedRequirementKind::Separated,
                 .path = *writtenInput,
                 .other = *preservedInput,
                 .family = {}});
          zeros.emplace_back(storage, range);
        }
      }
    }
  }
  state.forgetZeroedMemory();
  for (auto &[storage, range] : zeros)
    state.safety->initialize(storage, std::move(range));
  if (!memory) {
    for (const auto &[storage, entries] : witnesses) {
      (void)entries;
      state.safety->writtenStorage.insert(storage);
    }
    return;
  }
  state.safety->writtenStorage.insert(memory->storage);
  for (auto &[storage, entries] : witnesses) {
    std::erase_if(entries, [&](const auto &witness) {
      if (storage == memory->storage) {
        const auto through = witness.zero.shifted(1);
        const bool before = checkedAtMost(memory->end, witness.zero, state);
        const bool after =
            through && checkedAtMost(*through, memory->begin, state);
        // A complete zero store preserves a zero it may overlap, even when
        // separate before/overlap alternatives cannot be decided statically.
        const bool replaces = zeroed;
        return !before && !after && !replaces;
      }
      // An input cannot designate a local object created in this frame.
      // Merely different indirect place names do not establish separation.
      if ((isLocalStorage(memory->storage) && witness.input) ||
          (isLocalStorage(storage) && memory->input) ||
          (isLocalStorage(storage) && isLocalStorage(memory->storage) &&
           places.root(storage) != places.root(memory->storage)))
        return false;
      if (hasBufferPositions && isLocalStorage(memory->storage)) {
        // A live allocation base cannot overlap an automatic variable. Buffer
        // entry backing likewise predates this frame; a fresh replacement
        // retains that separation without reusing the old pointer's validity.
        const bool bufferBacking = std::ranges::any_of(
            state.safety->buffers.values, [&](const auto &entry) {
              const auto &[data, fact] = entry;
              const auto backing = state.safety->objects.contains(data)
                                       ? state.safety->objects.at(data)
                                       : places.deref(data);
              return backing == storage &&
                     (fact.shape.ownsBacking || fact.entryBacking);
            });
        const bool allocation =
            std::ranges::any_of(checkedObjects, [&](const auto &entry) {
              return entry.second == storage;
            });
        if (bufferBacking || allocation)
          return false;
      }
      const auto input =
          witness.input ? builder.summaryPathOf(*witness.input) : std::nullopt;
      if (!memory->input || !input) {
        state.safety->writtenStorage.insert(storage);
        return true;
      }
      const auto pair = std::minmax(*memory->input, *input);
      if (memoryContext.separations.contains(pair))
        return false;
      const bool before =
          memoryContext.orders.contains({*memory->input, *input});
      const bool after =
          memoryContext.orders.contains({*input, *memory->input});
      if (before && !checkedAtMost(memory->end, witness.zero, state) &&
          witness.input && witness.zero.place) {
        // A fixed leading copy can require a later entry witness. This is
        // a caller obligation, including when the two cursor entries coincide.
        const auto end = foldAffine(memory->end, state);
        if (end.isConstant() && end.constant >= 0 &&
            std::cmp_less_equal(end.constant, core::MaxTraversalIterations)) {
          state.relations.learnAtLeast(*witness.zero.place, end.constant);
          auto &minimum = checkedWitnessMinimum[*witness.zero.place];
          minimum = std::max(minimum, end.constant);
          if (recording())
            inferred.checked.require(
                {.kind = core::CheckedRequirementKind::Terminated,
                 .path = *input,
                 .other = {},
                 .begin = core::PathAffine::ofConstant(minimum),
                 .end = {},
                 .family = {},
                 .when = summaryGuardOf(guardHere(state))});
        }
      }
      const auto through = witness.zero.shifted(1);
      if ((before && checkedAtMost(memory->end, witness.zero, state)) ||
          (after && through && checkedAtMost(*through, memory->begin, state)) ||
          zeroed)
        return false;
      // A serialized context may describe shared storage through an alias
      // chain. Do not infer separation merely because this particular pair
      // lacks a direct record. The context's validated path budget bounds
      // this closure; may-alias chains conservatively retire the witness.
      std::set<core::SummaryPath> reachable{pair.first};
      bool changed = true;
      while (changed) {
        changed = false;
        for (const auto &alias : memoryContext.aliases) {
          if (reachable.contains(alias.first))
            changed |= reachable.insert(alias.second).second;
          if (reachable.contains(alias.second))
            changed |= reachable.insert(alias.first).second;
        }
      }
      const bool related = reachable.contains(pair.second);
      if (related || *memory->input == *input) {
        state.safety->writtenStorage.insert(storage);
        return true;
      }
      // A generic traversal may retain input bytes under explicit separation
      // of the referents. Separating their holders is insufficient (RFC 0021).
      if (recording())
        inferred.checked.require(
            {.kind = core::CheckedRequirementKind::Separated,
             .path = pair.first,
             .other = pair.second,
             .begin = {},
             .end = {},
             .family = {}});
      return false;
    });
    if (!entries.empty()) {
      // Preserve the actual witnessed zero as ordinary byte evidence too.
      // An entry edge may know this byte before a loop has materialized the
      // witness; a local cursor update must not erase that common premise.
      for (const auto &witness : entries)
        if (const auto through = witness.zero.shifted(1))
          state.safety->initialize(storage, {.begin = witness.zero,
                                             .end = *through,
                                             .when = witness.when,
                                             .zeroed = true});
      state.safety->termination[storage] = std::move(entries);
    }
  }
}

std::optional<core::Affine>
FunctionDataflow::checkedTerminatorQuantity(const core::PathAffine &value,
                                            const CallExpr &call,
                                            core::AnalysisState &state) {
  if (!value.path || value.expression || value.path->isResult())
    return std::nullopt;
  const auto memory = checkedPathMemory(*value.path, call, {}, {}, state);
  const auto witness = memory ? checkedWitness(*memory, state) : std::nullopt;
  if (!memory || !witness)
    return std::nullopt;
  const auto negative = foldAffine(memory->begin, state).times(-1);
  auto distance = negative ? sumOf(witness->zero, *negative) : std::nullopt;
  if (!distance && checkedAtMost({}, memory->begin, state) &&
      checkedAtMost(memory->begin, witness->zero, state)) {
    const auto zero = checkedByteExpression(witness->zero, state);
    const auto cursor = checkedByteExpression(memory->begin, state);
    const auto expression =
        zero && cursor ? NumericExpression::operation(core::IntegerOp::Subtract,
                                                      *zero, *cursor)
                       : std::nullopt;
    if (expression) {
      const auto [slot, inserted] = expressionPlaces.try_emplace(*expression);
      if (inserted) {
        slot->second = places.create("remaining terminated prefix");
        numericExpressions.emplace(slot->second, *expression);
      }
      distance = core::Affine::ofPlace(slot->second);
    }
  }
  const auto scaled = distance ? distance->times(value.scale) : std::nullopt;
  return scaled ? scaled->shifted(value.constant) : std::nullopt;
}

} // namespace weavec::analysis
