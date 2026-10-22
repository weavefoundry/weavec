# weavec::Frontend -- Clang FrontendAction / libTooling integration.
weavec_add_library(
  Frontend
  SOURCES Driver.cpp
          ResourceDir.cpp
          # RFC 0035, RFC 0036: the guard passes, their frames, globals and
          # library calls, the unsafe regions and the enforcement ledger.
          EnforcementLedger.cpp
          GuardExpand.cpp
          GuardFrames.cpp
          GuardLoops.cpp
          GuardGlobals.cpp
          GuardInsert.cpp
          GuardLibrary.cpp
          GuardPass.cpp
          UnsafeRegions.cpp
  PUBLIC_DEPS weavec::Core
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
