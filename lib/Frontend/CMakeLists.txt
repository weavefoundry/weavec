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
          # RFC 0043: ownership's rules.
          SafeOwnership.cpp
          # RFC 0044: Safe C++.
          SafeCxx.cpp
          SafeCxxFlow.cpp
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
  # RFC 0043 §10: the CFG ownership's rules run on.
  clangAnalysis
  clangDriver
  clangParse
  clangSema
  clangSerialization
  clangAST
  clangBasic
  clangLex)

# RFC 0043 §12: the rules' explanations, embedded for --weavec-explain as the
# library table is in Core.
set(_weavec_safe_rules "${PROJECT_SOURCE_DIR}/resources/safe/rules.md")
set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS "${_weavec_safe_rules}")
file(READ "${_weavec_safe_rules}" _weavec_safe_rules_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "'\\\\x\\1',"
                     _weavec_safe_rules_bytes "${_weavec_safe_rules_hex}")
file(
  CONFIGURE
  OUTPUT
  "${CMAKE_CURRENT_BINARY_DIR}/SafeRulesText.inc"
  CONTENT
  "// Generated from resources/safe/rules.md by lib/Frontend/CMakeLists.txt.
// NOLINTNEXTLINE(*-avoid-c-arrays)
static constexpr char SafeRulesTextData[] = {@_weavec_safe_rules_bytes@ '\\0'};
static constexpr std::size_t SafeRulesTextSize = sizeof(SafeRulesTextData) - 1;
"
  @ONLY)
target_include_directories(weavecFrontend PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
