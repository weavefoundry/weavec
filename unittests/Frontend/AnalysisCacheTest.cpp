//===- AnalysisCacheTest.cpp - Checkpoint validation (RFC 0020) -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Frontend/AnalysisCache.h"

#include "weavec/Analysis/ClangLocation.h"
#include "weavec/Frontend/AnalysisStats.h"
#include "weavec/Frontend/CheckedArtifacts.h"
#include "weavec/Frontend/ProgramAnalysis.h"

#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Tooling/CompilationDatabase.h"

#include "llvm/Support/Compression.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <gtest/gtest.h>

namespace weavec::frontend {
namespace {

class AnalysisCacheTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("weavec-cache-test", directory));
  }
  void TearDown() override {
    EXPECT_FALSE(llvm::sys::fs::remove_directories(directory));
  }
  std::string path() const {
    llvm::SmallString<256> result(directory);
    llvm::sys::path::append(result, key + ".wcache");
    return result.str().str();
  }
  static AnalysisCheckpoint checkpoint() {
    AnalysisCheckpoint result;
    result.importedIdentity = checkedDigest("imports");
    UnitResult unit;
    unit.exports.source = "cache-input.c";
    // A widened real-project offset must survive the ordinary summary
    // transport used inside a checkpoint (RFC 0020).
    core::FunctionSummary generic;
    generic.addReturn(
        core::ValueSource::freshAt("free", core::PointerOffset::inside(),
                                   core::PathAffine::ofConstant(24)));
    unit.exports.functions["get"].summary.assign(std::move(generic));
    auto &contract = unit.exports.checkedDefinitions["main"];
    contract.computed = contract.selected = true;
    contract.obligations.add({.property = core::SafetyProperty::Initialization,
                              .outcome = core::SafetyOutcome::Unresolved,
                              .location = {.file = unit.exports.source,
                                           .line = 4,
                                           .column = 2,
                                           .opaque = 0},
                              .function = "main",
                              .subject = "x",
                              .reason = "not written",
                              .calls = {}});
    unit.dependencies = {"missing", "@counts"};
    unit.diagnostics.push_back({.severity = core::Severity::Error,
                                .id = core::diag::CheckingIncomplete,
                                .message = "not established",
                                .location = {.file = unit.exports.source,
                                             .line = 4,
                                             .column = 2,
                                             .opaque = 0},
                                .notes = {},
                                .fixits = {}});
    unit.diagnostics.back().addNote(
        "origin",
        {.file = unit.exports.source, .line = 3, .column = 1, .opaque = 0});
    unit.diagnostics.back().addFixIt(unit.diagnostics.back().location, " = 0");
    result.units.push_back(std::move(unit));
    return result;
  }
  llvm::SmallString<256> directory;
  const std::string key = checkedDigest("unit inputs");
};

