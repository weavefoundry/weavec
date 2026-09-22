//===- UnitPipeline.cpp - One unit's analysis (RFC 0030) ------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/UnitPipeline.h"

#include "weavec/Analysis/BoundaryInvariants.h"
#include "weavec/Analysis/Concurrency.h"
#include "weavec/Analysis/DataflowEngine.h"
#include "weavec/Analysis/KindInference.h"
#include "weavec/Analysis/KindTable.h"
#include "weavec/Analysis/SiteCollector.h"

#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"

#include <string>
#include <utility>
#include <vector>

namespace weavec::analysis {

/// The main source as the user named it.
static std::string mainSource(const clang::ASTContext &context) {
  const clang::SourceManager &sm = context.getSourceManager();
  if (const auto entry = sm.getFileEntryRefForID(sm.getMainFileID()))
    return entry->getName().str();
  return {};
}

UnitPipelineResult runUnitAnalysis(clang::ASTContext &context,
                                   const UnitPipelineOptions &options,
                                   core::DiagnosticSink &out) {
  UnitPipelineResult result;
  if (options.discoverOnly) {
    result.exports =
        DataflowEngine::discover(context, options.engine, options.database);
    return result;
  }
  const core::LibrarySpec &library = options.library != nullptr
                                         ? *options.library
                                         : core::LibrarySpec::shipped();

  // §1 step 2: kinds, then sites, before any engine fact. Every round needs
  // the kinds, which seed the engine's extents (§15 item 14), so that a
  // silent round computes the summaries the authoritative one does; only a
  // round that builds the ledger needs the sites.
  std::shared_ptr<const UnitKinds> kinds = UnitKinds::build(context, library);
  auto sites = std::make_shared<SiteIndex>();
  if (options.buildLedger)
    *sites = SiteCollector(context, kinds->table, library, &kinds->inferred,
                           &kinds->slots, &kinds->solution)
                 .collect();

  LedgerAdapterOptions adapterOptions;
  adapterOptions.config = options.config;
  adapterOptions.source = mainSource(context);
  adapterOptions.target = context.getTargetInfo().getTriple().str();
  adapterOptions.lowered = options.lowered;
  // §5.3: G, what threads and signal handlers share, before the engine runs.
  if (options.buildLedger) {
    auto share = std::make_shared<const ConcurrencyShare>(
        collectConcurrencyShare(context, library));
    if (!share->empty())
      adapterOptions.concurrent = [share](const SiteInfo &info) {
        return info.operand != nullptr && rootedInShare(*info.operand, *share);
      };
  }
  // A silent round (no ledger) keeps nothing (§2.6).
  LedgerAdapter adapter(context, *sites, std::move(adapterOptions),
                        options.buildLedger ? LedgerAdapter::Mode::Authoritative
                                            : LedgerAdapter::Mode::Discarding);

  // *Diagnostics*: the malformed and conflicting declarations
  // `AttributeReader` found (`invalid-annotation`), before the engine's.
  for (core::Diagnostic &diagnostic :
       kindProblemDiagnostics(kinds->table, context.getSourceManager()))
    adapter.report(std::move(diagnostic), core::Certainty::Possible);

  // The §7.6 candidates (the designated first cut) are not assumed.
  const std::vector<FieldCandidate> fieldAssumptions{};
  const EngineInput input{
      .context = context,
      .sites = *sites,
      .kinds = kinds->table,
      .library = library,
      .slots = kinds->slots.constraints(),
      .database = options.database,
      .fieldAssumptions = fieldAssumptions,
      .inferred = &kinds->inferred,
      .slotCollection = &kinds->slots,
      .slotSolution = &kinds->solution,
      .options = options.engine,
  };
  DataflowEngine engine;
  engine.analyzeUnit(input, adapter);
  result.exports = engine.exports();

  // §1 step 2, §9.4: the boundary invariants judge what the engine
  // published, and `finish` records their rows and propagation. At link the
  // other units' rows come along, so the propagation is program-wide
  // (§13.2 step 5).
  if (options.buildLedger) {
    llvm::ArrayRef<BoundaryRow> program;
    if (options.database != nullptr && options.database->programFacts)
      program = options.database->programFacts->boundaries;
    BoundaryVerdicts verdicts =
        checkBoundaryInvariants(*sites, adapter.boundaries(), program);
    result.exports.boundaries = std::move(verdicts.exported);
    adapter.boundaryDecisions(std::move(verdicts.decisions));
  }

  auto planned = std::make_shared<PlannedLedger>(adapter.finish());
  planned->sites = std::move(sites);
  if (options.buildLedger)
    result.ledger = std::move(planned);
  result.kinds = std::move(kinds);

  // §1 step 3: the diagnostics, in the order they were published.
  for (const core::Diagnostic &diagnostic : adapter.diagnostics())
    out.report(diagnostic);
  return result;
}

} // namespace weavec::analysis
