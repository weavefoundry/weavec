//===- DataflowLibraryEffects.cpp - Hidden state and callbacks (§8, §5.3) -===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §8.2: what a `LibrarySpec` row says beyond a summary. Hidden state
// slot `S` is a synthetic global place `<S>`: `retain(S)` stores the argument
// there, `reads(S)` uses what it holds (a released value is a use after
// free, probe 25), and `invalidates(S)` possibly ends every other name for
// its value (probe 25c); `static(S)` results are copies of it (PlaceBuilder).
// §5.3: a `sync` callback's target runs zero or more times during the call:
// its effects on globals apply as may-effects (probe 47), and what it may do
// through the arguments it is handed is the unknown-callee default on them;
// an unresolved target is that default with reason `callback`.
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

using namespace clang;

namespace weavec::analysis {

void FunctionDataflow::applyLibraryState(const CallExpr &call,
                                         const core::LibraryMatch &library,
                                         core::AnalysisState &state) {
  const core::LibraryEntry &row = *library.entry;
  // `invalidates(S)` comes first: the call's own `static(S)` result is a
  // borrow of the new value. The slot holds a new value; the old one's other
  // names may be dangling (C does not say the storage moves: conditional).
  for (const std::string &slot : row.invalidates)
    (void)doConsume(PlaceRef{.place = builder.statePlace(slot),
                             .derefs = {},
                             .element = {}},
                    core::MoveReason::Freed, call, state, {}, /*library=*/true,
                    /*replaced=*/true, {}, false, {},
                    core::MoveOrigin{.conditional = true, .lossy = true});
  // `retain(S)`: a non-null argument is what slot `S` holds from now on.
  for (unsigned rowArg = 0; rowArg < row.params.size(); ++rowArg) {
    const core::LibraryParam &param = row.params[rowArg];
    const int index = library.callArgument(rowArg);
    if (param.effect != core::LibraryParam::Effect::Retain || index < 0 ||
        static_cast<unsigned>(index) >= call.getNumArgs())
      continue;
    const Expr &arg = *call.getArg(static_cast<unsigned>(index));
    const ValueOrigin value = builder.classifyValue(arg);
    const auto nullness = nullnessOf(value, arg, state);
    if (value.kind == ValueOrigin::Kind::Null ||
        (nullness && nullness->state == core::Nullness::Null))
      continue;
    applyPointerAssign(builder.statePlace(param.state), value, call,
                       /*constPointee=*/false, state);
  }
  // `reads(S)`: the call uses what the slot holds. One report: what the
  // call hands out afterwards is not reported again.
  for (const std::string &slot : row.reads) {
    const core::PlaceId place = builder.statePlace(slot);
    if (const auto hit = findMoved(place, state)) {
      if (publishing())
        reportUseOfMoved(place, *hit, call);
      reinit(place, state);
    }
  }
}

bool FunctionDataflow::fromLibraryState(core::PlaceId pointer,
                                        const core::AnalysisState &state) {
  if (builder.isStatePlace(pointer))
    return true;
  return llvm::any_of(state.aliases.edgesFrom(pointer), [&](const auto &edge) {
    return builder.isStatePlace(edge.first);
  });
}

bool FunctionDataflow::applyLibraryCallbacks(const CallExpr &call,
                                             const core::LibraryMatch &library,
                                             core::AnalysisState &state) {
  bool resolved = true;
  const core::SourceLocation here = locate(call);
  for (unsigned rowArg = 0; rowArg < library.entry->params.size(); ++rowArg) {
    const core::LibraryParam &param = library.entry->params[rowArg];
    const int index = library.callArgument(rowArg);
    if (!param.callback ||
        param.callback->kind != core::LibCallback::Kind::Sync || index < 0 ||
        static_cast<unsigned>(index) >= call.getNumArgs() ||
        param.type != core::LibraryParam::Type::Function)
      continue;
    // The arguments the target's pointer parameters point into.
    std::vector<const Expr *> handed;
    for (const std::uint8_t into : param.callback->arguments) {
      const int at = library.callArgument(into);
      if (at >= 0 && static_cast<unsigned>(at) < call.getNumArgs() &&
          !llvm::is_contained(handed, call.getArg(static_cast<unsigned>(at))))
        handed.push_back(call.getArg(static_cast<unsigned>(at)));
    }
    const core::CallTargets targets =
        functionTargets(*call.getArg(static_cast<unsigned>(index)), state);
    std::vector<SummarySnapshot> targetSummaries;
    bool unknown = targets.unknown;
    for (const std::string &symbol : targets.functions) {
      if (const auto target = summaries.lookupSymbol(symbol))
        targetSummaries.push_back(target->summary);
      else
        unknown = true;
    }
    // What a target may do through the pointers it is handed (a borrow
    // into the named arguments): anything but reading gets the default.
    const bool writesHanded =
        llvm::any_of(targetSummaries, [](const auto &summary) {
          return llvm::any_of(summary->effects,
                              [](const auto &entry) {
                                return entry.first.isParam() &&
                                       (entry.second.mutates() ||
                                        entry.second.escaped ||
                                        entry.second.unknown);
                              }) ||
                 llvm::any_of(summary->stores, [](const core::Store &store) {
                   return store.dest.isParam();
                 });
        });
    unknownCode = unknown ? "callback" : unquotedCalleeName(call);
    unknownIsCallback = unknown;
    if (unknown || writesHanded)
      for (const Expr *arg : handed)
        if (arg->getType()->isPointerType())
          applyUnknownToValue(builder.classifyValue(*arg), /*readOnly=*/false,
                              here, state);
    if (unknown) {
      applyUnknownToReachable(here, state);
      resolved = false;
      continue;
    }
    // Their effects on globals, as may-effects (zero or more invocations):
    // a release is conditional and never settled by a test of the result; a
    // global they store to holds its old value or the stored one.
    for (const SummarySnapshot &summary : targetSummaries) {
      auto may = std::make_shared<core::FunctionSummary>();
      // An incomplete target's may-effects (§5.5) reach the arguments too.
      may->incomplete = summary->incomplete;
      std::vector<core::SummaryPath> stored;
      for (const auto &[path, effect] : summary->effects)
        if (path.isGlobal())
          may->addEffect(path, effect);
      for (const core::Store &store : summary->stores)
        if (store.dest.isGlobal())
          stored.push_back(store.dest);
      CallEffects effects;
      effects.summary = may;
      effects.source = SummarySource::Inferred;
      // Every argument is the row's (none is a variadic one to escape).
      effects.declaredParams = call.getNumArgs();
      mayEffects = true;
      applySummary(call, effects, state);
      applyUnknownEffects(call, effects, state);
      mayEffects = false;
      for (const core::SummaryPath &path : stored)
        if (const auto ref = builder.resolveSummaryPath(path, call))
          forgetFactsOf({ref->place}, state);
    }
  }
  return resolved;
}

} // namespace weavec::analysis
