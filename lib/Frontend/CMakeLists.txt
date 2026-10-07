# weavec::Frontend -- Clang FrontendAction / libTooling integration.
weavec_add_library(
  Frontend
  SOURCES AnalysisStats.cpp
          ClangDiagnosticSink.cpp
          DiagnosticControl.cpp
          Driver.cpp
          FrontendAction.cpp
          ProgramAnalysis.cpp
          ResourceDir.cpp
          # RFC 0035: the guard pass, its frames, globals and library calls,
          # the unsafe regions and the enforcement ledger.
          EnforcementLedger.cpp
          GuardExpand.cpp
          GuardFrames.cpp
          GuardLoops.cpp
          GuardGlobals.cpp
          GuardInsert.cpp
          GuardLibrary.cpp
          GuardPass.cpp
          UnsafeRegions.cpp
          # RFC 0035 §8: the advisory analysis's summary lines and what
          # `weavec --whole-program` checks across units.
          AnalysisSummary.cpp
          InterfaceFacts.cpp
          ProgramChecks.cpp
  PUBLIC_DEPS weavec::Analysis
  USES_LLVM)

target_compile_definitions(
  weavecFrontend
  PRIVATE "WEAVEC_BUILD_RESOURCE_DIR=\"${WEAVEC_BUILD_RESOURCE_DIR}\""
          "WEAVEC_RESOURCE_DIR_RELATIVE=\"${WEAVEC_RESOURCE_DIR_RELATIVE}\""
          "WEAVEC_CLANG_RESOURCE_DIR=\"${WEAVEC_CLANG_RESOURCE_DIR}\""
          "WEAVEC_CLANG_EXECUTABLE=\"${WEAVEC_CLANG_EXECUTABLE}\"")

weavec_link_llvm(
  weavecFrontend
  Support
  Option
  Core
  Analysis
  # RFC 0032 §2.6: the symbols of a link's inputs (a program-defined
  # allocator).
  Object
  Passes
  TransformUtils)
weavec_link_clang(
  weavecFrontend
  clangTooling
  clangFrontendTool
  clangFrontend
  clangCodeGen
  clangDriver
  clangParse
  clangSema
  clangSerialization
  clangAST
  clangBasic
  clangLex)