TEST_F(AnalysisCacheTest, RoundTripPreservesFailureNotesAndOwnedDiagnosticIds) {
  const auto before = checkpoint();
  ASSERT_TRUE(writeAnalysisCheckpoint(directory.str(), key, before, nullptr));
  const auto after = readAnalysisCheckpoint(directory.str(), key, nullptr);
  ASSERT_TRUE(after);
  ASSERT_EQ(after->units.size(), 1U);
  EXPECT_EQ(after->units[0].dependencies, before.units[0].dependencies);
  EXPECT_EQ(after->units[0].exports.checkedDefinitions,
            before.units[0].exports.checkedDefinitions);
  ASSERT_EQ(after->units[0].diagnostics.size(), 1U);
  const auto &diagnostic = after->units[0].diagnostics[0];
  // Parsing has destroyed its JSON buffer. The id must still name valid bytes;
  // C++ does not require distinct occurrences of a string literal to coalesce.
  EXPECT_EQ(diagnostic.id, core::diag::CheckingIncomplete);
  ASSERT_EQ(diagnostic.notes.size(), 1U);
  EXPECT_EQ(diagnostic.notes[0].message, "origin");
  ASSERT_EQ(diagnostic.fixits.size(), 1U);
  EXPECT_EQ(diagnostic.fixits[0].insertion, " = 0");
  EXPECT_TRUE(CheckedReport::failed(after->units[0].exports));
}
TEST_F(AnalysisCacheTest, SharedLedgersPreserveOversizedExpandedContracts) {
  auto before = checkpoint();
  auto &unit = before.units.front().exports;
  auto &contract = unit.checkedDefinitions.at("main");
  contract.obligations = {};
  const std::string reason(4096, 'x');
  for (unsigned i = 0; i < core::MaxSafetyObligations; ++i)
    contract.obligations.add(
        {.property = core::SafetyProperty::Initialization,
         .outcome = core::SafetyOutcome::Unresolved,
         .location =
             {.file = unit.source, .line = i + 1, .column = 2, .opaque = 0},
         .function = "main",
         .subject = "byte",
         .reason = reason,
         .calls = {
             {.file = unit.source, .line = i + 2, .column = 3, .opaque = 0}}});
  contract.obligations.markLimited();
  unit.checkedDefinitions["alias"] = contract;
  auto &function = unit.functions.at("get");
  auto generic = function.summary.get();
  generic.checked = contract;
  function.summary.assign(std::move(generic));
  core::CallbackBindings callback;
  callback[core::SummaryPath::param(0)] = core::CallTargets::any();
  function.specializations[callback].assign(function.summary.get());
  core::CallContext memory;
  memory.facts[core::SummaryPath::param(0)] =
      core::ValueFact::of(core::Outcome::NonNull);
  function.memorySpecializations[memory].assign(function.summary.get());
  core::AnalysisStats stats;
  ASSERT_TRUE(writeAnalysisCheckpoint(directory.str(), key, before, &stats));
  EXPECT_LT(stats.count("cache_uncompressed_bytes"), 1024U * 1024U);
  const auto after = readAnalysisCheckpoint(directory.str(), key, nullptr);
  ASSERT_TRUE(after);
  const auto &decoded = after->units.front().exports;
  EXPECT_EQ(decoded.functions, unit.functions);
  EXPECT_EQ(decoded.checkedDefinitions, unit.checkedDefinitions);
  for (const auto &[name, value] : decoded.checkedDefinitions)
    EXPECT_TRUE(value.obligations.sameExplanationsAs(
        unit.checkedDefinitions.at(name).obligations));
  const auto &decodedFunction = decoded.functions.at("get");
  for (const auto *summary :
       {&decodedFunction.summary.get(),
        &decodedFunction.specializations.at(callback).get(),
        &decodedFunction.memorySpecializations.at(memory).get()})
    EXPECT_TRUE(
        summary->checked.obligations.sameExplanationsAs(contract.obligations));
  auto changed = unit;
  changed.checkedDefinitions.at("main").obligations.add(
      {.property = core::SafetyProperty::Initialization,
       .outcome = core::SafetyOutcome::Violation,
       .location = {.file = unit.source, .line = 1, .column = 2, .opaque = 0},
       .function = "main",
       .subject = "byte",
       .reason = "changed proof",
       .calls = {}});
  EXPECT_NE(checkpointExportsIdentity(unit),
            checkpointExportsIdentity(changed));
}

TEST_F(AnalysisCacheTest, ProducerValidationPreservesTheGlobalNameTable) {
  auto before = checkpoint();
  auto &unit = before.units.front().exports;
  (void)unit.globals.idFor("unused");
  const auto global = unit.globals.idFor("counter");
  auto generic = unit.functions.at("get").summary.get();
  generic.addEffect(core::SummaryPath::global(global), {.read = true});
  unit.functions.at("get").summary.assign(std::move(generic));
  ASSERT_TRUE(writeAnalysisCheckpoint(directory.str(), key, before, nullptr));
  const auto after = readAnalysisCheckpoint(directory.str(), key, nullptr);
  ASSERT_TRUE(after);
  const auto &decoded = after->units.front().exports;
  const auto mapped = decoded.globals.find("counter");
  ASSERT_TRUE(mapped);
  // RFC 0022 retains the producer's names before any first-use references.
  EXPECT_EQ(*mapped, global);
  EXPECT_EQ(decoded.globals, unit.globals);
  EXPECT_TRUE(decoded.functions.at("get")
                  .summary.get()
                  .effects.at(core::SummaryPath::global(*mapped))
                  .read);
  EXPECT_EQ(checkpointExportsIdentity(unit),
            checkpointExportsIdentity(decoded));
}

