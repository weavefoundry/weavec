//===- ProgramAnalysisTest.cpp - Tests for the whole-program driver -------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/ProgramAnalysis.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Tooling/Tooling.h"

#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace weavec::frontend {
namespace {

constexpr const char *Prelude = R"c(
typedef unsigned long size_t;
void *malloc(size_t);
void free(void *);
int cond(void);
#line 1
)c";

/// Records `file:line: message` for every warning and error, across units.
class Recorder final : public clang::DiagnosticConsumer {
public:
  void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                        const clang::Diagnostic &info) override {
    DiagnosticConsumer::HandleDiagnostic(level, info);
    if (level < clang::DiagnosticsEngine::Warning)
      return;
    llvm::SmallString<128> formatted;
    info.FormatDiagnostic(formatted);
    // Drop the ` [weavec::<id>]` suffix the sink appends.
    std::string message = formatted.str().str();
    if (const auto at = message.rfind(" [weavec::"); at != std::string::npos)
      message.erase(at);
    std::string where;
    if (info.hasSourceManager() && info.getLocation().isValid()) {
      const clang::SourceManager &sm = info.getSourceManager();
      const clang::PresumedLoc loc = sm.getPresumedLoc(info.getLocation());
      where = std::string(loc.getFilename()) + ":" +
              std::to_string(loc.getLine()) + ": ";
    }
    lines.push_back(
        where +
        (level == clang::DiagnosticsEngine::Error ? "error: " : "warning: ") +
        message);
  }

  std::vector<std::string> lines;
};

/// A unit held in memory; every unit of a test shares one virtual file
/// system so headers can be shared too.
class InMemoryUnit final : public ProgramUnit {
public:
  InMemoryUnit(std::string fileName,
               llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> fs,
               Recorder &recorder)
      : file(std::move(fileName)), fs(std::move(fs)), recorder(recorder) {}

  [[nodiscard]] std::string name() const override { return file; }

  bool run(clang::tooling::FrontendActionFactory &factory) override {
    const std::vector<std::string> args{
        "weavec", "-fsyntax-only", "-std=c17", "-x", "c", "-w", file};
    auto files = llvm::makeIntrusiveRefCnt<clang::FileManager>(
        clang::FileSystemOptions(), fs);
    clang::tooling::ToolInvocation invocation(args, factory.create(),
                                              files.get());
    // The consumer is shared by every run; its counts must not carry over
    // (a real driver has one consumer per compiler instance).
    recorder.clear();
    invocation.setDiagnosticConsumer(&recorder);
    return invocation.run();
  }

private:
  std::string file;
  llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> fs;
  Recorder &recorder;
};

struct Program {
  Recorder recorder;
  llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> fs =
      llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
  FrontendOptions options;
  std::vector<std::string> files;

  Program &add(const std::string &name, const std::string &code) {
    fs->addFile(
        "/src/" + name, 0,
        llvm::MemoryBuffer::getMemBufferCopy(std::string(Prelude) + code));
    files.push_back("/src/" + name);
    return *this;
  }

  Program &header(const std::string &name, const std::string &code) {
    fs->addFile("/src/" + name, 0, llvm::MemoryBuffer::getMemBufferCopy(code));
    return *this;
  }

  /// The analysis of the last `run`, for inspecting its database.
  std::unique_ptr<ProgramAnalysis> analysis;

  ProgramAnalysis::Result run() {
    analysis = std::make_unique<ProgramAnalysis>(options);
    for (const std::string &file : files)
      analysis->addUnit(std::make_unique<InMemoryUnit>(file, fs, recorder));
    return analysis->run();
  }
};

TEST(ProgramAnalysis, CalleesInOtherUnitsAreChecked) {
  Program program;
  program.header("node.h", "struct node { int v; };\n"
                           "struct node *node_new(void);\n"
                           "void node_free(struct node *n);\n");
  program.add("node.c", R"c(
#include "node.h"
struct node *node_new(void) { return malloc(sizeof(struct node)); }
void node_free(struct node *n) { free(n); }
)c");
  program.add("main.c", R"c(
#include "node.h"
int main(void) {
  struct node *n = node_new();
  node_free(n);
  node_free(n);
  return 0;
}
)c");
  const ProgramAnalysis::Result result = program.run();
  EXPECT_TRUE(result.failed.empty());
  EXPECT_TRUE(result.nonConverging.empty());
  EXPECT_EQ(result.errors, 1U);
  EXPECT_EQ(
      program.recorder.lines,
      (std::vector<std::string>{"/src/main.c:6: error: 'n' is freed twice"}));
  const analysis::ProgramDatabase &db = program.analysis->database();
  EXPECT_TRUE(db.defines("node_free"));
  EXPECT_TRUE(db.defines("node_new"));
  EXPECT_FALSE(db.defines("main"));
}

TEST(ProgramAnalysis, NoBoundaryWarningForCalleesTheProgramDefines) {
  Program program;
  program.add("a.c", "void take(char *p) { free(p); }\n");
  program.add("b.c", R"c(
void take(char *p);
void blob_close(void *b);
void f(void) {
  char *p = malloc(1);
  take(p);
  blob_close(malloc(1));
}
)c");
  program.add("c.c", R"c(
void blob_close(void *b);
void g(void) { blob_close(malloc(1)); }
)c");
  const ProgramAnalysis::Result result = program.run();
  EXPECT_EQ(result.errors, 0U) << llvm::join(program.recorder.lines, "\n");
  // `take` is defined in a.c: no warning. `blob_close` is defined nowhere:
  // one warning for the program, not one per calling unit.
  EXPECT_EQ(result.warnings, 1U);
  ASSERT_EQ(program.recorder.lines.size(), 1U)
      << llvm::join(program.recorder.lines, "\n");
  EXPECT_EQ(program.recorder.lines[0],
            "/src/b.c:7: warning: call to 'blob_close' is not checked: it has "
            "no definition or ownership annotations here");
}

