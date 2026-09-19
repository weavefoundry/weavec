//===- DataflowEngine.h - SafetyEngine over FunctionDataflow ----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §14: `SafetyEngine` implemented over `TranslationUnitAnalyzer`
// and `FunctionDataflow`. Every diagnostic the engine produces reaches the
// ledger through `LedgerAdapter::report`, in the order the engine produces
// it, with a placeholder certainty (definite for errors, possible for
// warnings) and a link to the innermost site at its location that has the
// facet the diagnostic id governs. The adapter's `beginFunction` marks each
// reporting pass.
//
// Stage S3-B moves the decisions themselves into `FunctionDataflow`
// (§15): `decide`, `requirement` and `witness` at each check, and certainty
// from the new `MoveRecord`, `NullRecord` and `Loan` bits.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_DATAFLOWENGINE_H
#define WEAVEC_ANALYSIS_DATAFLOWENGINE_H

#include "weavec/Analysis/FunctionAnalysis.h"
#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Analysis/SafetyEngine.h"
#include "weavec/Core/Diagnostic.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"

#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <optional>
#include <string_view>

namespace weavec::analysis {

class TranslationUnitAnalyzer;

/// The facet a diagnostic id is about (§3), or none for ids that are not
/// about a facet (`leak`, `invalid-annotation`, ...).
[[nodiscard]] std::optional<core::Facet> facetOfDiagnostic(std::string_view id);

class DataflowEngine final : public SafetyEngine {
public:
  DataflowEngine();
  ~DataflowEngine() override;
  DataflowEngine(const DataflowEngine &) = delete;
  DataflowEngine &operator=(const DataflowEngine &) = delete;
  DataflowEngine(DataflowEngine &&) = delete;
  DataflowEngine &operator=(DataflowEngine &&) = delete;

  void analyzeUnit(const EngineInput &input, LedgerAdapter &out) override;
  /// The exports `analyzeUnit` computed (RFC 0005); moved out once.
  [[nodiscard]] UnitExports exports() override;
  void dump(const clang::FunctionDecl &function,
            llvm::raw_ostream &os) override;

  /// RFC 0005 discovery: what the unit defines, imports and calls
  /// indirectly, without analysing anything. Summaries are empty.
  [[nodiscard]] static UnitExports discover(clang::ASTContext &unitContext,
                                            const EngineOptions &options,
                                            const ProgramDatabase *database);

private:
  class AdapterSink;
  std::unique_ptr<AdapterSink> sink;
  std::unique_ptr<TranslationUnitAnalyzer> analyzer;
  clang::ASTContext *context = nullptr;
  AnalysisOptions analysisOptions;
  UnitExports exported;
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_DATAFLOWENGINE_H
