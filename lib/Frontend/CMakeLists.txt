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
          # RFC 0041: Safe C's rules and its bounds in the passes.
          GuardSafe.cpp
          GuardTyped.cpp
          SafeCheck.cpp
          # RFC 0040 §4.2: weavec-cc --weavec-symbolize.
          Symbolizer.cpp
          UnsafeRegions.cpp
  PUBLIC_DEPS weavec::Core
  USES_LLVM)

target_compile_definitions(
  weavecFrontend
  PRIVATE "WEAVEC_BUILD_RESOURCE_DIR=\"${WEAVEC_BUILD_RESOURCE_DIR}\""
          "WEAVEC_RESOURCE_DIR_RELATIVE=\"${WEAVEC_RESOURCE_DIR_RELATIVE}\""
          "WEAVEC_RUNTIME_ABI=${WEAVEC_RUNTIME_ABI}")

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
  # RFC 0040 §4.2: the symbolizer's line tables.
  Symbolize
  TransformUtils)
weavec_link_clang(
  weavecFrontend
  clangFrontend
  clangCodeGen
  clangDriver
  clangParse
  clangSema
  clangSerialization
  clangAST
  clangBasic
  clangLex)
