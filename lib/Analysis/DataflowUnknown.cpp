//===- DataflowUnknown.cpp - Code WeaveC cannot see (RFC 0030 §5) ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §5: the sound defaults for code the engine cannot see. A call into
// a function with no body, summary, `LibrarySpec` entry or ownership contract
// (and an `asm` statement) may have released, retained or replaced whatever
// it reaches (§5.1, §5.7): the values it was handed get release records of
// unknown origin, which are never diagnosed and make later uses
// `unresolved(unknown-callee)`, and what they reach is forgotten. A platform
// function without a table entry borrows its arguments (§5.2). A summary
// that may under-approximate its callee adds the same default (§5.5), and a
// summary carries the default a callee applied to caller-visible memory as
// `unknown` effects. Also the Assume sites of `WEAVEC_ASSUME` (§6.2):
// proven, refuted (`contradicted-assumption`) or checked.
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Analysis/SiteCollector.h"
#include "weavec/Core/LibrarySpec.h"

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;

namespace weavec::analysis {

/// Compiler intrinsics (`__builtin_*`, `__sync_*`, ...) are not a checking
/// boundary: they are part of the language, not unknown code.
static bool isCompilerIntrinsic(const FunctionDecl &callee) {
  return callee.getBuiltinID() != 0 && callee.getName().starts_with("__");
}

/// Whether `value`'s type points to a `const` object: the callee cannot
/// write what it reaches through it (§5.1 *non-`const` pointee*).
static bool constPointee(const Expr &value) {
  const QualType type = value.getType();
  return type->isPointerType() && type->getPointeeType().isConstQualified();
}

std::optional<QualType> FunctionDataflow::placeType(core::PlaceId place) const {
  if (const auto *decl = dyn_cast_if_present<ValueDecl>(builder.declFor(place)))
    return decl->getType();
  if (places.isBase(place))
    return std::nullopt;
  const auto parent = places.parent(place);
  if (!parent)
    return std::nullopt;
  const auto type = placeType(*parent);
  if (!type)
    return std::nullopt;
  switch (places.step(place)) {
  case core::PathStep::Deref:
    if ((*type)->isPointerType())
      return (*type)->getPointeeType();
    return std::nullopt;
  case core::PathStep::Index:
    if (const auto *array = (*type)->getAsArrayTypeUnsafe())
      return array->getElementType();
    if ((*type)->isPointerType())
      return (*type)->getPointeeType();
    return std::nullopt;
  case core::PathStep::Field:
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<bool> FunctionDataflow::holdsPointer(core::PlaceId place) const {
  if (const auto type = placeType(place))
    return (*type)->isPointerType();
  return std::nullopt;
}

void FunctionDataflow::markUnknown(core::PlaceId place,
                                   const core::SourceLocation &here,
                                   bool reached, core::AnalysisState &state) {
  markUnknownBelow(place, state);
  if (unknownBatch != nullptr) {
    unknownBatch->marks.emplace_back(place, reached);
    return;
  }
  for (const ConsumeTarget &target :
       consumeTargets(place, core::ElementWitness::whole(), state)) {
    // A pointer the callee could write (reached through its pointee) may
    // have been initialised there: the uninitialised record gives way.
    if (const core::MoveRecord *record = state.moves.find(target.place);
        reached && record != nullptr &&
        record->reason == core::MoveReason::Uninitialized)
      state.moves.reinitialize(target.place);
    state.moves.markUnknown(target.place, here, unknownCode, unknownIsCallback);
  }
}

bool FunctionDataflow::isBelow(core::PlaceId place,
                               core::PlaceId object) const {
  for (auto at = places.parent(place); at; at = places.parent(*at))
    if (*at == object)
      return true;
  return false;
}

void FunctionDataflow::markUnknownBelow(core::PlaceId object,
                                        core::AnalysisState &state) {
  // Whatever this function had established below the object, the call may
  // have replaced or released: it inherits the object's record again.
  std::erase_if(state.established, [this, object](core::PlaceId place) {
    return place == object || isBelow(place, object);
  });
}

void FunctionDataflow::noteEstablished(core::PlaceId place,
                                       core::AnalysisState &state) const {
  // Nothing to shield from while no record of unknown origin stands.
  if (!state.moves.hasUnknownOrigin())
    return;
  const auto at = std::lower_bound(state.established.begin(),
                                   state.established.end(), place);
  if (at == state.established.end() || *at != place)
    state.established.insert(at, place);
}

std::optional<core::MoveRecord>
FunctionDataflow::inheritedUnknown(core::PlaceId place,
                                   const core::AnalysisState &state) const {
  if (!state.moves.hasUnknownOrigin())
    return std::nullopt;
  // The record only ever says that a pointer's object may be gone.
  if (!holdsPointer(place).value_or(true))
    return std::nullopt;
  const auto established = [&state](core::PlaceId at) {
    return std::binary_search(state.established.begin(),
                              state.established.end(), at);
  };
  // Upwards through the place tree, and sideways to the other names of the
  // same cell: `m = o->m; consume(o); m->in` reaches `o`'s record through
  // `m ~ o->m`. Bounded, and only walked while a record of unknown origin
  // stands.
  constexpr std::size_t MaxInheritSteps = 32;
  llvm::SmallVector<core::PlaceId, 8> queue{place};
  llvm::SmallDenseSet<std::uint32_t, 16> seen;
  seen.insert(place.value);
  for (std::size_t i = 0; i < queue.size() && i < MaxInheritSteps; ++i) {
    const core::PlaceId at = queue[i];
    // A place this function gave a value to shields what lies below it.
    if (established(at))
      continue;
    if (const core::MoveRecord *record = state.moves.find(at)) {
      // A record of unknown origin stands for every place below it: unknown
      // code had the pointer and may have released or replaced anything it
      // reaches. A record this function made speaks for its own place only;
      // an access through it is reported there.
      if (!record->unknownOrigin)
        continue;
      core::MoveRecord inherited = *record;
      inherited.via = std::nullopt;
      inherited.element = core::ElementWitness::whole();
      inherited.guard = {};
      inherited.ownValue = false;
      inherited.local = false;
      return inherited;
    }
    for (const auto &[alias, edge] : state.aliases.edgesFrom(at)) {
      if (edge.exact() && edge.offset.isZero() &&
          seen.insert(alias.value).second)
        queue.push_back(alias);
    }
    if (const auto parent = places.parent(at))
      if (seen.insert(parent->value).second)
        queue.push_back(*parent);
  }
  return std::nullopt;
}

void FunctionDataflow::forgetReachableFacts(core::PlaceId object,
                                            core::AnalysisState &state) {
  if (unknownBatch != nullptr) {
    unknownBatch->objects.push_back(object);
    return;
  }
  std::vector<core::PlaceId> reached = places.descendants(object);
  reached.insert(reached.begin(), object);
  forgetFactsOf(std::move(reached), state);
}

void FunctionDataflow::flushUnknown(UnknownBatch &batch,
                                    const core::SourceLocation &here,
                                    core::AnalysisState &state) {
  // Marking changes no alias: the places share their ancestors' mirrors.
  // A place marked twice is marked once, as reached if either said so.
  {
    llvm::DenseMap<std::uint32_t, bool> reachedOf;
    std::vector<core::PlaceId> order;
    for (const auto &[place, reached] : batch.marks) {
      const auto [it, inserted] = reachedOf.try_emplace(place.value, reached);
      if (inserted)
        order.push_back(place);
      else
        it->second = it->second || reached;
    }
    llvm::DenseMap<std::uint32_t, MirrorPlaces> cache;
    mirrorCache = &cache;
    const auto restore = llvm::scope_exit([this] { mirrorCache = nullptr; });
    for (const core::PlaceId place : order)
      markUnknown(place, here, reachedOf.lookup(place.value), state);
  }
  // Then the facts below the outermost objects, in one pass.
  std::ranges::sort(batch.objects);
  const auto [first, last] = std::ranges::unique(batch.objects);
  batch.objects.erase(first, last);
  std::vector<core::PlaceId> reached;
  for (const core::PlaceId object : batch.objects) {
    bool covered = false;
    for (auto parent = places.parent(object); parent && !covered;
         parent = places.parent(*parent))
      covered = std::ranges::binary_search(batch.objects, *parent);
    if (covered)
      continue;
    reached.push_back(object);
    llvm::append_range(reached, places.descendants(object));
  }
  batch = {};
  if (reached.empty())
    return;
  // RFC 0030 §5.1 (the lazy default): a pointer inside one of the objects
  // inherits its record, but a name *outside* the object for the same cell
  // does not, and the forgetting below separates it from the inside name.
  // Those few names are marked themselves.
  {
    std::vector<core::PlaceId> inside = reached;
    std::ranges::sort(inside);
    llvm::DenseMap<std::uint32_t, MirrorPlaces> cache;
    mirrorCache = &cache;
    const auto restore = llvm::scope_exit([this] { mirrorCache = nullptr; });
    for (const core::PlaceId place : inside) {
      if (!holdsPointer(place).value_or(true))
        continue;
      for (const auto &[alias, edge] : state.aliases.edgesFrom(place)) {
        if (!edge.exact() || !edge.offset.isZero() ||
            std::ranges::binary_search(inside, alias))
          continue;
        markUnknown(alias, here, /*reached=*/true, state);
      }
    }
  }
  forgetFactsOf(std::move(reached), state);
}

std::vector<core::PlaceId>
FunctionDataflow::numericNames(const core::AnalysisState &state) const {
  std::vector<core::PlaceId> result;
  const auto inputs = [&result](const NumericExpression &expression) {
    for (const auto &node : expression.all())
      if (node.key)
        result.push_back(*node.key);
  };
  for (const auto &[symbol, expression] : numericExpressions)
    inputs(expression);
  for (const auto &[holder, expression] : state.numericValues)
    inputs(expression);
  for (const auto &predicate : state.numericConditions.integers) {
    inputs(predicate.lhs);
    inputs(predicate.rhs);
  }
  for (const auto &[holder, record] : state.spatial.all()) {
    if (record.extent && record.extent->place)
      result.push_back(*record.extent->place);
    if (record.string && record.string->length && record.string->length->place)
      result.push_back(*record.string->length->place);
  }
  std::ranges::sort(result);
  const auto [first, last] = std::ranges::unique(result);
  result.erase(first, last);
  return result;
}

void FunctionDataflow::forgetFactsOf(std::vector<core::PlaceId> reached,
                                     core::AnalysisState &state) {
  // The snapshots do nothing for a place no numeric fact names; find the
  // named ones once (and again after a snapshot, which renames).
  std::vector<core::PlaceId> named = numericNames(state);
  // RFC 0030 §5.1: the callee writes what it reaches, and a write to a place
  // that holds a loan ends it (`open_func(ls, fs, &bl); statlist(ls);
  // close_func(ls);` undoes `fs->bl` through `ls->fs`, which this call may
  // have replaced). The loan is kept — nothing here saw it end — but it no
  // longer holds on every path, so an escape decided by it is possible,
  // never definite (§3.4). Every name of the cell counts, and the alias
  // edges that give them go below.
  if (!state.loans.loans().empty())
    for (const core::PlaceId place : reached) {
      state.loans.weakenHolder(place);
      for (const core::PlaceId mirror : mirrors(place, state))
        state.loans.weakenHolder(mirror);
    }
  for (const core::PlaceId place : reached) {
    if (std::ranges::binary_search(named, place)) {
      snapshotIntegerDependencies(place, nullptr, state);
      snapshotScalar(place, nullptr, state);
      named = numericNames(state);
    }
    state.numericWrites.insert(place);
    state.relations.forget(place);
    state.nulls.forget(place);
    // What the place held (an allocation at its start, a null) may have been
    // replaced; the escape already covers the leak rule.
    state.resources.forget(place);
    state.scalars.forget(place);
    // The pointers stored there may point elsewhere now, so they are no
    // longer the same value as any other name; the object the argument
    // itself points into keeps its extent (§5.1).
    state.spatial.forget(place);
    state.aliases.separate(place);
    state.definiteAliases.separate(place);
    state.callTargets.erase(place);
  }
  // One scan of the guards for the whole object.
  state.dropGuardsOn(std::move(reached));
}

void FunctionDataflow::applyUnknownToValue(const ValueOrigin &value,
                                           bool readOnly,
                                           const core::SourceLocation &here,
                                           core::AnalysisState &state) {
  switch (value.kind) {
  case ValueOrigin::Kind::Conditional:
    for (const ValueOrigin &alternative : value.alternatives)
      applyUnknownToValue(alternative, readOnly, here, state);
    return;
  case ValueOrigin::Kind::Copy:
    // The argument's place, and every name that holds the same value: the
    // callee may have released or kept the object. A null pointer points to
    // none.
    if (value.place) {
      const auto nullness = nullnessAt(value.place->place, state);
      if (nullness && nullness->state == core::Nullness::Null)
        return;
      markUnknown(value.place->place, here, /*reached=*/false, state);
    }
    break;
  case ValueOrigin::Kind::Borrow:
    break;
  default:
    return;
  }
  // May be retained: no leak is reported for it (RFC 0007, *Escape*).
  escapeValue(value, /*deep=*/true, state);
  // §3.1: the unknown code may have released an object of any type.
  state.noteRelease(core::AnalysisState::AnyType, /*owned=*/false);
  if (readOnly)
    return;
  // Every pointer cached below the pointee may have been released or
  // replaced, and nothing known about that memory holds any more.
  const std::optional<PlaceRef> pointee = builder.pointeeOf(value);
  if (!pointee)
    return;
  // RFC 0030 §5.1 (the lazy default): one record on the object the pointer
  // designates stands for every pointer below it. A place below it inherits
  // the record (`inheritedUnknown`) until this function gives it a value,
  // so the places this function names only *after* the call are covered
  // too, and a state in a large function does not carry hundreds of
  // records. The value's own holders are marked by the caller above.
  markUnknown(pointee->place, here, /*reached=*/true, state);
  forgetReachableFacts(pointee->place, state);
}

void FunctionDataflow::applyUnknownToReachable(const core::SourceLocation &here,
                                               core::AnalysisState &state) {
  state.noteRelease(core::AnalysisState::AnyType, /*owned=*/false);
  // Every escaped place: the callee may have been handed it earlier.
  for (const core::PlaceId holder : state.resources.holders())
    if (state.resources.isEscaped(holder))
      markUnknown(holder, here, /*reached=*/false, state);
  // Every pointer-typed global that external code can reach: one of
  // external linkage directly, and any other through a call back into this
  // unit's functions.
  const auto count = places.size();
  for (std::size_t i = 0; i < count; ++i) {
    const core::PlaceId place{static_cast<std::uint32_t>(i)};
    if (!places.isBase(place))
      continue;
    const auto *var = builder.varForPlace(place);
    if (var == nullptr || !var->hasGlobalStorage() ||
        !var->getType()->isPointerType())
      continue;
    markUnknown(place, here, /*reached=*/false, state);
    if (const auto object = places.child(place, core::PathStep::Deref, {}))
      forgetReachableFacts(*object, state);
  }
}

std::string FunctionDataflow::unquotedCalleeName(const CallExpr &call) {
  std::string name = calleeName(call);
  if (name.size() >= 2 && name.front() == '\'' && name.back() == '\'')
    return name.substr(1, name.size() - 2);
  return name;
}

/// The name of parameter `index` of `callee` for messages.
static std::string parameterName(const FunctionDecl &callee, unsigned index) {
  for (const FunctionDecl *redecl : callee.redecls())
    if (index < redecl->getNumParams() &&
        !redecl->getParamDecl(index)->getName().empty())
      return "'" + redecl->getParamDecl(index)->getNameAsString() + "'";
  return "parameter " + std::to_string(index + 1);
}

void FunctionDataflow::decideUnknownCall(const CallExpr &call,
                                         std::optional<unsigned> uncovered) {
  const SiteInfo *site = siteFor(call, core::Facet::Temporal);
  if (site == nullptr)
    return;
  const FunctionDecl *callee = call.getDirectCallee();
  std::string detail;
  std::optional<core::FixItHint> fixit;
  const SourceManager &sm = context.getSourceManager();
  if (callee == nullptr) {
    detail = "the target of " + calleeName(call) +
             " is unknown; annotate the parameters of its function type";
  } else if (uncovered && *uncovered < callee->getNumParams()) {
    // §5.1: "declare 'consume' with WEAVEC_BORROWED on 'p' if it neither
    // keeps nor frees it".
    const std::string name = callee->getNameAsString();
    detail = "declare '" + name + "' with WEAVEC_BORROWED on " +
             parameterName(*callee, *uncovered) +
             " if it neither keeps nor frees it";
    const ParmVarDecl *param = callee->getFirstDecl()->getParamDecl(*uncovered);
    const SourceLocation at = sm.getFileLoc(param->getLocation());
    if (at.isValid() && !sm.isInSystemHeader(at))
      fixit = core::FixItHint{.location = locate(at),
                              .insertion = "WEAVEC_BORROWED "};
  } else if (callee->getReturnType()->isPointerType()) {
    const std::string name = callee->getNameAsString();
    detail = "declare the result of '" + name +
             "' WEAVEC_OWNED or WEAVEC_BORROWED, or define '" + name +
             "' in this program";
    const SourceLocation at =
        sm.getFileLoc(callee->getFirstDecl()->getLocation());
    if (at.isValid() && !sm.isInSystemHeader(at))
      fixit = core::FixItHint{.location = locate(at),
                              .insertion = "WEAVEC_BORROWED "};
  } else {
    detail = "define '" + callee->getNameAsString() +
             "' in this program, or link a unit that has its WeaveC record";
  }
  decide(site, core::Facet::Temporal,
         core::FacetDecision::unresolvedFor(
             // §9.3: reached through a function pointer.
             callee == nullptr ? core::UnresolvedReason::Callback
                               : core::UnresolvedReason::UnknownCallee,
             std::move(detail)));
  if (fixit && publishing())
    ledger.suggest(*site->stmt, site->kind, site->boundary,
                   core::Facet::Temporal, std::move(*fixit));
}

void FunctionDataflow::handleUncheckedCall(const CallExpr &call,
                                           core::AnalysisState &state) {
  const FunctionDecl *callee = call.getDirectCallee();
  if (callee != nullptr && isCompilerIntrinsic(*callee))
    return;
  // Unknown code may call back into any reachable library entry point. A
  // later initializer or verified output can establish new private state.
  const auto count = places.size();
  for (std::size_t i = 0; i < count; ++i) {
    const core::PlaceId place{static_cast<std::uint32_t>(i)};
    if (!places.isBase(place))
      continue;
    const auto *var = builder.varForPlace(place);
    if (!var || !var->hasGlobalStorage() || !tracksScalar(place))
      continue;
    forgetBelow(place, state);
    forgetScalar(place, state, &call);
    state.callTargets.erase(place);
    state.nulls.forget(place);
    if (recording())
      if (const auto path = builder.summaryPathOf(place))
        inferred.addEffect(*path, core::PlaceEffect{.written = true});
  }
  // Nullness annotations say nothing about ownership, so they do not make
  // the callee known; but what they do say holds (RFC 0008, *Annotation
  // surface*): a `WEAVEC_NONNULL` parameter is a requirement on this call.
  if (callee != nullptr) {
    const SignatureAnnotations annotations = collectAnnotations(*callee);
    if (annotations.anyNullness() || annotations.anySizedBy()) {
      const core::FunctionSummary declared = summaryFromAnnotations(*callee);
      if (annotations.anyNullness())
        checkRequiredArguments(call, declared, state);
      if (annotations.anySizedBy())
        checkRequiredExtents(call, declared, state);
    }
  }

  // RFC 0030 §5.2: a function of the platform's own headers without a
  // table entry borrows its arguments for the call: no release, retain or
  // store effect; what it reaches may have been written.
  if (callee != nullptr && isPlatformDeclaration(*callee, summaries.library(),
                                                 context.getSourceManager())) {
    for (const Expr *arg : call.arguments()) {
      if (!arg->getType()->isPointerType())
        continue;
      const ValueOrigin origin = builder.classifyValue(*arg);
      escapeValue(origin, /*deep=*/true, state);
      forgetNullnessReachable(origin, state);
    }
    decide(siteFor(call, core::Facet::Temporal), core::Facet::Temporal,
           core::FacetDecision::trustedFor(core::TrustReason::SystemApi));
    return;
  }

  // RFC 0030 §5.1: the unknown-callee default, per pointer argument (none
  // has an ownership contract: the callee would have a summary otherwise).
  // The unit's exports list the callees that touch pointers (RFC 0005).
  if (recording() && callInvolvesPointers(call)) {
    if (callee != nullptr)
      (void)summaries.noteUnknownCallee(*callee);
    else
      (void)summaries.noteUnknownIndirect(call);
  }
  const core::SourceLocation here = locate(call);
  unknownCode =
      callee != nullptr ? callee->getNameAsString() : unquotedCalleeName(call);
  // §9.3: an indirect call whose slot has no target is a callback.
  unknownIsCallback = callee == nullptr;
  UnknownBatch batch;
  const bool owner = unknownBatch == nullptr;
  if (owner)
    unknownBatch = &batch;
  const auto flush = llvm::scope_exit([&] {
    if (!owner)
      return;
    unknownBatch = nullptr;
    flushUnknown(batch, here, state);
  });
  std::optional<unsigned> uncovered;
  for (unsigned i = 0; i < call.getNumArgs(); ++i) {
    const Expr &arg = *call.getArg(i);
    if (!arg.getType()->isPointerType())
      continue;
    if (!uncovered)
      uncovered = i;
    applyUnknownToValue(builder.classifyValue(arg), constPointee(arg), here,
                        state);
  }
  if (uncovered)
    applyUnknownToReachable(here, state);
  decideUnknownCall(call, uncovered);
}

bool FunctionDataflow::isExternCallee(const FunctionDecl &callee) const {
  if (callee.getDefinition() != nullptr || summaries.libraryMatch(callee))
    return false;
  if (const ProgramDatabase *database = summaries.programDatabase();
      database != nullptr && callee.getIdentifier() != nullptr &&
      database->defines(callee.getName()))
    return false;
  return !isPlatformDeclaration(callee, summaries.library(),
                                context.getSourceManager());
}

void FunctionDataflow::applyUnknownEffects(const CallExpr &call,
                                           const CallEffects &effects,
                                           core::AnalysisState &state) {
  const core::FunctionSummary &summary = *effects.summary;
  const core::SourceLocation here = locate(call);
  unknownCode = unquotedCalleeName(call);
  unknownIsCallback = false;
  // One call's marks and forgotten objects are applied together.
  UnknownBatch batch;
  const bool owner = unknownBatch == nullptr;
  if (owner)
    unknownBatch = &batch;
  const auto flush = llvm::scope_exit([&] {
    if (!owner)
      return;
    unknownBatch = nullptr;
    flushUnknown(batch, here, state);
  });
  // What the callee handed to code it cannot see (§5.1), as its summary
  // says: the caller's names for those values get the same default.
  PlaceBuilder::PathLookupCache lookups;
  // A parameter whose own value is unknown covers every path below it: the
  // default on the argument reaches all of its pointee.
  const auto rootUnknown = [&summary](const core::SummaryPath &path) {
    if (!path.isParam() || path.isRoot())
      return false;
    const auto root =
        summary.effects.find(core::SummaryPath::param(path.index));
    return root != summary.effects.end() && root->second.unknown;
  };
  for (const auto &[path, effect] : summary.effects) {
    if (!effect.unknown || rootUnknown(path))
      continue;
    if (path.isParam() && path.isRoot()) {
      if (path.index >= call.getNumArgs())
        continue;
      const Expr &arg = *call.getArg(path.index);
      if (arg.getType()->isPointerType())
        applyUnknownToValue(builder.classifyValue(arg), constPointee(arg), here,
                            state);
      continue;
    }
    // A global the caller has not named yet is named now: its later loads
    // must see the record.
    const auto place = path.isGlobal() ? [&]() -> std::optional<core::PlaceId> {
      const auto ref = builder.resolveSummaryPath(path, call);
      return ref ? std::optional(ref->place) : std::nullopt;
    }()
        : builder.lookupSummaryPath(path, call, lookups);
    if (place) {
      markUnknown(*place, here, /*reached=*/true, state);
      if (const auto object = places.child(*place, core::PathStep::Deref, {}))
        forgetReachableFacts(*object, state);
    }
  }

  // §5.1: a callee defined elsewhere applies its ownership contract; its
  // pointer parameters without one get the unknown-callee default.
  const FunctionDecl *callee = call.getDirectCallee();
  std::optional<unsigned> uncovered;
  bool external = false;
  if (effects.source == SummarySource::Annotation) {
    std::vector<AnnotationSet> params;
    if (callee != nullptr && isExternCallee(*callee)) {
      external = true;
      params = collectAnnotations(*callee).params;
    } else if (const auto seen = callTargetsSeen.find(&call);
               callee == nullptr &&
               (seen == callTargetsSeen.end() || seen->second.unknown ||
                seen->second.functions.empty())) {
      external = true;
      if (const Decl *declaration = indirectCalleeDecl(call))
        params = collectFunctionTypeAnnotations(*declaration).params;
    }
    if (external)
      for (unsigned i = 0; i < call.getNumArgs(); ++i) {
        const Expr &arg = *call.getArg(i);
        if (!arg.getType()->isPointerType() ||
            (i < params.size() && params[i].ownership()))
          continue;
        if (!uncovered)
          uncovered = i;
        applyUnknownToValue(builder.classifyValue(arg), constPointee(arg), here,
                            state);
      }
  }

  // §5.5: a summary that may under-approximate its callee (over budget,
  // out of rounds, or with a construct the engine does not model): its
  // known effects, then the unknown-callee default on every argument. A
  // reason about integers or extents leaves the effects complete, and so
  // does a context run the callee fell back from (the default-context
  // summary it used instead is sound) and an indirect call with a target
  // nobody knows (the callee applied §5.1 there, and its `unknown` effects
  // say what it handed on).
  const auto hiding = llvm::find_if(summary.incomplete, [](const auto &text) {
    const llvm::StringRef reason(text);
    return incompleteFacet(reason) == core::Facet::Temporal &&
           !reason.contains("call context") &&
           !reason.contains("callback context") &&
           reason != "indirect call has an unresolved target";
  });
  const bool incomplete = hiding != summary.incomplete.end();
  if (incomplete)
    for (const Expr *arg : call.arguments())
      if (arg->getType()->isPointerType())
        applyUnknownToValue(builder.classifyValue(*arg), constPointee(*arg),
                            here, state);
  if (uncovered || incomplete)
    applyUnknownToReachable(here, state);

  if (uncovered) {
    decideUnknownCall(call, uncovered);
  } else if (incomplete) {
    decide(siteFor(call, core::Facet::Temporal), core::Facet::Temporal,
           core::FacetDecision::unresolvedFor(
               core::incompletenessReason(*hiding),
               "the summary of " + calleeName(call) +
                   " is incomplete: " + *hiding));
  } else if (external) {
    decide(siteFor(call, core::Facet::Temporal), core::Facet::Temporal,
           core::FacetDecision::trustedFor(core::TrustReason::ExternContract));
  }
}

void FunctionDataflow::handleAsm(const GCCAsmStmt &stmt,
                                 core::AnalysisState &state) {
  // RFC 0030 §5.7: the unknown-callee default on its pointer operands
  // (detail "inline assembly"); a "memory" clobber reaches every escaped
  // place and every reachable global as well.
  const core::SourceLocation here = locate(stmt.getBeginLoc());
  unknownCode = "inline assembly";
  unknownIsCallback = false;
  bool any = false;
  for (unsigned i = 0; i < stmt.getNumOutputs(); ++i) {
    const Expr *output = stmt.getOutputExpr(i);
    if (output == nullptr || !output->getType()->isPointerType())
      continue;
    any = true;
    // The operand is written: what it held is gone, and what it holds now
    // came from code nobody can see.
    if (const auto ref = builder.resolve(*output)) {
      forgetReachableFacts(ref->place, state);
      markUnknown(ref->place, here, /*reached=*/true, state);
    }
  }
  for (unsigned i = 0; i < stmt.getNumInputs(); ++i) {
    const Expr *input = stmt.getInputExpr(i);
    if (input == nullptr || !input->getType()->isPointerType())
      continue;
    any = true;
    applyUnknownToValue(builder.classifyValue(*input), constPointee(*input),
                        here, state);
  }
  bool memory = false;
  for (unsigned i = 0; i < stmt.getNumClobbers(); ++i)
    memory = memory || stmt.getClobber(i) == "memory";
  if (memory)
    applyUnknownToReachable(here, state);
  (void)any;
}

// -- Aliases of a released object (RFC 0030 §3.1) ----------------------------

/// The declaration a release's argument loads its value from: a field or a
/// global variable (`free(s->a)`, `free(g_buf)`).
static const Decl *loadedSlot(const Expr &argument) {
  const Expr *e = argument.IgnoreParenCasts();
  if (const auto *member = dyn_cast<MemberExpr>(e))
    return member->getMemberDecl()->getCanonicalDecl();
  if (const auto *ref = dyn_cast<DeclRefExpr>(e))
    if (const auto *var = dyn_cast<VarDecl>(ref->getDecl());
        var != nullptr && var->hasGlobalStorage())
      return var->getCanonicalDecl();
  return nullptr;
}

bool FunctionDataflow::isOwningPlace(core::PlaceId place) {
  if (!summaries.owningSlots) {
    // §9.4: a slot is owning if some function of the unit releases a value
    // loaded from it (flow-insensitive). Stage S7's slot analysis follows
    // wrappers; here the releasing calls are the library's.
    class Releases : public RecursiveASTVisitor<Releases> {
    public:
      llvm::DenseSet<const Decl *> slots;
      explicit Releases(const SummaryStore &store) : summaries(store) {}
      const SummaryStore &summaries;
      bool VisitCallExpr(CallExpr *call) {
        const FunctionDecl *callee = call->getDirectCallee();
        const auto row =
            callee != nullptr ? summaries.libraryMatch(*callee) : std::nullopt;
        if (!row)
          return true;
        for (unsigned i = 0; i < call->getNumArgs(); ++i)
          if (const core::LibraryParam *param = row->param(i);
              param != nullptr &&
              param->effect == core::LibraryParam::Effect::Release)
            if (const Decl *slot = loadedSlot(*call->getArg(i)))
              slots.insert(slot);
        return true;
      }
    } releases(summaries);
    releases.TraverseDecl(context.getTranslationUnitDecl());
    summaries.owningSlots = std::move(releases.slots);
  }
  const NamedDecl *decl = builder.declFor(place);
  if (decl == nullptr)
    return false;
  if (const auto *var = dyn_cast<VarDecl>(decl);
      var != nullptr && !var->hasGlobalStorage())
    return false;
  return summaries.owningSlots->contains(decl->getCanonicalDecl());
}

std::uint64_t FunctionDataflow::pointeeTypeKey(core::PlaceId pointer) const {
  const auto type = placeType(pointer);
  if (!type || !(*type)->isPointerType())
    return core::AnalysisState::AnyType;
  const QualType pointee =
      (*type)->getPointeeType().getCanonicalType().getUnqualifiedType();
  // A character type or `void` may designate any object (C's effective-type
  // rules); so may a type the engine cannot name.
  if (pointee->isVoidType() || pointee->isCharType() ||
      pointee->isIncompleteType())
    return core::AnalysisState::AnyType;
  return reinterpret_cast<std::uintptr_t>(pointee.getTypePtr());
}

void FunctionDataflow::noteRelease(core::PlaceId released,
                                   core::AnalysisState &state) {
  state.noteRelease(pointeeTypeKey(released), isOwningPlace(released));
}

bool FunctionDataflow::mayAliasReleased(core::PlaceId pointer,
                                        const core::AnalysisState &state) {
  if (state.releasedTypes.empty() || state.storedSinceRelease.contains(pointer))
    return false;
  // Only a pointer from outside the function's own values: loaded from a
  // parameter- or global-rooted place, or copied from one.
  const auto outside = [this](core::PlaceId place) {
    const auto *var = builder.varForPlace(places.root(place));
    return var != nullptr && (isa<ParmVarDecl>(var) || var->hasGlobalStorage());
  };
  if (!outside(pointer) &&
      llvm::none_of(state.aliases.edgesFrom(pointer),
                    [&](const auto &edge) { return outside(edge.first); }))
    return false;
  // Place identity: an allocation this function made and still holds is
  // not one it released.
  if (const auto record = state.resources.recordOf(pointer);
      record && record->origin == core::ResourceOrigin::Allocated)
    return false;
  // The effective-type rules separate incompatible non-character types,
  // unless the unit is compiled with `-fno-strict-aliasing`.
  const std::uint64_t type = pointeeTypeKey(pointer);
  if (options.strictAliasing && type != core::AnalysisState::AnyType &&
      !state.releasedTypes.contains(core::AnalysisState::AnyType) &&
      !state.releasedTypes.contains(type))
    return false;
  // Owner uniqueness (A1, A3): two owning places hold distinct objects.
  return state.releasedUnowned || !isOwningPlace(pointer);
}

bool FunctionDataflow::storedSinceRelease(const ValueOrigin &origin,
                                          const core::AnalysisState &state) {
  // A copy is as old as what it copies.
  if (origin.kind == ValueOrigin::Kind::Conditional)
    return llvm::all_of(origin.alternatives, [&](const ValueOrigin &arm) {
      return storedSinceRelease(arm, state);
    });
  if (origin.kind != ValueOrigin::Kind::Copy)
    return true;
  return origin.place && state.storedSinceRelease.contains(origin.place->place);
}

// -- Assumptions (RFC 0030 §6.2) ----------------------------------------------

/// The expression `WEAVEC_ASSUME(e)` states: `e` in `weavec_assume_((e) != 0)`.
static const Expr &assumedExpression(const Expr &argument) {
  const Expr *e = argument.IgnoreParenImpCasts();
  if (const auto *compare = dyn_cast<BinaryOperator>(e);
      compare != nullptr && compare->getOpcode() == BO_NE)
    if (const auto *zero =
            dyn_cast<IntegerLiteral>(compare->getRHS()->IgnoreParenImpCasts());
        zero != nullptr && zero->getValue() == 0)
      return *compare->getLHS()->IgnoreParens();
  return *e;
}

bool FunctionDataflow::handleAssumption(const CallExpr &call,
                                        core::AnalysisState &state) {
  const FunctionDecl *callee = call.getDirectCallee();
  if (callee == nullptr || call.getNumArgs() != 1 ||
      !getAnnotations(*callee).assume)
    return false;
  const Expr &condition = *call.getArg(0);
  const SiteInfo *site = siteFor(call, core::Facet::Assertion);
  // Proven when the facts here contradict `!e`.
  bool proven = false;
  // What the facts say of the variables `e` reads, for a refutation's note.
  std::vector<std::pair<const VarDecl *, std::int64_t>> values;
  if (publishing()) {
    core::AnalysisState negated = state;
    edgeInfeasible = false;
    applyCondition(condition, /*holds=*/false, /*wrapped=*/true, negated);
    proven = edgeInfeasible;
    llvm::SmallVector<const Expr *, 8> operands{&condition};
    while (!operands.empty()) {
      const Expr *e = operands.pop_back_val();
      if (const auto *ref = dyn_cast<DeclRefExpr>(e->IgnoreParenImpCasts()))
        if (const auto *var = dyn_cast<VarDecl>(ref->getDecl());
            var != nullptr && var->getType()->isIntegerType())
          if (const auto fact = scalarFactOf(*ref, state);
              fact && fact->constant &&
              llvm::none_of(values, [var](const auto &entry) {
                return entry.first == var;
              }))
            values.emplace_back(var, *fact->constant);
      for (const Stmt *child : e->children())
        if (const auto *operand = dyn_cast_or_null<Expr>(child))
          operands.push_back(operand);
    }
  }
  // In every case the analysis assumes `e` from here on, as on the true edge
  // of `if (e)`: sound in the enforcing modes, which trap first (or fail the
  // build). An assumption the facts contradict ends the path, as an
  // infeasible edge does.
  edgeInfeasible = false;
  applyCondition(condition, /*holds=*/true, /*wrapped=*/true, state);
  const bool refuted = edgeInfeasible;
  if (refuted)
    blockTerminated = true;
  edgeInfeasible = false;
  if (!publishing())
    return true;
  if (!refuted) {
    decide(site, core::Facet::Assertion,
           proven ? core::FacetDecision::proven()
                  : core::FacetDecision::checked());
    return true;
  }
  decide(site, core::Facet::Assertion, core::FacetDecision::violation());
  const Expr &assumed = assumedExpression(condition);
  const SourceManager &sm = context.getSourceManager();
  const CharSourceRange range = Lexer::makeFileCharRange(
      CharSourceRange::getTokenRange(assumed.getSourceRange()), sm,
      context.getLangOpts());
  std::string text =
      Lexer::getSourceText(range, sm, context.getLangOpts()).str();
  if (text.empty()) {
    llvm::raw_string_ostream os(text);
    assumed.printPretty(os, nullptr, context.getPrintingPolicy());
  }
  core::Diagnostic diagnostic =
      makeError(core::diag::ContradictedAssumption,
                "assumption '" + text + "' is false here", call);
  // The facts that refute it, where the variable got its value.
  for (const auto &[var, value] : values)
    diagnostic.addNote("'" + var->getNameAsString() + "' is " +
                           std::to_string(value) + " here",
                       locate(var->getLocation()));
  report(std::move(diagnostic), core::Certainty::Definite, site,
         core::Facet::Assertion);
  return true;
}

} // namespace weavec::analysis