TEST_F(AnalysisCacheTest, LossyMetadataNeverGetsAReusableIdentity) {
  auto before = checkpoint();
  // Core's bounded metadata wire cannot carry this signature. It must not
  // collapse distinct imported facts to the fallback limited contract.
  before.units.front().exports.checkedDefinitions.at("main").signature =
      std::string(65537, 's');
  EXPECT_TRUE(checkpointExportsIdentity(before.units.front().exports).empty());
  core::AnalysisStats stats;
  EXPECT_FALSE(writeAnalysisCheckpoint(directory.str(), key, before, &stats));
  EXPECT_EQ(stats.count("cache_write_failures"), 1U);
  EXPECT_FALSE(readAnalysisCheckpoint(directory.str(), key, nullptr));
}

TEST_F(AnalysisCacheTest, AccumulatedRequestsRetainLosslessCheckpoints) {
  auto before = checkpoint();
  auto &unit = before.units.front().exports;
  // RFC 0028: Lua's callers request 40 callback contexts. The request union
  // remains relevant even when the analyzer can compute only 32 results.
  for (unsigned i = 0; i < 40; ++i) {
    const core::CallbackBindings bindings{
        {core::SummaryPath::param(0),
         core::CallTargets::function("target" + std::to_string(i))}};
    unit.callbackRequests["invoke"].insert(bindings);
    core::CallContext input;
    input.facts[core::SummaryPath::param(0)] = core::ValueFact::ofConstant(i);
    unit.memoryRequests["invoke"].insert(input);
  }
  const auto identity = checkpointExportsIdentity(unit);
  ASSERT_FALSE(identity.empty());
  core::AnalysisStats stats;
  ASSERT_TRUE(writeAnalysisCheckpoint(directory.str(), key, before, &stats));
  EXPECT_EQ(stats.count("cache_writes"), 1U);
  EXPECT_EQ(stats.count("cache_write_failures"), 0U);
  const auto after = readAnalysisCheckpoint(directory.str(), key, &stats);
  ASSERT_TRUE(after);
  const auto &decoded = after->units.front().exports;
  EXPECT_EQ(decoded.callbackRequests, unit.callbackRequests);
  EXPECT_EQ(decoded.memoryRequests, unit.memoryRequests);
  EXPECT_EQ(decoded.functions, unit.functions);
  EXPECT_EQ(decoded.checkedDefinitions, unit.checkedDefinitions);
  EXPECT_EQ(checkpointExportsIdentity(decoded), identity);
}

TEST_F(AnalysisCacheTest, StreamedDiagnosticsPreserveNestedTextAndFixIts) {
  auto before = checkpoint();
  std::string text = std::string(65536, 'x') + "é😀\"\\";
  for (unsigned byte = 0; byte < 32; ++byte)
    text += static_cast<char>(byte);
  auto &diagnostic = before.units.front().diagnostics.front();
  diagnostic.message = text;
  diagnostic.location.file = text;
  diagnostic.notes.front().message = text;
  diagnostic.notes.front().addNote(text, diagnostic.location);
  diagnostic.fixits.front().insertion = text;
  ASSERT_TRUE(writeAnalysisCheckpoint(directory.str(), key, before, nullptr));
  const auto after = readAnalysisCheckpoint(directory.str(), key, nullptr);
  ASSERT_TRUE(after);
  const auto &decoded = after->units.front().diagnostics.front();
  EXPECT_EQ(decoded.message, text);
  EXPECT_EQ(decoded.location.file, text);
  ASSERT_EQ(decoded.notes.size(), 1U);
  EXPECT_EQ(decoded.notes.front().message, text);
  ASSERT_EQ(decoded.notes.front().notes.size(), 1U);
  EXPECT_EQ(decoded.notes.front().notes.front().message, text);
  ASSERT_EQ(decoded.fixits.size(), 1U);
  EXPECT_EQ(decoded.fixits.front().insertion, text);
  EXPECT_EQ(decoded.id, diagnostic.id);
  EXPECT_EQ(decoded.severity, diagnostic.severity);
}

