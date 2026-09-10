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
  if (!value->getType()->isCharType() || !PlaceBuilder::isPlaceExpr(*value))
    return;
  const auto memory = checkedLvalue(*value, state);
  if (!memory || !checkedValid(*memory, state))
    return;
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
    core::AnalysisState &state) {
  auto witnesses = std::move(state.safety->termination);
  state.forgetZeroedMemory();
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
