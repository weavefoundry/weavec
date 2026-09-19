//===- UnitPipeline.cpp - One unit's analysis (RFC 0030) ------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/UnitPipeline.h"

#include "weavec/Analysis/AttributeReader.h"
#include "weavec/Analysis/DataflowEngine.h"
#include "weavec/Analysis/KindTable.h"
#include "weavec/Analysis/SiteCollector.h"
#include "weavec/Core/FnSlots.h"

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

  // §1 step 2: kinds, then sites, before any engine fact.
  KindTable kinds;
  auto sites = std::make_shared<SiteIndex>();
  if (options.buildLedger) {
    kinds = AttributeReader(context, library).read();
    *sites = SiteCollector(context, kinds, library).collect();
  }

  LedgerAdapterOptions adapterOptions;
  adapterOptions.config = options.config;
  adapterOptions.source = mainSource(context);
  adapterOptions.target = context.getTargetInfo().getTriple().str();
  adapterOptions.lowered = options.lowered;
  // A silent round (no ledger) keeps nothing (§2.6).
  LedgerAdapter adapter(context, *sites, std::move(adapterOptions),
                        options.buildLedger ? LedgerAdapter::Mode::Authoritative
                                            : LedgerAdapter::Mode::Discarding);

  // Stage S7 solves the unit's function-pointer slots and S6 proposes the
  // field invariants; until then both are empty.
  const core::FnSlots slots{};
  const std::vector<FieldCandidate> fieldAssumptions{};
  const EngineInput input{
      .context = context,
      .sites = *sites,
      .kinds = kinds,
      .library = library,
      .slots = slots,
      .database = options.database,
      .fieldAssumptions = fieldAssumptions,
      .options = options.engine,
  };
  DataflowEngine engine;
  engine.analyzeUnit(input, adapter);
  result.exports = engine.exports();

  auto planned = std::make_shared<PlannedLedger>(adapter.finish());
  planned->sites = std::move(sites);
  if (options.buildLedger)
    result.ledger = std::move(planned);

  // §1 step 3: the diagnostics, in the order they were published.
  for (const core::Diagnostic &diagnostic : adapter.diagnostics())
    out.report(diagnostic);
  return result;
}

} // namespace weavec::analysis
