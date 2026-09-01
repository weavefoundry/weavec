cmake_minimum_required(VERSION 3.24...3.31)

project(
  WeaveC
  VERSION 0.1.0
  DESCRIPTION
    "A mostly source-compatible C compiler bringing inferred ownership and borrowing to C"
  HOMEPAGE_URL "https://github.com/weavefoundry/weavec"
  LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Project-wide settings
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      "Debug"
      CACHE STRING "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)"
            FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release
                                               RelWithDebInfo MinSizeRel)
endif()

# Whether this is the top-level project or being consumed via add_subdirectory.
set(WEAVEC_IS_TOP_LEVEL ${PROJECT_IS_TOP_LEVEL})

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(WEAVEC_BUILD_TESTS "Build unit and integration tests"
       ${WEAVEC_IS_TOP_LEVEL})
option(WEAVEC_BUILD_TOOLS "Build the weavec command-line tools" ON)
option(WEAVEC_BUILD_DOCS "Build documentation targets" OFF)
option(WEAVEC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(WEAVEC_ENABLE_LTO "Enable link-time optimization for release builds" OFF)
option(WEAVEC_ENABLE_CCACHE "Use ccache/sccache if available" ON)
option(WEAVEC_ENABLE_CLANG_TIDY "Run clang-tidy as part of the build" OFF)
option(WEAVEC_INSTALL "Generate install targets" ${WEAVEC_IS_TOP_LEVEL})
set(WEAVEC_SANITIZERS
    ""
    CACHE
      STRING
      "Semicolon-separated list of sanitizers to enable (address;undefined;thread;memory)"
)

include(WeaveCVersion)
include(WeaveCCompilerCache)
include(WeaveCWarnings)
include(WeaveCSanitizers)
include(WeaveCLLVM)
include(WeaveCHelpers)

# ---------------------------------------------------------------------------
# Common interface targets
# ---------------------------------------------------------------------------
# weavec::options -- compile options shared by every WeaveC target.
add_library(weavec_options INTERFACE)
add_library(weavec::options ALIAS weavec_options)
target_compile_features(weavec_options INTERFACE cxx_std_20)
weavec_apply_warnings(weavec_options)
weavec_apply_sanitizers(weavec_options)

if(WEAVEC_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _weavec_ipo_supported OUTPUT _weavec_ipo_output)
  if(_weavec_ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
  else()
    message(WARNING "LTO requested but not supported: ${_weavec_ipo_output}")
  endif()
endif()

if(WEAVEC_ENABLE_CLANG_TIDY)
  find_program(WEAVEC_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
  set(CMAKE_CXX_CLANG_TIDY "${WEAVEC_CLANG_TIDY_EXE}")
endif()

# ---------------------------------------------------------------------------
# Generated configuration headers
# ---------------------------------------------------------------------------
set(WEAVEC_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/include")
configure_file(include/weavec/Config/Version.h.in
               "${WEAVEC_GENERATED_INCLUDE_DIR}/weavec/Config/Version.h" @ONLY)

# Resource directory holding the C-facing annotation header (weavec.h). In the
# build tree it points at the source directory so tests can run uninstalled.
set(WEAVEC_RESOURCE_DIR_RELATIVE "lib/weavec")
set(WEAVEC_BUILD_RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")

# ---------------------------------------------------------------------------
# Components
# ---------------------------------------------------------------------------
add_subdirectory(lib)

if(WEAVEC_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(WEAVEC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(unittests)
  add_subdirectory(test)
endif()

if(WEAVEC_BUILD_DOCS)
  add_subdirectory(docs)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
if(WEAVEC_INSTALL)
  include(WeaveCInstall)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
if(WEAVEC_IS_TOP_LEVEL)
  message(STATUS "")
  message(STATUS "WeaveC ${WEAVEC_VERSION_STRING}")
  message(STATUS "  Build type:        ${CMAKE_BUILD_TYPE}")
  message(STATUS "  C++ compiler:      ${CMAKE_CXX_COMPILER_ID} "
                 "${CMAKE_CXX_COMPILER_VERSION}")
  message(STATUS "  LLVM:              ${LLVM_PACKAGE_VERSION} (${LLVM_DIR})")
  message(STATUS "  Clang:             ${CLANG_INSTALL_PREFIX}")
  message(STATUS "  Sanitizers:        ${WEAVEC_SANITIZERS}")
  message(STATUS "  Warnings as errors:${WEAVEC_WARNINGS_AS_ERRORS}")
  message(STATUS "  Tests:             ${WEAVEC_BUILD_TESTS}")
  message(STATUS "  Compiler cache:    ${WEAVEC_COMPILER_CACHE_SUMMARY}")
  message(STATUS "")
endif()
