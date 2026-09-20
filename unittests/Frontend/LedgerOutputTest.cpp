//===- LedgerOutputTest.cpp - Tests for writing ledgers and summaries -----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/LedgerOutput.h"

#include "weavec/Frontend/FrontendAction.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace weavec::frontend {

using core::Facet;
using core::FacetDecision;

namespace {
/// A fresh directory outside any repository, removed afterwards.
class ScratchDirectory {
public:
  ScratchDirectory() {
    EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("weavec-ledger", path));
  }
  ~ScratchDirectory() { std::ignore = llvm::sys::fs::remove_directories(path); }
  ScratchDirectory(const ScratchDirectory &) = delete;
  ScratchDirectory &operator=(const ScratchDirectory &) = delete;

  [[nodiscard]] std::string operator/(llvm::StringRef name) const {
    llvm::SmallString<256> joined(path);
    llvm::sys::path::append(joined, name);
    return joined.str().str();
  }
  [[nodiscard]] std::string str() const { return path.str().str(); }

private:
  llvm::SmallString<256> path;
};
} // namespace

/// One unit with one checked dereference, as a unit pipeline leaves it:
/// without the unit's identity.
static core::Ledger unitLedger() {
  core::Ledger ledger;
  core::FunctionLedger function{
      .name = "f", .line = 1, .linkage = core::Linkage::External};
  core::Site site{.ordinal = 0,
                  .kind = core::SiteKind::Deref,
                  .location = {.file = "", .line = 2, .column = 3, .opaque = 0},
                  .text = "*p"};
  site.addFacet(Facet::Null).decide(FacetDecision::checked());
  function.sites.push_back(site);
  ledger.units.emplace_back();
  ledger.units.front().functions.push_back(function);
  return ledger;
}

static std::optional<llvm::json::Value> readJson(const std::string &path) {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer)
    return std::nullopt;
  llvm::Expected<llvm::json::Value> value =
      llvm::json::parse((*buffer)->getBuffer());
  if (!value) {
    llvm::consumeError(value.takeError());
    return std::nullopt;
  }
  return std::move(*value);
}

TEST(LedgerOutput, SummaryFollowsTheLedgerUnlessSet) {
  LedgerOutputOptions options;
  EXPECT_FALSE(options.writesLedger());
  EXPECT_FALSE(options.printsSummary());
  options.path = "out/";
  EXPECT_TRUE(options.printsSummary());
  options.summary = false;
  EXPECT_FALSE(options.printsSummary());
  options.path.clear();
  options.summary = true;
  EXPECT_TRUE(options.printsSummary());
}

TEST(LedgerOutput, WritesAUnitLedgerIntoADirectory) {
  const ScratchDirectory scratch;
  core::Ledger ledger = unitLedger();
  const UnitIdentity unit{.source = "src/a.c",
                          .object = "build/a.o",
                          .target = "arm64-apple-macosx15.0",
                          .workingDirectory = scratch.str()};
  const core::LedgerConfig config{.checks = core::ChecksMode::Trap,
                                  .zeroInit = false,
                                  .require = core::RequireLevel::Checked,
                                  .budget = 7};
  const LedgerOutputOptions options{.path = scratch / "ledgers/"};
  std::string line;
  llvm::raw_string_ostream summary(line);
  std::string error;
  ASSERT_TRUE(emitUnitLedger(ledger, unit, config, options, summary, &error))
      << error;
  // §12.4: the line of a unit whose checks are not enforced.
  EXPECT_EQ(line, "weavec: src/a.c: 1 site: 0 proven, 1 checkable (not "
                  "enforced), 0 unresolved, 0 trusted; 0 errors, 0 warnings\n");

  const std::optional<llvm::json::Value> json =
      readJson(scratch / "ledgers/a.o.ledger.json");
  ASSERT_TRUE(json);
  const llvm::json::Object &root = *json->getAsObject();
  EXPECT_EQ(root.getString("schema"), "weavec-ledger");
  EXPECT_EQ(root.getString("scope"), "unit");
  EXPECT_EQ(root.getObject("producer")->getString("name"), "weavec");
  const llvm::json::Object &configJson = *root.getObject("config");
  EXPECT_EQ(configJson.getString("require"), "checked");
  EXPECT_EQ(configJson.getBoolean("zeroInit"), false);
  EXPECT_EQ(configJson.getInteger("budget"), 7);
  const llvm::json::Object &unitJson =
      *root.getArray("units")->front().getAsObject();
  EXPECT_EQ(unitJson.getString("source"), "src/a.c");
  EXPECT_EQ(unitJson.getString("object"), "build/a.o");
  EXPECT_EQ(unitJson.getString("target"), "arm64-apple-macosx15.0");
  EXPECT_EQ(
      unitJson.getArray("functions")->front().getAsObject()->getString("file"),
      "src/a.c");
  // No `.git` above the scratch directory: the root is the job's directory.
  EXPECT_EQ(ledger.root, scratch.str());
}

