//===- ProgramChecksTest.cpp - Tests for the whole program's checks -------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/ProgramChecks.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace weavec::frontend {
namespace {

using core::SlotKey;

core::SourceLocation at(std::string file, std::uint32_t line,
                        std::uint32_t column) {
  return core::SourceLocation{
      .file = std::move(file), .line = line, .column = column, .opaque = 0};
}

ProgramMember member(std::string source) {
  ProgramMember unit;
  unit.source = std::move(source);
  unit.cwd = "/work";
  unit.exports.source = unit.source;
  return unit;
}

/// `name` defined in `unit` with external linkage and `summary`.
void define(ProgramMember &unit, const std::string &name,
            core::FunctionEffects summary, FunctionInterface facts = {}) {
  unit.exports.functions[name].effects = std::move(summary);
  unit.facts.functions[name] = std::move(facts);
}

core::FunctionEffects freesParam(std::uint32_t param) {
  core::FunctionEffects summary;
  summary.effects.push_back(
      core::PathEffect{.kind = core::PathEffect::Kind::Release,
                       .path = core::SummaryPath::param(param).deref(),
                       .family = "free"});
  return summary;
}

} // namespace

TEST(ProgramChecks, DisplayPathsAreRelativeToTheWorkingDirectoryWhenInside) {
  EXPECT_EQ(displayPath("src/a.c", "/work", "/work"), "src/a.c");
  EXPECT_EQ(displayPath("/work/src/../a.c", "/elsewhere", "/work"), "a.c");
  EXPECT_EQ(displayPath("a.c", "/other", "/work"), "/other/a.c");
  EXPECT_EQ(displayPath("", "/work", "/work"), "");
}

TEST(ProgramChecks, SlotsAreSolvedAcrossUnits) {
  // Lua's shape: `lstate.c` stores `new_state`'s parameter into a field and
  // calls through it; `lua.c` passes its own `l_alloc`.
  ProgramMember state = member("state.c");
  const SlotKey field = SlotKey::field("struct global_state", "frealloc");
  state.facts.slots = SlotFacts{
      .rows = {core::SlotRow{.slot = field,
                             .targets = {},
                             .sources = {SlotKey::param("new_state", 0)},
                             .open = std::nullopt}},
      .unit = "state.c",
      .defined = {"new_state", "state_release"},
      .exported = {"new_state", "state_release"},
      .confinedRecords = {},
      .escapedStatics = {}};
  ProgramMember main = member("main.c");
  main.facts.slots =
      SlotFacts{.rows = {core::SlotRow{.slot = SlotKey::param("new_state", 0),
                                       .targets = {"main.c:l_alloc"},
                                       .sources = {},
                                       .open = std::nullopt}},
                .unit = "main.c",
                .defined = {"main", "main.c:l_alloc"},
                .exported = {"main"},
                .confinedRecords = {},
                .escapedStatics = {}};
  const std::vector<ProgramMember> members{state, main};
  const core::SlotSolution closed = solveProgramSlots(members, true);
  EXPECT_EQ(closed.targets(field), std::set<std::string>{"main.c:l_alloc"});
  EXPECT_FALSE(closed.isOpen(field));
  EXPECT_EQ(closed.resolveCall(field).kind,
            core::IndirectCallKind::ClosedSingle);

  // Code a library's users run may store into the field too.
  const core::SlotSolution open = solveProgramSlots(members, false);
  EXPECT_TRUE(open.isOpen(field));
  EXPECT_EQ(open.resolveCall(field).kind, core::IndirectCallKind::OpenKnown);

  std::string text;
  llvm::raw_string_ostream os(text);
  dumpProgramSlots(closed, os);
  EXPECT_NE(text.find("field struct global_state frealloc: {main.c:l_alloc} "
                      "closed"),
            std::string::npos)
      << text;
}

