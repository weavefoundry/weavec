//===- ProgramAnalysis.h - Whole-program driver ----------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Analyses a program of several translation units so that callers see the
// summaries of callees defined in other units (RFC 0005, *The whole-program
// algorithm*): discover what each unit defines and imports, order the units
// dependencies-first by strongly connected component, analyse acyclic units
// once and cyclic groups to a fixpoint, each against a database of the
// exports analysed so far.
//
// The orchestrator does not know how a unit is parsed: `ProgramUnit` runs a
// frontend action over one unit, from a compilation database (`weavec
// --whole-program`) or a recorded cc1 command line (`weavec-cc`).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_PROGRAMANALYSIS_H
#define WEAVEC_FRONTEND_PROGRAMANALYSIS_H

#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Frontend/DiagnosticControl.h"
#include "weavec/Frontend/FrontendAction.h"

#include "clang/Tooling/Tooling.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace weavec::frontend {

/// One translation unit of a program.
class ProgramUnit {
public:
  virtual ~ProgramUnit() = default;

  /// The unit's name for messages, usually its main source file.
  [[nodiscard]] virtual std::string name() const = 0;

  /// Parses the unit and runs the action `factory` creates over it. False
  /// if the unit could not be processed (the reason has been printed).
  /// Diagnostics the action emits do not make this false.
  virtual bool run(clang::tooling::FrontendActionFactory &factory) = 0;
};

/// The whole-program algorithm over an arbitrary set of units.
class ProgramAnalysis {
public:
  struct Result {
    /// Units that could not be parsed or re-run, by name.
    std::vector<std::string> failed;
    /// Cyclic components that did not settle within `MaxRounds`, as lists
    /// of unit names.
    std::vector<std::vector<std::string>> nonConverging;
    /// WeaveC errors and warnings printed.
    std::size_t errors = 0;
    std::size_t warnings = 0;

    [[nodiscard]] bool ok() const noexcept {
      return failed.empty() && nonConverging.empty() && errors == 0;
    }
  };

  /// `opts.database`, `alreadyReported`, `boundaryOnce`, `silent`,
  /// `discoverOnly` and `onResult` are managed by the analysis; the rest
  /// apply to every unit.
  explicit ProgramAnalysis(FrontendOptions opts);

  /// Adds a unit to analyse. `known` are its exports if they are already
  /// on hand (from a sidecar); otherwise the unit is parsed once to discover
  /// them. `reported` are the diagnostics an earlier step already printed
  /// for it.
  void addUnit(std::unique_ptr<ProgramUnit> unit,
               std::optional<analysis::UnitExports> known = std::nullopt,
               std::set<ReportedDiagnostic> reported = {});

  /// Adds the exports of a unit that is part of the program but is not
  /// analysed again (an object whose compile-time view already stands).
  void addExports(analysis::UnitExports exports);

  [[nodiscard]] Result run();

  /// After `run`: the exports of every unit, joined.
  [[nodiscard]] const analysis::ProgramDatabase &database() const noexcept {
    return settled;
  }

  /// Upper bound on fixpoint rounds for a cyclic component.
  static constexpr unsigned MaxRounds = 16;
  /// RFC 0011, *Whole-program widening*: from this round on a member's new
  /// exports are joined with its previous ones before comparison, so the
  /// sequence is monotone in the finite summary lattice and settles.
  static constexpr unsigned WidenAfter = 6;
  /// The widening step: joins each of `exports`' function summaries with
  /// the same function's summary in a member's `previous` exports (both
  /// numbered by one database), and unions the count fields.
  static void widen(analysis::UnitExports &exports,
                    const analysis::UnitExports &previous);

private:
  struct Unit {
    std::unique_ptr<ProgramUnit> unit;
    std::optional<analysis::UnitExports> exports;
    std::set<ReportedDiagnostic> reported;
  };

  FrontendOptions options;
  std::vector<Unit> units;
  std::vector<analysis::UnitExports> fixed;
  analysis::ProgramDatabase settled;
  std::set<std::string> boundaryOnce;

  /// Runs `unit` with `options` completed by `overrides`; the result of the
  /// consumer, or `nullopt` if the unit could not be processed.
  std::optional<UnitResult> runUnit(ProgramUnit &unit,
                                    const FrontendOptions &overrides);
  [[nodiscard]] std::vector<std::vector<unsigned>> unitGraph() const;
  void analyzeAcyclic(unsigned index, Result &result);
  void analyzeCyclic(const std::vector<unsigned> &component, Result &result);
  /// `settled` plus the exports of a cyclic component's members.
  [[nodiscard]] analysis::ProgramDatabase
  databaseFor(const std::vector<analysis::UnitExports> &members) const;
};

/// A unit parsed by `ClangTool` from a compilation database, as `weavec
/// --whole-program` does.
class CompilationDatabaseUnit final : public ProgramUnit {
public:
  CompilationDatabaseUnit(
      const clang::tooling::CompilationDatabase &compilations,
      std::string sourcePath,
      std::vector<clang::tooling::ArgumentsAdjuster> adjusters)
      : compilations(compilations), source(std::move(sourcePath)),
        adjusters(std::move(adjusters)) {}

  [[nodiscard]] std::string name() const override { return source; }
  bool run(clang::tooling::FrontendActionFactory &factory) override;

private:
  const clang::tooling::CompilationDatabase &compilations;
  std::string source;
  std::vector<clang::tooling::ArgumentsAdjuster> adjusters;
};

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_PROGRAMANALYSIS_H