TEST_F(AnalysisCacheTest, ImportedNamesAndTypeSpellingsHaveLosslessKeys) {
  analysis::UnitExports unit;
  unit.source = "directory with spaces/callback.c";
  auto &callback = unit.functions["callback"];
  callback.addressTaken = true;
  callback.typeKey = "char *(const char *)";
  core::FunctionSummary generic;
  generic.addEffect(core::SummaryPath::param(0), {.read = true});
  callback.summary.assign(std::move(generic));
  unit.functions["local"] = callback;
  unit.functions["local"].external = false;
  const std::set<std::string> dependencies{"callback", unit.source + "#local",
                                           "missing symbol"};
  analysis::ProgramDatabase database;
  database.add(unit);
  const auto identity =
      checkpointExportsIdentity(database.checkpointInputs(dependencies));
  ASSERT_EQ(identity.size(), 64U);
  auto changed = unit;
  auto modified = changed.functions.at("callback").summary.get();
  modified.addEffect(core::SummaryPath::param(0), {.written = true});
  changed.functions.at("callback").summary.assign(std::move(modified));
  analysis::ProgramDatabase changedDatabase;
  changedDatabase.add(changed);
  EXPECT_NE(identity, checkpointExportsIdentity(
                          changedDatabase.checkpointInputs(dependencies)));
  changed = unit;
  changed.functions.at("callback").typeKey = "char *(char *)";
  analysis::ProgramDatabase changedType;
  changedType.add(changed);
  EXPECT_NE(identity, checkpointExportsIdentity(
                          changedType.checkpointInputs(dependencies)));
}

TEST_F(AnalysisCacheTest, IndependentUnitReusesImportedPointerReturnType) {
  const auto callback = directory.str().str() + "/callback.c";
  const auto empty = directory.str().str() + "/empty.c";
  ASSERT_TRUE(writeAtomicText(
      callback, "char *identity(const char *p){return (char *)p;}\n"
                "char *(*selected)(const char *) = identity;\n"));
  ASSERT_TRUE(writeAtomicText(empty, "int unused;\n"));
  clang::tooling::FixedCompilationDatabase commands(directory.str(),
                                                    {"-xc", "-std=c11"});
  const auto run = [&](core::AnalysisStats &stats) {
    FrontendOptions options;
    options.analysis.checked = true;
    options.analysis.stats = &stats;
    options.analysisCache = directory.str().str();
    ProgramAnalysis program(options);
    program.addUnit(std::make_unique<CompilationDatabaseUnit>(
        commands, callback, std::vector<clang::tooling::ArgumentsAdjuster>{}));
    program.addUnit(std::make_unique<CompilationDatabaseUnit>(
        commands, empty, std::vector<clang::tooling::ArgumentsAdjuster>{}));
    return program.run();
  };
  core::AnalysisStats coldStats;
  const auto cold = run(coldStats);
  ASSERT_TRUE(cold.failed.empty());
  ASSERT_GT(coldStats.count("function_analyses"), 0U);
  EXPECT_EQ(coldStats.count("cache_writes"), 2U);
  core::AnalysisStats warmStats;
  const auto warm = run(warmStats);
  EXPECT_TRUE(warm.failed.empty());
  EXPECT_EQ(warm.errors, cold.errors);
  EXPECT_EQ(warm.warnings, cold.warnings);
  EXPECT_EQ(warmStats.count("cache_hits"), 2U);
  EXPECT_EQ(warmStats.count("function_analyses"), 0U);
}

TEST_F(AnalysisCacheTest, CheckpointPublicationPreservesExportsOnWriteFailure) {
  // RFC 0020: publication temporarily owns completed exports. Later runs
  // still need their imports to order units, even when the cache write fails.
  const auto producer = directory.str().str() + "/producer.c";
  const auto consumer = directory.str().str() + "/consumer.c";
  const auto blocked = directory.str().str() + "/blocked-cache";
  ASSERT_TRUE(writeAtomicText(producer, "int source_value(void){return 1;}\n"));
  ASSERT_TRUE(writeAtomicText(
      consumer, "int source_value(void);\n"
                "int read_value(void){return 42/source_value();}\n"));
  ASSERT_TRUE(writeAtomicText(blocked, "not a directory"));
  clang::tooling::FixedCompilationDatabase commands(directory.str(),
                                                    {"-xc", "-std=c11"});
  for (const auto &cache : {blocked, directory.str().str()}) {
    core::AnalysisStats stats;
    FrontendOptions options;
    options.analysis.checked = true;
    options.analysis.stats = &stats;
    options.analysisCache = cache;
    ProgramAnalysis program(options);
    // Deliberately put the consumer first: retained dependency information
    // must still make the producer available before checking its division.
    program.addUnit(std::make_unique<CompilationDatabaseUnit>(
        commands, consumer, std::vector<clang::tooling::ArgumentsAdjuster>{}));
    program.addUnit(std::make_unique<CompilationDatabaseUnit>(
        commands, producer, std::vector<clang::tooling::ArgumentsAdjuster>{}));
    ASSERT_TRUE(program.run().ok());
    ASSERT_NE(program.database().find("read_value"), nullptr);
    const auto before = *program.database().find("read_value");
    if (cache == blocked)
      EXPECT_GT(stats.count("cache_write_failures"), 0U);
    else
      EXPECT_GT(stats.count("cache_writes"), 0U);
    const auto analyses = stats.count("function_analyses");
    EXPECT_TRUE(program.run().ok());
    ASSERT_NE(program.database().find("read_value"), nullptr);
    EXPECT_TRUE(before == *program.database().find("read_value"));
    if (cache == blocked)
      EXPECT_GT(stats.count("function_analyses"), analyses);
  }
}

