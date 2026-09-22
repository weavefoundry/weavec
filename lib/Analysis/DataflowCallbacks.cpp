//===- DataflowCallbacks.cpp - Function pointer value flow ---------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "weavec/Analysis/DataflowEngine.h"
#include "weavec/Analysis/Summaries.h"

using namespace clang;

namespace weavec::analysis {

const core::LibraryMatch *
FunctionDataflow::resolvedLibrary(const CallExpr &call) const {
  const auto source = callSources.find(&call);
  if (source == callSources.end() || source->second != SummarySource::Library)
    return nullptr;
  const auto row = callLibraries.find(&call);
  return row == callLibraries.end() ? nullptr : &row->second;
}

/// RFC 0030 §9.3: the summary store's name for a function the slots name.
/// A slot spells a function with internal linkage `<unit>:<name>`, the store
/// `<unit>#<name>`; a definition in this unit is asked for its own name.
static std::string slotSymbol(const SlotCollection &slots,
                              const std::string &target) {
  if (const clang::FunctionDecl *definition = slots.function(target))
    return callableSymbol(*definition);
  const std::size_t at = target.rfind(':');
  if (at == std::string::npos)
    return target;
  return target.substr(0, at) + "#" + target.substr(at + 1);
}

const core::SlotSolution *FunctionDataflow::solvedSlots() const {
  // §9.3: at link and in `--whole-program` the slots are solved over every
  // unit's constraints and reach the engine through the program database;
  // a per-TU compile has only the unit's own solution. (The §7.3 slot kinds
  // stay the unit's either way: spatial and null outcomes are decided per
  // TU and copied verbatim at link, §1.)
  if (const ProgramDatabase *database = summaries.programDatabase();
      database != nullptr && database->programFacts)
    return &database->programFacts->slots;
  return options.slotSolution;
}

core::CallResolution
FunctionDataflow::slotResolutionOf(const CallExpr &call) const {
  const core::SlotSolution *solution = solvedSlots();
  if (options.slots == nullptr || solution == nullptr)
    return {};
  const auto slot = options.slots->calleeSlot(call);
  if (!slot)
    return {};
  return solution->resolveCall(*slot);
}

std::optional<core::CallTargets>
FunctionDataflow::slotTargetsOf(core::PlaceId place) const {
  const core::SlotSolution *solution = solvedSlots();
  if (options.slots == nullptr || solution == nullptr)
    return std::nullopt;
  const auto *decl = dyn_cast_or_null<ValueDecl>(builder.declFor(place));
  if (decl == nullptr)
    return std::nullopt;
  // Only a slot that holds function pointers has targets; every other
  // global and field would answer "open" and say nothing.
  QualType held = decl->getType();
  while (const clang::ArrayType *array = context.getAsArrayType(held))
    held = array->getElementType();
  if (!held->isFunctionPointerType())
    return std::nullopt;
  const auto slot = options.slots->slotOf(*decl);
  if (!slot)
    return std::nullopt;
  const std::set<std::string> &solved = solution->targets(*slot);
  core::CallTargets result;
  // An open slot can hold a value the solver never saw (§9.3), and so, as
  // far as a caller is concerned, does one with more targets than a
  // `CallTargets` can name.
  result.unknown =
      solution->isOpen(*slot) || solved.size() > core::MaxCallTargets;
  if (solved.size() > core::MaxCallTargets)
    return result;
  for (const std::string &target : solved) {
    if (target == core::UnknownFunction)
      result.unknown = true;
    else
      result.functions.insert(slotSymbol(*options.slots, target));
  }
  return result;
}

core::CallTargets
FunctionDataflow::originTargets(const ValueOrigin &origin,
                                const core::AnalysisState &state) {
  if (!origin.targets.empty())
    return origin.targets;
  if (origin.kind == ValueOrigin::Kind::Conditional) {
    bool hasFunction = false;
    for (const auto &arm : origin.alternatives)
      hasFunction |= !originTargets(arm, state).empty();
    if (!hasFunction)
      return {};
    core::CallTargets result;
    for (const auto &arm : origin.alternatives) {
      auto targets = originTargets(arm, state);
      if (targets.empty()) {
        if (arm.kind == ValueOrigin::Kind::Null)
          targets.null = true;
        else
          targets.unknown = true;
      }
      result.join(targets);
    }
    return result;
  }
  if (origin.place) {
    if (const auto it = state.callTargets.find(origin.place->place);
        it != state.callTargets.end())
      return it->second;
    // RFC 0030 §9.3: the slots say what a global or a field can hold.
    if (const auto slotted = slotTargetsOf(origin.place->place);
        slotted && !slotted->empty())
      return *slotted;
  }
  return {};
}

core::CallTargets FunctionDataflow::functionTargets(const Expr &expr,
                                                    core::AnalysisState &state,
                                                    unsigned depth) {
  if (depth > core::MaxHeapPathDepth)
    return core::CallTargets::any();
  const Expr *e = expr.IgnoreParens();
  if (const auto *cast = dyn_cast<CastExpr>(e)) {
    if (cast->getCastKind() == CK_NullToPointer)
      return {.functions = {}, .unknown = false, .null = true};
    if (cast->getCastKind() == CK_IntegralToPointer ||
        cast->getCastKind() == CK_PointerToIntegral)
      return core::CallTargets::any();
    if (cast->getCastKind() == CK_BitCast &&
        !ASTContext::hasSameUnqualifiedType(cast->getType(),
                                            cast->getSubExpr()->getType()))
      return core::CallTargets::any();
    return functionTargets(*cast->getSubExpr(), state, depth + 1);
  }
  if (const auto *unary = dyn_cast<UnaryOperator>(e);
      unary &&
      (unary->getOpcode() == UO_AddrOf || unary->getOpcode() == UO_Deref))
    return functionTargets(*unary->getSubExpr(), state, depth + 1);
  if (const auto *conditional = dyn_cast<ConditionalOperator>(e)) {
    auto result =
        functionTargets(*conditional->getTrueExpr(), state, depth + 1);
    result.join(
        functionTargets(*conditional->getFalseExpr(), state, depth + 1));
    return result;
  }
  if (const auto *ref = dyn_cast<DeclRefExpr>(e)) {
    if (const auto *fn = dyn_cast<FunctionDecl>(ref->getDecl())) {
      summaries.registerCallable(*fn);
      return core::CallTargets::function(callableSymbol(*fn));
    }
  }
  if (auto place = builder.resolve(*e)) {
    const auto found = state.callTargets.find(place->place);
    if (found == state.callTargets.end() || found->second.unknown) {
      ValueOrigin input;
      input.kind = ValueOrigin::Kind::Copy;
      input.place = *place;
      const auto source = sourceValueOf(input, state, true);
      const auto &path = source.path;
      if (path && path->isParam() && recording())
        inferred.callbackInputs.insert(*path);
    }
    if (found != state.callTargets.end())
      return found->second;
    // RFC 0030 §9.3: the solved slot of a global or a field.
    if (const auto slotted = slotTargetsOf(place->place);
        slotted && !slotted->empty())
      return *slotted;
  }
  auto staticValue = SummaryStore::staticTargets(*e);
  if (!staticValue.empty())
    return staticValue;
  if (const auto *ref = dyn_cast<DeclRefExpr>(e)) {
    if (const auto *var = dyn_cast<VarDecl>(ref->getDecl());
        var && var->hasGlobalStorage() && var->hasInit())
      return functionTargets(*var->getInit(), state, depth + 1);
  }
  if (const auto *member = dyn_cast<MemberExpr>(e)) {
    const Expr *base = member->getBase()->IgnoreParenImpCasts();
    if (const auto *address = dyn_cast<UnaryOperator>(base);
        address && address->getOpcode() == UO_AddrOf)
      base = address->getSubExpr()->IgnoreParenImpCasts();
    if (const auto *ref = dyn_cast<DeclRefExpr>(base)) {
      if (const auto *var = dyn_cast<VarDecl>(ref->getDecl());
          var && var->hasGlobalStorage() && var->hasInit()) {
        if (const auto *init =
                dyn_cast<InitListExpr>(var->getInit()->IgnoreParenImpCasts())) {
          const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
          if (field && field->getFieldIndex() < init->getNumInits())
            return functionTargets(*init->getInit(field->getFieldIndex()),
                                   state, depth + 1);
        }
      }
    }
  }
  const auto origin = builder.classifyValue(*e);
  auto result = originTargets(origin, state);
  return result.empty() ? core::CallTargets::any() : result;
}

std::optional<ResolvedSummary>
FunctionDataflow::resolveCall(const CallExpr &call) {
  if (const auto cached = callSummaries.find(&call);
      cached != callSummaries.end()) {
    if (!cached->second)
      return std::nullopt;
    const core::LibraryMatch *row = resolvedLibrary(call);
    return ResolvedSummary{.summary = cached->second,
                           .source = callSources.at(&call),
                           .library = row != nullptr ? std::optional(*row)
                                                     : std::nullopt};
  }
  const FunctionDecl *direct = call.getDirectCallee();
  if (!currentState)
    return direct ? summaries.lookup(*direct) : summaries.lookupIndirect(call);
  core::AnalysisState &state = *currentState;
  std::shared_ptr<const core::FunctionSummary> result;
  SummarySource source = SummarySource::Inferred;
  std::optional<core::LibraryMatch> library;
  const auto captureCallbacks = [&](const core::FunctionSummary &summary) {
    core::CallbackBindings bindings;
    for (const auto &path : summary.callbackInputs) {
      if (path.root == core::SummaryRoot::Param &&
          path.index < call.getNumArgs() && path.steps.empty()) {
        bindings[path] = functionTargets(*call.getArg(path.index), state);
      } else if (const auto place = builder.resolveSummaryPath(path, call)) {
        const auto actualPath = stableSummaryPathOf(place->place);
        const auto it = state.callTargets.find(place->place);
        if (it != state.callTargets.end())
          bindings[path] = it->second;
        else if (const auto slotted = slotTargetsOf(place->place))
          bindings[path] = *slotted;
        else
          bindings[path] = core::CallTargets::any();
        if (bindings[path].empty())
          bindings[path] = core::CallTargets::any();
        if (actualPath && bindings[path].unknown && recording() &&
            (actualPath->isGlobal() || actualPath->isParam()))
          inferred.callbackInputs.insert(*actualPath);
      }
    }
    // RFC 0022: replaying the generic unresolved global set adds no
    // target or nullness premise. Keep its dependency, not a duplicate
    // specialization whose entry state would resolve the same set.
    std::erase_if(bindings, [](const auto &binding) {
      return binding.first.isGlobal() && binding.second.unknown &&
             binding.second.functions.empty();
    });
    return bindings;
  };
  const auto contextualize =
      [&](std::string_view symbol,
          std::shared_ptr<const core::FunctionSummary> base) {
        // Path resolution validates this target's object views before using
        // its footprint. The final contextual result replaces this below.
        auto &snapshot = callSummaries[&call];
        snapshot = std::move(base);
        auto bindings = captureCallContext(call, *snapshot, state);
        if (!bindings)
          return snapshot;
        if (const auto callbacks = callbackContexts.find(&call);
            callbacks != callbackContexts.end())
          bindings->callbacks = callbacks->second;
        memoryContexts[&call] = *bindings;
        // RFC 0030 §2.6: the context run's findings are this call's.
        const bool reporting = recording() && emitDiagnostics;
        std::vector<core::Diagnostic> found;
        const auto specialized = summaries.specializeMemory(
            symbol, *bindings, options, reporting ? &found : nullptr);
        if (reporting && !ledger.isDiscarding())
          summaries.claimedMemoryContexts.insert(
              {std::string(symbol), *bindings});
        reportContextFindings(call, std::move(found),
                              "called here with related pointer arguments");
        if (!specialized) {
          decideIncomplete("call context unavailable or limit reached", call);
          return snapshot;
        }
        // RFC 0030 §15 item 3: what the context run could not model (it
        // decides no rows of its own, §2.6) leaves this call's use of its
        // summary unresolved.
        for (const std::string &reason : specialized->summary->incomplete)
          if (incompleteFacet(reason) == core::Facet::Temporal) {
            decideIncomplete(reason, call);
            break;
          }
        return summaries.retainSummary(*specialized);
      };
  if (direct) {
    if (const auto base = summaries.lookup(*direct)) {
      result = summaries.retainSummary(*base);
      source = base->source;
      library = base->library;
      auto bindings = captureCallbacks(*result);
      if (!result->callbackInputs.empty()) {
        // A known target supplies its actual body and memory effects
        // (RFC 0014), rather than release semantics from its C prototype.
        const bool known =
            !bindings.empty() &&
            std::ranges::any_of(bindings, [](const auto &binding) {
              return !binding.second.functions.empty() || binding.second.null;
            });
        if (known) {
          callbackContexts[&call] = bindings;
          if (const auto specialized =
                  summaries.specialize(*direct, bindings, options, nullptr)) {
            result = summaries.retainSummary(*specialized);
          } else {
            decideIncomplete("callback context unavailable or limit reached",
                             call);
          }
        }
      }
      const bool knownBody =
          direct->getDefinition() != nullptr ||
          (summaries.programDatabase() != nullptr &&
           summaries.programDatabase()->defines(direct->getName()));
      if (source == SummarySource::Inferred ||
          source == SummarySource::Program ||
          (source == SummarySource::Annotation && knownBody)) {
        summaries.registerCallable(*direct);
        result = contextualize(callableSymbol(*direct), std::move(result));
      }
      if (!memoryContexts.contains(&call) && callbackContexts.contains(&call) &&
          recording() && emitDiagnostics && memoryContext.reportDiagnostics) {
        std::vector<core::Diagnostic> found;
        (void)summaries.specialize(*direct, callbackContexts.at(&call), options,
                                   &found);
        if (!ledger.isDiscarding())
          summaries.claimedCallbackContexts.insert(
              {callableSymbol(*direct), callbackContexts.at(&call)});
        // RFC 0030 §2.6: a context run's finding names the call.
        reportContextFindings(call, std::move(found),
                              "called here with function pointer arguments");
      }
    }
  } else {
    auto targets = functionTargets(*call.getCallee(), state);
    if (const auto place = builder.resolvePointerValue(*call.getCallee());
        place && state.nulls.isNonNull(place->place))
      targets.null = false;
    // RFC 0030 §9.3: the solved slot decides the call. Its targets are
    // flow-insensitive, so they speak only where the flow-sensitive ones
    // say nothing; the four behaviours follow from the slot's kind.
    const core::CallResolution resolution = slotResolutionOf(call);
    callResolutions[&call] = resolution;
    // What this function has watched the callee operand hold is exact and
    // wins: `g = unknown; g(p);` calls the value just stored, whatever the
    // flow-insensitive slot may also hold.
    bool tracked = false;
    if (const auto operand = builder.resolvePointerValue(*call.getCallee()))
      tracked = state.callTargets.contains(operand->place);
    if (!tracked && targets.unknown && !resolution.targets.empty() &&
        resolution.kind != core::IndirectCallKind::OpenUnknown &&
        // A slot with more targets than a `CallTargets` can name says
        // little and costs a summary join and a context run per target:
        // the §5.1 default is both sound and cheaper.
        resolution.targets.size() <= core::MaxCallTargets) {
      for (const std::string &target : resolution.targets) {
        if (target == core::UnknownFunction)
          continue;
        if (const clang::FunctionDecl *definition =
                options.slots->function(target))
          summaries.registerCallable(*definition);
        targets.functions.insert(slotSymbol(*options.slots, target));
      }
      // Closed: every value the slot can hold is here. Open with known
      // targets: as closed for temporal facts, which is what the summaries
      // below carry; the call's own facet says the values come from
      // outside (`openCallTemporalDecision`).
      targets.unknown = false;
    }
    callTargetsSeen[&call] = targets;
    // An explicit type contract can cover an unresolved target. Known
    // targets still supply their actual effects when there is no contract.
    if (const auto contract = summaries.lookupIndirect(call)) {
      result = summaries.retainSummary(*contract);
      source = contract->source;
    } else {
      bool returns = false;
      std::optional<SummarySource> singleSource;
      std::shared_ptr<core::FunctionSummary> joined;
      for (const auto &symbol : targets.functions) {
        const auto target = summaries.lookupSymbol(symbol);
        if (!target) {
          targets.unknown = true;
          continue;
        }
        if (targets.functions.size() == 1 && !targets.unknown &&
            !targets.null) {
          singleSource = target->source;
          library = target->library;
        }
        auto targetSummary = summaries.retainSummary(*target);
        callbackContexts.erase(&call);
        if (target->source != SummarySource::Library &&
            !targetSummary->callbackInputs.empty()) {
          auto bindings = captureCallbacks(*targetSummary);
          const bool known =
              std::ranges::any_of(bindings, [](const auto &binding) {
                return !binding.second.functions.empty() || binding.second.null;
              });
          if (known) {
            callbackContexts[&call] = bindings;
            if (const auto *definition = summaries.callable(symbol)) {
              if (const auto specialized = summaries.specialize(
                      *definition, bindings, options, nullptr))
                targetSummary = summaries.retainSummary(*specialized);
              else
                decideIncomplete(
                    "callback context unavailable or limit reached", call);
            } else {
              core::CallContext callbackContext;
              callbackContext.callbacks = bindings;
              if (const auto specialized = summaries.specializeMemory(
                      symbol, callbackContext, options, nullptr))
                targetSummary = summaries.retainSummary(*specialized);
              else
                decideIncomplete(
                    "callback context unavailable or limit reached", call);
            }
          }
        }
        auto actual = target->source == SummarySource::Library
                          ? std::move(targetSummary)
                          : contextualize(symbol, std::move(targetSummary));
        returns |= !actual->neverReturns;
        if (!result) {
          result = std::move(actual);
        } else {
          if (!joined)
            joined = std::make_shared<core::FunctionSummary>(*result);
          joined->join(*actual);
          result = joined;
        }
      }
      if (result && result->neverReturns != (!returns && !targets.unknown)) {
        if (!joined)
          joined = std::make_shared<core::FunctionSummary>(*result);
        joined->neverReturns = !returns && !targets.unknown;
        result = joined;
      }
      if (targets.unknown || targets.null || targets.functions.empty()) {
        if (result) {
          if (recording())
            inferred.incomplete.insert(
                "indirect call has an unresolved target");
          // Retain known effects and the unresolved alternative's boundary.
          handleUncheckedCall(call, state);
        }
      }
      if (singleSource && !targets.unknown && !targets.null)
        source = *singleSource;
      // §9.3: spatial and null facts of the result never come from an open
      // slot, because they are decided per TU and copied verbatim at link
      // (§1). The result takes the §7.3 default for a function outside the
      // unit instead.
      if (result && resolution.kind == core::IndirectCallKind::OpenKnown &&
          !result->returns.empty()) {
        auto opened = std::make_shared<core::FunctionSummary>(*result);
        opened->returns.clear();
        result = std::move(opened);
      }
    }
  }
  if (result && source == SummarySource::Library && library) {
    auto specialized = std::make_shared<core::FunctionSummary>(*result);
    specializeIntegerBuiltin(call, *library, *specialized, state);
    result = std::move(specialized);
  }
  if (source != SummarySource::Library)
    library.reset();
  callSources[&call] = source;
  if (library)
    callLibraries.insert_or_assign(&call, *library);
  else
    callLibraries.erase(&call);
  auto &cached = callSummaries[&call];
  cached = std::move(result);
  if (!cached)
    return std::nullopt;
  return ResolvedSummary{
      .summary = cached, .source = source, .library = library};
}

void FunctionDataflow::reportContextFindings(
    const CallExpr &call, std::vector<core::Diagnostic> found,
    std::string_view note) {
  if (found.empty())
    return;
  const SiteInfo *site = siteFor(call, core::Facet::Temporal);
  for (core::Diagnostic &diagnostic : found) {
    const core::Certainty certainty = diagnostic.certainty;
    const std::optional<core::Facet> facet = facetOfDiagnostic(diagnostic.id);
    const bool temporal = facet == core::Facet::Temporal;
    // The call's temporal facet: a violation when the finding is definite
    // in the context, `may-released` (or `may-moved`) when possible.
    if (temporal)
      decide(site, core::Facet::Temporal,
             certainty == core::Certainty::Definite
                 ? core::FacetDecision::violation()
                 : core::FacetDecision::unresolvedFor(
                       diagnostic.id == core::diag::UseAfterMove
                           ? core::UnresolvedReason::MayMoved
                           : core::UnresolvedReason::MayReleased));
    if (!note.empty())
      diagnostic.addNote(std::string(note), locate(call));
    report(std::move(diagnostic), certainty, temporal ? site : nullptr, facet);
  }
}

} // namespace weavec::analysis