TEST(ProgramChecks, BoundaryRowsCarryTheirUnit) {
  ProgramMember a = member("a.c");
  a.facts.boundaries = {
      analysis::BoundaryRow{.unit = {},
                            .function = "remember",
                            .site = 2,
                            .reason = core::UnresolvedReason::DanglingEscape,
                            .placeClass = "g_cache"}};
  const std::vector<analysis::BoundaryRow> rows =
      programBoundaries(std::vector<ProgramMember>{a});
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0].unit, "a.c");
  EXPECT_EQ(rows[0].placeClass, "g_cache");
}

TEST(ProgramChecks, ALyingDeclarationIsAnErrorAtTheDeclaration) {
  // Probe 38: the caller declares `inspect` WEAVEC_BORROWED; its definition
  // frees the parameter.
  ProgramMember caller = member("main.c");
  caller.exports.imports = {"inspect"};
  caller.facts.imports["inspect"] = ImportInterface{
      .declared = DeclaredInterface{.params = {DeclaredParam{
                                        .name = "p",
                                        .kind = std::nullopt,
                                        .ownership = "WEAVEC_BORROWED"}},
                                    .result = std::nullopt,
                                    .ownership = std::nullopt},
      .location = at("main.c", 6, 6)};
  ProgramMember callee = member("impl.c");
  define(callee, "inspect", freesParam(0),
         FunctionInterface{.params = {"default single nullable"},
                           .location = at("impl.c", 4, 6)});
  const std::vector<ProgramMember> members{caller, callee};
  const std::vector<core::Diagnostic> diagnostics =
      verifyDeclarations(members, "/work");
  ASSERT_EQ(diagnostics.size(), 1U);
  const core::Diagnostic &error = diagnostics.front();
  EXPECT_EQ(error.id, core::diag::AnnotationMismatch);
  EXPECT_EQ(error.severity, core::Severity::Error);
  EXPECT_EQ(error.certainty, core::Certainty::Definite);
  EXPECT_EQ(error.message, "'inspect' is declared WEAVEC_BORROWED here but "
                           "its definition in 'impl.c' frees 'p'");
  EXPECT_EQ(error.location, at("/work/main.c", 6, 6));
  ASSERT_EQ(error.notes.size(), 1U);
  EXPECT_EQ(error.notes.front().message, "defined here");
  EXPECT_EQ(error.notes.front().location, at("/work/impl.c", 4, 6));

  // A definition that keeps its word is not reported.
  std::vector<ProgramMember> honest{caller, member("impl.c")};
  define(honest[1], "inspect", core::FunctionEffects{});
  EXPECT_TRUE(verifyDeclarations(honest, "/work").empty());
  // Nor is a declaration no other unit defines.
  EXPECT_TRUE(
      verifyDeclarations(std::vector<ProgramMember>{caller}, "/work").empty());
}

TEST(ProgramChecks, ResultContractsAreVerified) {
  ProgramMember caller = member("main.c");
  caller.facts.imports["get"] = ImportInterface{
      .declared = DeclaredInterface{.params = {},
                                    .result = "unknown nonnull",
                                    .ownership = "WEAVEC_OWNED"},
      .location = at("main.c", 2, 7)};
  ProgramMember callee = member("get.c");
  core::FunctionEffects summary;
  summary.results.push_back(core::ResultEffect{
      .value = core::ValueDesc{.kind = core::ValueDesc::Kind::Path,
                               .path = core::SummaryPath::param(0)},
      .classes = {core::ResultClass::NonNull}});
  summary.results.push_back(core::ResultEffect{
      .value = core::ValueDesc{.kind = core::ValueDesc::Kind::Null},
      .classes = {core::ResultClass::Null}});
  define(callee, "get", summary);
  const std::vector<core::Diagnostic> diagnostics =
      verifyDeclarations(std::vector<ProgramMember>{caller, callee}, "/work");
  ASSERT_EQ(diagnostics.size(), 1U + 1U);
  EXPECT_EQ(diagnostics[0].message,
            "'get' is declared WEAVEC_OWNED here but its definition in "
            "'get.c' returns a borrowed pointer");
  EXPECT_EQ(diagnostics[1].message,
            "'get' is declared to return unknown nonnull here but its "
            "definition in 'get.c' may return null");
}

} // namespace weavec::frontend