TEST_F(AnalysisCacheTest, BufferedRetainedDiagnosticsPreserveExactOutput) {
  auto ast = clang::tooling::buildASTFromCode("int value;\n", "diagnostic.c");
  ASSERT_TRUE(ast);
  auto &diagnostics = ast->getDiagnostics();
  auto *previous = diagnostics.getClient();
  auto owned = diagnostics.takeClient();
  const auto location = analysis::toCoreLocation(
      ast->getSourceManager(), ast->getSourceManager().getLocForStartOfFile(
                                   ast->getSourceManager().getMainFileID()));
  UnitResult result;
  result.diagnostics.push_back({.severity = core::Severity::Error,
                                .id = core::diag::CheckingIncomplete,
                                .message = std::string(20000, 'x') + " 100%",
                                .location = location,
                                .notes = {},
                                .fixits = {}});
  result.diagnostics.back().addNote("retained note", location);
  result.diagnostics.push_back({.severity = core::Severity::Error,
                                .id = core::diag::CheckingIncomplete,
                                .message = "locationless final error",
                                .location = {},
                                .notes = {},
                                .fixits = {}});
  FrontendOptions options;
  std::string expected;
  llvm::raw_string_ostream output(expected);
  clang::TextDiagnosticPrinter reference(output,
                                         diagnostics.getDiagnosticOptions());
  diagnostics.setClient(&reference, false);
  diagnostics.Reset(true);
  reference.BeginSourceFile(ast->getLangOpts(), &ast->getPreprocessor());
  (void)replayUnitResult(result, diagnostics, options);
  reference.EndSourceFile();
  output << "2 errors generated.\nafter diagnostics\n";

  testing::internal::CaptureStderr();
  const auto actual = analyzeRetainedUnit(*ast, options, &result);
  llvm::errs() << "after diagnostics\n";
  const auto text = testing::internal::GetCapturedStderr();
  EXPECT_EQ(text, expected);
  EXPECT_EQ(actual.errors, 2U);
  EXPECT_EQ(diagnostics.getClient(), &reference);
  const bool owns = static_cast<bool>(owned);
  diagnostics.setClient(owns ? owned.release() : previous, owns);
}

TEST_F(AnalysisCacheTest, InvalidSharedTablesNeverPublishAPartialUnit) {
  ASSERT_TRUE(
      writeAnalysisCheckpoint(directory.str(), key, checkpoint(), nullptr));
  const auto buffer = llvm::MemoryBuffer::getFile(path());
  ASSERT_TRUE(buffer);
  const auto original = (*buffer)->getBuffer().split('\n').second.str();
  for (unsigned mutation = 0; mutation < 8; ++mutation) {
    auto parsed = llvm::json::parse(original);
    ASSERT_TRUE(parsed);
    auto &object = *parsed->getAsObject();
    auto &unit = *object.getArray("units")->front().getAsObject();
    auto &references = *unit.getArray("ledgers");
    switch (mutation) {
    case 0:
      references.pop_back();
      break;
    case 1:
      references.push_back(0);
      break;
    case 2:
      references.front() = 999999;
      break;
    case 3:
      (*object.getArray("obligation_records")->front().getAsArray())[2] =
          999999;
      break;
    case 4: {
      const auto property =
          (*object.getArray("obligation_records")->front().getAsArray())[0]
              .getAsUINT64();
      ASSERT_TRUE(property);
      (*object.getArray("strings"))[*property] = "invented-property";
      break;
    }
    case 5: {
      auto &ledger = *object.getArray("ledgers")->front().getAsArray();
      ASSERT_GT(ledger.size(), 1U);
      ledger.push_back(ledger.back());
      break;
    }
    case 6:
      object.getArray("obligation_fields")->pop_back();
      break;
    case 7: {
      auto &path = *object.getArray("call_paths")->front().getAsArray();
      path.push_back(0);
      path.push_back(0);
      break;
    }
    default:
      FAIL() << "unknown checkpoint mutation";
    }
    std::string changed;
    llvm::raw_string_ostream out(changed);
    out << *parsed;
    ASSERT_TRUE(
        writeAtomicText(path(), checkedDigest(changed) + '\n' + changed));
    EXPECT_FALSE(readAnalysisCheckpoint(directory.str(), key, nullptr))
        << mutation;
  }
}