TEST(LedgerOutput, WritesSarifToAFileAndPrintsOnlyWhenAsked) {
  const ScratchDirectory scratch;
  core::Ledger ledger = unitLedger();
  const LedgerOutputOptions options{.path = scratch / "a.sarif",
                                    .format = LedgerFormat::Sarif,
                                    .summary = false};
  std::string line;
  llvm::raw_string_ostream summary(line);
  ASSERT_TRUE(emitUnitLedger(ledger,
                             UnitIdentity{.source = scratch / "a.c",
                                          .workingDirectory = scratch.str()},
                             core::LedgerConfig{}, options, summary));
  EXPECT_TRUE(line.empty());
  const std::optional<llvm::json::Value> sarif = readJson(scratch / "a.sarif");
  ASSERT_TRUE(sarif);
  EXPECT_EQ(sarif->getAsObject()->getString("version"), "2.1.0");
}

TEST(LedgerOutput, EnforcedChecksAreNamedChecked) {
  const LedgerOutputOptions enforced{.summary = true, .checksEnforced = true};
  for (const core::ChecksMode checks :
       {core::ChecksMode::Trap, core::ChecksMode::None}) {
    core::Ledger ledger = unitLedger();
    std::string line;
    llvm::raw_string_ostream summary(line);
    ASSERT_TRUE(emitUnitLedger(ledger, UnitIdentity{.source = "a.c"},
                               core::LedgerConfig{.checks = checks}, enforced,
                               summary));
    EXPECT_NE(line.find(checks == core::ChecksMode::None
                            ? "1 checkable (not enforced)"
                            : "1 checked,"),
              std::string::npos)
        << line;
  }
}

TEST(LedgerOutput, AFailedWriteIsReported) {
  core::Ledger ledger = unitLedger();
  const LedgerOutputOptions options{.path = "/nonexistent/weavec-unit-test/",
                                    .summary = true};
  std::string line;
  llvm::raw_string_ostream summary(line);
  std::string error;
  EXPECT_FALSE(emitUnitLedger(ledger, UnitIdentity{.source = "a.c"},
                              core::LedgerConfig{}, options, summary, &error));
  EXPECT_NE(error.find("/nonexistent/weavec-unit-test"), std::string::npos)
      << error;
  // The summary line is printed all the same.
  EXPECT_NE(line.find("weavec: a.c: 1 site"), std::string::npos);
}

TEST(LedgerOutput, WritesTheProgramLedgerForTheOutput) {
  const ScratchDirectory scratch;
  core::Ledger ledger = unitLedger();
  ledger.units.front().source = scratch / "a.c";
  ledger.assumptions = core::Assumptions{};
  ledger.assumptions->a3.inputsWithoutRecords = {"libz.a"};
  const LedgerOutputOptions options{.path = scratch.str() + "/"};
  std::string line;
  llvm::raw_string_ostream summary(line);
  ASSERT_TRUE(emitProgramLedger(ledger, scratch / "bin/prog", scratch.str(),
                                core::LedgerConfig{}, options, summary));
  EXPECT_EQ(line,
            "weavec: program prog: 1 site in 1 unit: 0 proven, 1 checkable "
            "(not enforced), 0 unresolved, 0 trusted; 0 errors, 0 warnings; 1 "
            "input without a WeaveC record (libz.a); unverified: 0 exported "
            "requirements (A1), 0 header invariants (A3)\n");
  const std::optional<llvm::json::Value> json =
      readJson(scratch / "prog.ledger.json");
  ASSERT_TRUE(json);
  EXPECT_EQ(json->getAsObject()->getString("scope"), "program");
}

TEST(LedgerOutput, AppliesTheWarningFlagsToTheDiagnostics) {
  core::Ledger ledger = unitLedger();
  const auto diagnostic = [](std::string_view id, core::Severity severity,
                             core::Certainty certainty, std::uint32_t line) {
    return core::LedgerDiagnostic{
        .id = std::string(id),
        .severity = severity,
        .certainty = certainty,
        .message = "m",
        .location = {.file = "a.c", .line = line, .column = 1, .opaque = 0}};
  };
  ledger.diagnostics = {
      diagnostic(core::diag::UseAfterFree, core::Severity::Warning,
                 core::Certainty::Possible, 1),
      diagnostic(core::diag::UseAfterFree, core::Severity::Error,
                 core::Certainty::Definite, 2),
      diagnostic(core::diag::Leak, core::Severity::Warning,
                 core::Certainty::Definite, 3),
  };
  core::FacetRecord &temporal =
      ledger.units.front().functions.front().sites.front().addFacet(
          Facet::Temporal);
  temporal.decide(
      FacetDecision::unresolvedFor(core::UnresolvedReason::MayReleased));
  temporal.diagnostic = 0;
  core::FacetRecord &null =
      *ledger.units.front().functions.front().sites.front().facet(Facet::Null);
  null.diagnostic = 1;

  DiagnosticControl control;
  std::string error;
  ASSERT_TRUE(control.parse("-Wno-weavec-use-after-free", error));
  ASSERT_TRUE(control.parse("-Werror=weavec-leak", error));
  applyDiagnosticControl(ledger, control);
  ASSERT_EQ(ledger.diagnostics.size(), 2U);
  EXPECT_EQ(ledger.diagnostics[0].certainty, core::Certainty::Definite);
  EXPECT_EQ(ledger.diagnostics[0].severity, core::Severity::Error);
  EXPECT_EQ(ledger.diagnostics[1].id, core::diag::Leak);
  EXPECT_EQ(ledger.diagnostics[1].severity, core::Severity::Error);
  // The dropped diagnostic's facet loses its link; the kept one follows.
  EXPECT_EQ(temporal.diagnostic, std::nullopt);
  EXPECT_EQ(null.diagnostic, 0U);
}

