//===- RetainedUnitTest.cpp - Tests for retained-unit reporting -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/AnalysisStats.h"
#include "weavec/Frontend/FrontendAction.h"

#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Tooling/Tooling.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <gtest/gtest.h>

#include <string>

namespace weavec::frontend {
namespace {

constexpr const char *DoubleFree = R"c(
typedef unsigned long size_t;
void *malloc(size_t);
void free(void *);
void twice(void) {
  char *p = malloc(4);
  free(p);
  free(p);
}
void again(void) {
  char *q = malloc(4);
  free(q);
  free(q);
}
)c";

// RFC 0020: a retained unit buffers Clang's rendering of each diagnostic in
// a private stream. The text must be exactly what Clang's own printer writes.
TEST(RetainedUnit, BufferedDiagnosticsMatchTheDirectPrinter) {
  auto ast = clang::tooling::buildASTFromCode(DoubleFree, "retained.c");
  ASSERT_TRUE(ast);
  auto &diagnostics = ast->getDiagnostics();
  auto *previous = diagnostics.getClient();
  auto owned = diagnostics.takeClient();
  const FrontendOptions options;

  std::string expected;
  llvm::raw_string_ostream output(expected);
  clang::TextDiagnosticPrinter reference(output,
                                         diagnostics.getDiagnosticOptions());
  diagnostics.setClient(&reference, false);
  diagnostics.Reset(true);
  reference.BeginSourceFile(ast->getLangOpts(), &ast->getPreprocessor());
  const auto direct =
      analyzeTranslationUnit(ast->getASTContext(), diagnostics, options);
  reference.EndSourceFile();
  ASSERT_EQ(direct.errors, 2U);
  output << "2 errors generated.\nafter diagnostics\n";

  testing::internal::CaptureStderr();
  const auto retained = analyzeRetainedUnit(*ast, options);
  llvm::errs() << "after diagnostics\n";
  const auto text = testing::internal::GetCapturedStderr();
  EXPECT_EQ(text, expected);
  EXPECT_EQ(retained.errors, 2U);
  EXPECT_EQ(diagnostics.getClient(), &reference);
  const bool owns = static_cast<bool>(owned);
  diagnostics.setClient(owns ? owned.release() : previous, owns);
}

TEST(AnalysisStats, StatisticsDistinguishProgressFromFinalSnapshots) {
  llvm::SmallString<256> directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("weavec-stats-test", directory));
  llvm::SmallString<256> path(directory);
  llvm::sys::path::append(path, "stats.json");
  core::AnalysisStats stats;
  for (const bool final : {false, true}) {
    stats.add("function_analyses");
    ASSERT_TRUE(writeAnalysisStats(path.str(), &stats, final));
    const auto buffer = llvm::MemoryBuffer::getFile(path);
    ASSERT_TRUE(buffer);
    auto parsed = llvm::json::parse((*buffer)->getBuffer());
    ASSERT_TRUE(static_cast<bool>(parsed));
    ASSERT_NE(parsed->getAsObject(), nullptr);
    EXPECT_EQ(parsed->getAsObject()->getBoolean("final"), final);
    const auto *counters = parsed->getAsObject()->getObject("counters");
    ASSERT_NE(counters, nullptr);
    EXPECT_EQ(counters->getInteger("function_analyses"), final ? 2 : 1);
  }
  EXPECT_FALSE(llvm::sys::fs::remove_directories(directory));
}

} // namespace
} // namespace weavec::frontend