TEST_F(AnalysisCacheTest, RetainedAstNeverAcquiresANewerInputIdentity) {
  const auto source = directory.str().str() + "/retained.c";
  ASSERT_TRUE(writeAtomicText(source, "int value(void) { return 1; }\n"));
  clang::tooling::FixedCompilationDatabase commands(directory.str(),
                                                    {"-std=c17", "-x", "c"});
  FrontendOptions options;
  options.analysisCache = directory.str().str();
  options.discoverOnly = options.silent = true;
  CompilationDatabaseUnit retained(commands, source, {});
  ASSERT_TRUE(retained.analyze(options));
  // Ask for its identity only after the file changed: it must still bind
  // the older AST, rather than current filesystem bytes.
  ASSERT_TRUE(writeAtomicText(source, "int value(void) { return 2; }\n"));
  const auto oldIdentity = retained.inputIdentity(options);
  ASSERT_FALSE(oldIdentity.empty());
  CompilationDatabaseUnit current(commands, source, {});
  const auto newIdentity = current.inputIdentity(options);
  ASSERT_FALSE(newIdentity.empty());
  EXPECT_NE(oldIdentity, newIdentity);
  EXPECT_EQ(retained.inputIdentity(options), oldIdentity);
}

TEST_F(AnalysisCacheTest, ObservedInputChangesDuringParsingDisableReuse) {
  const auto source = directory.str().str() + "/changing.c";
  ASSERT_TRUE(writeAtomicText(source, "int value(void) { return 1; }\n"));
  clang::tooling::FixedCompilationDatabase commands(directory.str(),
                                                    {"-std=c17", "-x", "c"});
  unsigned invocations = 0;
  const clang::tooling::ArgumentsAdjuster editBeforeAst =
      [&](const clang::tooling::CommandLineArguments &arguments,
          llvm::StringRef) {
        if (++invocations == 2)
          EXPECT_TRUE(
              writeAtomicText(source, "int value(void) { return 2; }\n"));
        return arguments;
      };
  core::AnalysisStats stats;
  FrontendOptions options;
  options.analysis.stats = &stats;
  options.analysisCache = directory.str().str();
  options.discoverOnly = options.silent = true;
  CompilationDatabaseUnit unit(commands, source, {editBeforeAst});
  ASSERT_TRUE(unit.analyze(options));
  EXPECT_TRUE(unit.inputIdentity(options).empty());
  EXPECT_EQ(stats.count("cache_input_changes"), 1U);
}

TEST_F(AnalysisCacheTest, TruncationAndWrongChecksumNeverBecomeAHit) {
  ASSERT_TRUE(
      writeAnalysisCheckpoint(directory.str(), key, checkpoint(), nullptr));
  const auto buffer = llvm::MemoryBuffer::getFile(path());
  ASSERT_TRUE(buffer);
  const auto bytes = (*buffer)->getBuffer().str();
  for (const auto size :
       {0U, 63U, 65U, static_cast<unsigned>(bytes.size() - 1)}) {
    ASSERT_TRUE(writeAtomicText(path(), bytes.substr(0, size)));
    EXPECT_FALSE(readAnalysisCheckpoint(directory.str(), key, nullptr));
  }
  auto corrupt = bytes;
  corrupt.back() = corrupt.back() == 'x' ? 'y' : 'x';
  ASSERT_TRUE(writeAtomicText(path(), corrupt));
  core::AnalysisStats stats;
  EXPECT_FALSE(readAnalysisCheckpoint(directory.str(), key, &stats));
  EXPECT_EQ(stats.count("cache_invalid_records"), 1U);
}

