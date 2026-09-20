//===- FunctionAnalysis.h - Per-function ownership analysis ----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The analysis layer walks Clang ASTs, translates them into facts about core
// `PlaceId`s, and drives the core model to produce diagnostics. It is the only
// layer that knows about both Clang and the core model.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_FUNCTIONANALYSIS_H
#define WEAVEC_ANALYSIS_FUNCTIONANALYSIS_H

#include "weavec/Analysis/Summaries.h"
#include "weavec/Core/AnalysisStats.h"
#include "weavec/Core/Diagnostic.h"
#include "weavec/Core/Ledger.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"

#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <set>
#include <string>

namespace weavec::core {
class SlotSolution;
} // namespace weavec::core

namespace weavec::analysis {

class SlotCollection;

class KindInferenceResult;
class KindTable;
class LedgerAdapter;

/// Tunables for the analyses.
struct AnalysisOptions {
  /// RFC 0030 §5.5: the CFG block transfers one run of `FunctionDataflow`
  /// may make over one function body, fixpoint and final pass together
  /// (`-fweavec-budget`, `--budget`); 0 is unlimited. The context runs of
  /// one function share a second budget of the same size.
  std::uint64_t budget = core::DefaultBudget;
  /// RFC 0030 §11: locals and the lowered allocations are zero-initialised
  /// (not `-fno-weavec-zero-init`). Without it a possibly uninitialised
  /// pointer's null facet is `unresolved(no-zero-init)`.
  bool zeroInit = true;
  /// RFC 0030 §3.1: the unit follows C's effective-type rules; under
  /// `-fno-strict-aliasing` every two pointee types may designate one
  /// object (`may-alias-released`).
  bool strictAliasing = true;
  /// If set, print the inferred facts for every analysed function
  /// (`--dump-analysis`): places and their kinds, lifetimes, and the state
  /// at function exit. Intended for debugging and lit tests; the format is
  /// not stable.
  llvm::raw_ostream *dumpStream = nullptr;
  /// RFC 0020: optional invocation-owned work accounting.
  core::AnalysisStats *stats = nullptr;
  /// Optional immutable preparation owned by the current retained AST.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::shared_ptr<FunctionPreparationCache> preparation = {};
  /// RFC 0030 §7, §15 item 14: the unit's kinds, which seed extents and
  /// nullness at parameter entry, at slot loads and at call results, and
  /// the §7.5 requirements; null for none (every pointer then starts with
  /// nothing known, as before S6).
  const KindTable *kinds = nullptr;
  const KindInferenceResult *inferred = nullptr;
  /// RFC 0030 §9.3: the unit's function-pointer slots and the solution the
  /// indirect calls are resolved through (the unit's own in a per-TU
  /// compile, the program's at link and in `--whole-program`). Null for
  /// none: every indirect call is then judged by its flow-sensitive
  /// targets alone.
  const SlotCollection *slots = nullptr;
  const core::SlotSolution *slotSolution = nullptr;
};

/// Runs every WeaveC check over a single function definition.
///
/// Implements the sound intra-procedural checker of RFC 0002 (model:
/// RFC 0001): a forward dataflow over the function's `clang::CFG` whose
/// state is `core::AnalysisState`, followed by one final pass that reports
/// and records the function's summary (RFC 0003). Calls are interpreted
/// through the summaries in the `SummaryStore` handed to `analyze`;
/// `TranslationUnitAnalyzer` orders functions so callees come first.
class FunctionAnalyzer {
public:
  /// Everything the analysis publishes, its diagnostics included, goes
  /// through `ledgerAdapter` (RFC 0030 §14): the authoritative one for the
  /// reporting pass, a discarding one for fixpoint rounds.
  FunctionAnalyzer(clang::ASTContext &ctx, LedgerAdapter &ledgerAdapter,
                   AnalysisOptions analysisOptions = {});

  /// Analyzes `function`, which must have a body, resolving callees from
  /// `summaries` and recording the inferred summary into it. Diagnostics
  /// are emitted only if `emitDiagnostics`. Functions annotated
  /// `weavec.unsafe` are analyzed with body reports suppressed. Returns true if
  /// the recorded summary changed. Validate declaration annotations
  /// independently of body specialization.
  void validate(const clang::FunctionDecl &function);

  bool analyze(const clang::FunctionDecl &function, SummaryStore &summaries,
               bool emitDiagnostics = true, bool widenSummary = false);

private:
  clang::ASTContext &context;
  LedgerAdapter &ledger;
  AnalysisOptions options;
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_FUNCTIONANALYSIS_H
