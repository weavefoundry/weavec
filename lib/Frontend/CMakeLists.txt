# weavec::Frontend -- Clang FrontendAction / libTooling integration.
weavec_add_library(
  Frontend
  SOURCES ClangDiagnosticSink.cpp
          DiagnosticControl.cpp
          Driver.cpp
          FrontendAction.cpp
          ProgramAnalysis.cpp
          ResourceDir.cpp
          Sidecar.cpp
  PUBLIC_DEPS weavec::Analysis
  USES_LLVM)

target_compile_definitions(
  weavecFrontend
  PRIVATE "WEAVEC_BUILD_RESOURCE_DIR=\"${WEAVEC_BUILD_RESOURCE_DIR}\""
          "WEAVEC_RESOURCE_DIR_RELATIVE=\"${WEAVEC_RESOURCE_DIR_RELATIVE}\""
          "WEAVEC_CLANG_RESOURCE_DIR=\"${WEAVEC_CLANG_RESOURCE_DIR}\""
          "WEAVEC_CLANG_EXECUTABLE=\"${WEAVEC_CLANG_EXECUTABLE}\"")

weavec_link_llvm(weavecFrontend Support Option)
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