TEST(LedgerOutput, SummaryNamesAreRelativeToTheJob) {
  EXPECT_EQ(summaryName("src/a.c", "/proj"), "src/a.c");
  EXPECT_EQ(summaryName("../a.c", "/proj/build"), "../a.c");
  EXPECT_EQ(summaryName("/proj/src/a.c", "/proj"), "src/a.c");
  EXPECT_EQ(summaryName("/elsewhere/a.c", "/proj"), "/elsewhere/a.c");
}

namespace {
/// Calls the consumer hook at the end of the unit, as the WeaveC consumer
/// does once the unit pipeline produced the ledger.
class HookAction final : public clang::ASTFrontendAction {
public:
  explicit HookAction(FrontendOptions opts) : options(std::move(opts)) {}

protected:
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &compiler,
                    llvm::StringRef /*inFile*/) override {
    class Consumer final : public clang::ASTConsumer {
    public:
      Consumer(clang::CompilerInstance &compiler,
               const FrontendOptions &options)
          : compiler(compiler), options(options) {}
      void HandleTranslationUnit(clang::ASTContext & /*context*/) override {
        core::Ledger ledger = unitLedger();
        emitUnitLedger(ledger, compiler, options);
      }

    private:
      clang::CompilerInstance &compiler;
      const FrontendOptions &options;
    };
    return std::make_unique<Consumer>(compiler, options);
  }

private:
  FrontendOptions options;
};
} // namespace

TEST(LedgerOutput, TheConsumerHookTakesTheUnitFromTheCompiler) {
  const ScratchDirectory scratch;
  FrontendOptions options;
  options.ledgerOutput.path = scratch.str() + "/";
  options.ledgerOutput.summary = false;
  ASSERT_TRUE(clang::tooling::runToolOnCodeWithArgs(
      std::make_unique<HookAction>(options), "int x;\n",
      {"-target", "x86_64-unknown-linux-gnu"}, "unit.c"));
  const std::optional<llvm::json::Value> json =
      readJson(scratch / "unit.c.ledger.json");
  ASSERT_TRUE(json);
  const llvm::json::Object &unit =
      *json->getAsObject()->getArray("units")->front().getAsObject();
  EXPECT_EQ(unit.getString("target"), "x86_64-unknown-linux-gnu");
  EXPECT_TRUE(unit.getString("source")->ends_with("unit.c"));

  // A silent run (a fixpoint round) writes nothing.
  options.silent = true;
  options.ledgerOutput.path = scratch.str() + "/silent/";
  ASSERT_TRUE(clang::tooling::runToolOnCodeWithArgs(
      std::make_unique<HookAction>(options), "int x;\n", {}, "unit.c"));
  EXPECT_FALSE(llvm::sys::fs::exists(scratch / "silent"));
}

TEST(LedgerOutput, TheRetainedUnitHookTakesTheUnitFromTheAST) {
  const ScratchDirectory scratch;
  const std::unique_ptr<clang::ASTUnit> ast =
      clang::tooling::buildASTFromCodeWithArgs(
          "int x;\n", {"-target", "aarch64-unknown-linux-gnu"}, "retained.c");
  ASSERT_TRUE(ast);
  FrontendOptions options;
  options.ledgerOutput.path = scratch.str() + "/";
  options.ledgerOutput.summary = false;
  core::Ledger ledger = unitLedger();
  ASSERT_TRUE(emitUnitLedger(ledger, *ast, options));
  const std::optional<llvm::json::Value> json =
      readJson(scratch / "retained.c.ledger.json");
  ASSERT_TRUE(json);
  const llvm::json::Object &unit =
      *json->getAsObject()->getArray("units")->front().getAsObject();
  EXPECT_EQ(unit.getString("target"), "aarch64-unknown-linux-gnu");
  ASSERT_NE(unit.get("object"), nullptr);
  EXPECT_EQ(unit.get("object")->kind(), llvm::json::Value::Null);
}

} // namespace weavec::frontend