TEST(ProgramAnalysis, MutuallyDependentUnitsReachAFixpoint) {
  Program program;
  // a.c needs b.c's summary of `b_free` and b.c needs a.c's of `a_free`.
  program.add("a.c", R"c(
void b_free(void *p);
void a_free(void *p) { b_free(p); }
)c");
  program.add("b.c", R"c(
void a_free(void *p);
void b_free(void *p) { free(p); }
int use(void) {
  char *x = malloc(1);
  a_free(x);
  return x[0];
}
)c");
  const ProgramAnalysis::Result result = program.run();
  EXPECT_TRUE(result.nonConverging.empty());
  EXPECT_TRUE(result.failed.empty());
  EXPECT_EQ(result.errors, 1U);
  EXPECT_EQ(program.recorder.lines,
            (std::vector<std::string>{
                "/src/b.c:7: error: use of 'x' after it was freed"}));
  const core::FunctionSummary *aFree =
      program.analysis->database().find("a_free");
  ASSERT_NE(aFree, nullptr);
  EXPECT_TRUE(aFree->frees(0));
}

TEST(ProgramAnalysis, CallbacksRegisteredElsewhereAreCandidates) {
  Program program;
  program.add("handlers.c", R"c(
void on_done(void *p) { free(p); }
void (*get_handler(void))(void *);
void (*get_handler(void))(void *) { return on_done; }
)c");
  program.add("loop.c", R"c(
void (*get_handler(void))(void *);
int run(void) {
  char *buf = malloc(4);
  void (*h)(void *) = get_handler();
  h(buf);
  return buf[0];
}
)c");
  const ProgramAnalysis::Result result = program.run();
  EXPECT_TRUE(result.failed.empty());
  EXPECT_EQ(result.errors, 1U);
  EXPECT_EQ(program.recorder.lines,
            (std::vector<std::string>{
                "/src/loop.c:7: error: use of 'buf' after it was freed"}));
}

TEST(ProgramAnalysis, UnparsableUnitsAreReportedNotFatal) {
  Program program;
  program.add("ok.c", "void f(void) {}\n");
  program.files.push_back("/src/missing.c");
  const ProgramAnalysis::Result result = program.run();
  EXPECT_EQ(result.failed, (std::vector<std::string>{"/src/missing.c"}));
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.errors, 0U) << llvm::join(program.recorder.lines, "\n");
}

TEST(ProgramAnalysis, DumpNamesUnitsThenTheProgram) {
  Program program;
  std::string dump;
  llvm::raw_string_ostream os(dump);
  program.options.analysis.dumpStream = &os;
  program.add("a.c", "void a_free(void *p) { free(p); }\n");
  program.add("b.c", R"c(
void a_free(void *p);
void b(void) { a_free(malloc(1)); }
)c");
  const ProgramAnalysis::Result result = program.run();
  EXPECT_TRUE(result.ok());
  EXPECT_NE(dump.find("unit '/src/a.c':\nfunction 'a_free':\n"),
            std::string::npos)
      << dump;
  EXPECT_NE(dump.find("unit '/src/b.c':\nfunction 'b':\n"), std::string::npos)
      << dump;
  EXPECT_NE(dump.find("program:\n  function 'a_free': param 0: freed; "
                      "stores{} returns{}\n"),
            std::string::npos)
      << dump;
  // Dependencies come first.
  EXPECT_LT(dump.find("unit '/src/a.c'"), dump.find("unit '/src/b.c'"));
}

TEST(ProgramAnalysis, KnownExportsSkipDiscoveryAndFixedUnitsAreNotRerun) {
  Program program;
  program.add("main.c", R"c(
void node_free(void *n);
int main(void) {
  char *n = malloc(1);
  node_free(n);
  node_free(n);
  return 0;
}
)c");
  // node.c stands as compiled: only its exports take part.
  analysis::UnitExports node;
  node.source = "node.c";
  core::FunctionSummary freeSummary;
  freeSummary.addEffect(core::SummaryPath::param(0),
                        core::PlaceEffect{.freed = true});
  node.functions["node_free"] =
      analysis::ExportedFunction{.summary = freeSummary,
                                 .typeKey = "void (void *)",
                                 .external = true,
                                 .addressTaken = false};

  ProgramAnalysis analysis(program.options);
  analysis.addExports(node);
  for (const std::string &file : program.files) {
    analysis.addUnit(
        std::make_unique<InMemoryUnit>(file, program.fs, program.recorder));
  }
  const ProgramAnalysis::Result result = analysis.run();
  EXPECT_EQ(result.errors, 1U);
  EXPECT_EQ(
      program.recorder.lines,
      (std::vector<std::string>{"/src/main.c:6: error: 'n' is freed twice"}));
}

TEST(ProgramAnalysis, AlreadyReportedDiagnosticsAreNotRepeated) {
  Program program;
  program.add("main.c", R"c(
int main(void) {
  char *n = malloc(1);
  free(n);
  free(n);
  return 0;
}
)c");
  ProgramAnalysis analysis(program.options);
  std::set<ReportedDiagnostic> earlier{ReportedDiagnostic{
      .id = "double-free", .file = "/src/main.c", .line = 5, .column = 3}};
  analysis.addUnit(std::make_unique<InMemoryUnit>(program.files[0], program.fs,
                                                  program.recorder),
                   std::nullopt, earlier);
  const ProgramAnalysis::Result result = analysis.run();
  EXPECT_EQ(result.errors, 0U);
  EXPECT_TRUE(program.recorder.lines.empty());
}

} // namespace
} // namespace weavec::frontend