TEST_F(AnalysisCacheTest,
       IncompatibleVersionAndUnknownIdsAreMissesEvenWithDigest) {
  ASSERT_TRUE(
      writeAnalysisCheckpoint(directory.str(), key, checkpoint(), nullptr));
  const auto buffer = llvm::MemoryBuffer::getFile(path());
  ASSERT_TRUE(buffer);
  const auto original = (*buffer)->getBuffer().split('\n').second.str();
  for (const auto &[from, to] :
       std::vector<std::pair<std::string, std::string>>{
           {"\"version\":3", "\"version\":999"},
           {R"("id":"checking-incomplete")", R"("id":"invented")"}}) {
    auto changed = original;
    const auto at = changed.find(from);
    ASSERT_NE(at, std::string::npos);
    changed.replace(at, from.size(), to);
    ASSERT_TRUE(
        writeAtomicText(path(), checkedDigest(changed) + '\n' + changed));
    EXPECT_FALSE(readAnalysisCheckpoint(directory.str(), key, nullptr));
  }
  EXPECT_FALSE(
      readAnalysisCheckpoint(directory.str(), "../../unsafe", nullptr));
  EXPECT_FALSE(writeAnalysisCheckpoint(directory.str(), "../../unsafe",
                                       checkpoint(), nullptr));
}

TEST_F(AnalysisCacheTest, ReadFailureAndDirectoryFailureAreLostOptimizations) {
  EXPECT_FALSE(readAnalysisCheckpoint(directory.str(), key, nullptr));
  ASSERT_TRUE(writeAtomicText(path(), "a file, not a directory"));
  core::AnalysisStats stats;
  EXPECT_FALSE(writeAnalysisCheckpoint(path(), key, checkpoint(), &stats));
  EXPECT_EQ(stats.count("cache_write_failures"), 1U);
}

TEST_F(AnalysisCacheTest, CompressedRecordsValidateBoundsAndLogicalChecksum) {
  if (!llvm::compression::zstd::isAvailable())
    GTEST_SKIP() << "LLVM has no zstd support";
  auto before = checkpoint();
  auto &diagnostics = before.units[0].diagnostics;
  diagnostics.resize(32, diagnostics.front());
  for (auto &diagnostic : diagnostics)
    diagnostic.message = std::string(65536, 'x');
  core::AnalysisStats stats;
  ASSERT_TRUE(writeAnalysisCheckpoint(directory.str(), key, before, &stats));
  EXPECT_LT(stats.count("cache_bytes_written") * 10,
            stats.count("cache_uncompressed_bytes"));
  auto buffer = llvm::MemoryBuffer::getFile(path());
  ASSERT_TRUE(buffer);
  const auto bytes = (*buffer)->getBuffer().str();
  ASSERT_NE(bytes.find("\nzstd-v1 "), std::string::npos);
  const auto after = readAnalysisCheckpoint(directory.str(), key, nullptr);
  ASSERT_TRUE(after);
  ASSERT_EQ(after->units[0].diagnostics.size(), diagnostics.size());
  EXPECT_EQ(after->units[0].diagnostics.back().message,
            diagnostics.back().message);
  for (const auto &bad : std::vector<std::string>{
           bytes.substr(0, bytes.size() - 3),
           std::string(64, '0') + bytes.substr(64),
           bytes.substr(0, 65) + "zstd-v1 4294967297\ninvalid",
           bytes.substr(0, 65) + "zstd-v1 0\ninvalid",
           bytes.substr(0, 65) + "zstd-v1 not-a-size\ninvalid"}) {
    ASSERT_TRUE(writeAtomicText(path(), bad));
    EXPECT_FALSE(readAnalysisCheckpoint(directory.str(), key, nullptr));
  }
}

TEST_F(AnalysisCacheTest, StatisticsDistinguishProgressFromFinalSnapshots) {
  core::AnalysisStats stats;
  for (const bool final : {false, true}) {
    stats.add("function_analyses");
    ASSERT_TRUE(writeAnalysisStats(path(), &stats, final));
    const auto buffer = llvm::MemoryBuffer::getFile(path());
    ASSERT_TRUE(buffer);
    auto parsed = llvm::json::parse((*buffer)->getBuffer());
    ASSERT_TRUE(parsed);
    ASSERT_NE(parsed->getAsObject(), nullptr);
    EXPECT_EQ(parsed->getAsObject()->getBoolean("final"), final);
    const auto *counters = parsed->getAsObject()->getObject("counters");
    ASSERT_NE(counters, nullptr);
    EXPECT_EQ(counters->getInteger("function_analyses"), final ? 2 : 1);
  }
}

} // namespace
} // namespace weavec::frontend
