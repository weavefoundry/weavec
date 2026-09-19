//===- SafetyEngine.h - The engine interface (RFC 0030) ---------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §14: what an engine gets for one unit (`EngineInput`) and what it
// implements (`SafetyEngine`). Everything an engine produces flows through
// `LedgerAdapter`, so RFC 0031 can replace `FunctionDataflow` (today's
// engine, `DataflowEngine`) without touching the ledger, the kinds, the
// library table, the planner, the emitter, the formats or the tests.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_SAFETYENGINE_H
#define WEAVEC_ANALYSIS_SAFETYENGINE_H

#include "weavec/Analysis/FunctionAnalysis.h"
#include "weavec/Analysis/KindTable.h"
#include "weavec/Analysis/LedgerAdapter.h"
#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Analysis/SiteCollector.h"
#include "weavec/Analysis/Summaries.h"
#include "weavec/Core/FnSlots.h"
#include "weavec/Core/Ledger.h"
#include "weavec/Core/LibrarySpec.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"

#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace weavec::analysis {

/// An engine's tunables (§14).
struct EngineOptions {
  /// `FunctionDataflow`'s own tunables: `dumpStream`, `stats`, and until the
  /// sound defaults land (S3) the RFC 0003–0006 flags.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  AnalysisOptions analysis = {};
  /// §5.5: block transfers per function body; 0 is unlimited.
  std::uint64_t budget = core::DefaultBudget;
  /// §11: locals and allocations are zero-initialised.
  bool zeroInit = true;
  /// §6.3.
  core::RequireLevel require = core::RequireLevel::None;
  /// §10.7: publish witnesses for proven spatial facets too.
  bool verify = false;
  /// §3.1: the unit follows C's effective-type rules (not
  /// `-fno-strict-aliasing`).
  bool strictAliasing = true;
  /// The functions whose diagnostics are reported (`mainFileOnly` until
  /// every emitted function reports, §15 item 18); empty reports all.
  std::function<bool(const clang::FunctionDecl &)> shouldReport = nullptr;
  /// RFC 0020: receives the callee summaries the unit's analysis read.
  SummaryStore::Dependencies *dependencies = nullptr;
};

/// Everything an engine gets for one unit (§14).
struct EngineInput {
  clang::ASTContext &context;
  /// `SiteCollector`: statement to site, per function.
  const SiteIndex &sites;
  /// Declared (and, from S6, inferred) kinds (§7).
  const KindTable &kinds;
  const core::LibrarySpec &library;
  /// The function-pointer slots' local or program-wide solution (§9.3).
  const core::FnSlots &slots;
  /// Other units' records at link; null otherwise.
  const ProgramDatabase *database = nullptr;
  /// §7.6 candidates assumed at entry.
  const std::vector<FieldCandidate> &fieldAssumptions;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  EngineOptions options = {};
};

/// What an engine implements (§14).
class SafetyEngine {
public:
  virtual ~SafetyEngine();
  SafetyEngine(const SafetyEngine &) = delete;
  SafetyEngine &operator=(const SafetyEngine &) = delete;
  SafetyEngine(SafetyEngine &&) = delete;
  SafetyEngine &operator=(SafetyEngine &&) = delete;

  /// Analyses every emitted function of the unit, publishing through `out`.
  virtual void analyzeUnit(const EngineInput &input, LedgerAdapter &out) = 0;
  /// Summaries and exported facts for the unit record (§13.1).
  [[nodiscard]] virtual UnitExports exports() = 0;
  /// `--dump-analysis` text for one function (unstable format).
  virtual void dump(const clang::FunctionDecl &function,
                    llvm::raw_ostream &os) = 0;

protected:
  SafetyEngine() = default;
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_SAFETYENGINE_H
