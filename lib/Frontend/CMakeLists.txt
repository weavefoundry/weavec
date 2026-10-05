# weavec::Frontend -- Clang FrontendAction / libTooling integration.
weavec_add_library(
  Frontend
  SOURCES AnalysisStats.cpp
          ClangDiagnosticSink.cpp
          DeferredCodeGenConsumer.cpp
          DiagnosticControl.cpp
          # RFC 0031 §9.2: the dispatch edge split.
          DispatchEdges.cpp
          Driver.cpp
          FrontendAction.cpp
          # RFC 0034 §1: the guards the backend canonicalises, merges and
          # expands.
          GuardPasses.cpp
          Prelude.cpp
          ProgramAnalysis.cpp
          ResourceDir.cpp
          # RFC 0030: the ledger writers, where ledgers and summary lines
          # go, the format-28 unit record and the link step.
          LedgerOutput.cpp
          LedgerWriter.cpp
          LinkStep.cpp
          RecordFacts.cpp
          RecordPayload.cpp
          UnitRecord.cpp
          # RFC 0030 S5: the check rewrites and zero-initialisation.
          CheckEmitter.cpp
          ZeroInit.cpp
          # RFC 0032 §4, §5: which stack and global objects are registered.
          ObjectRegistration.cpp
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
