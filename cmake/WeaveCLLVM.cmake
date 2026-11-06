# Locates LLVM and Clang and exposes helper functions for linking against them.
#
# Discovery order:
#   1. LLVM_DIR / Clang_DIR if set (e.g. via CMakePresets or -D).
#   2. CMAKE_PREFIX_PATH.
#   3. On macOS, the Homebrew `llvm` keg as a convenience hint.
#
# Exposes:
#   weavec::llvm    INTERFACE target carrying LLVM include dirs and definitions
#   weavec_link_llvm(<target> <components...>)
#   weavec_link_clang(<target> <clang-libs...>)

set(WEAVEC_LLVM_MIN_VERSION
    "20.0"
    CACHE STRING "Minimum supported LLVM version")
mark_as_advanced(WEAVEC_LLVM_MIN_VERSION)

if(APPLE
   AND NOT LLVM_DIR
   AND NOT DEFINED ENV{LLVM_DIR})
  find_program(_weavec_brew NAMES brew)
  if(_weavec_brew)
    execute_process(
      COMMAND "${_weavec_brew}" --prefix llvm
      OUTPUT_VARIABLE _weavec_brew_llvm
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _weavec_brew_result
      ERROR_QUIET)
    if(_weavec_brew_result EQUAL 0 AND IS_DIRECTORY
                                       "${_weavec_brew_llvm}/lib/cmake/llvm")
      list(APPEND CMAKE_PREFIX_PATH "${_weavec_brew_llvm}")
    endif()
  endif()
endif()

# LLVMConfigVersion.cmake only accepts same-major requests, so find any version
# and enforce the minimum ourselves.
find_package(LLVM REQUIRED CONFIG)
if(LLVM_PACKAGE_VERSION VERSION_LESS WEAVEC_LLVM_MIN_VERSION)
  message(
    FATAL_ERROR "WeaveC requires LLVM >= ${WEAVEC_LLVM_MIN_VERSION}; found "
                "${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")
endif()
find_package(Clang REQUIRED CONFIG HINTS "${LLVM_DIR}/../clang"
             "${LLVM_LIBRARY_DIR}/cmake/clang")

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_INSTALL_PREFIX}")
message(STATUS "Found Clang at ${CLANG_INSTALL_PREFIX}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}" "${CLANG_CMAKE_DIR}")
include(AddLLVM)

# LLVM's own header for FileCheck / not / count / lit locations.
set(WEAVEC_LLVM_TOOLS_DIR "${LLVM_TOOLS_BINARY_DIR}")

# Clang's resource directory (builtin headers such as stddef.h, the
# compiler-rt builtins) and the reference `clang` the tests compare with. RFC
# 0038 section 6: weavec-cc records neither path; Clang's driver finds the
# resource directory from its executable, at ../lib/clang/<major>, where the
# install puts a copy (cmake/WeaveCInstall.cmake) and the build tree a link.
find_program(
  WEAVEC_REFERENCE_CLANG
  NAMES clang clang-${LLVM_VERSION_MAJOR}
  HINTS "${LLVM_TOOLS_BINARY_DIR}"
  NO_DEFAULT_PATH)
mark_as_advanced(WEAVEC_REFERENCE_CLANG)
set(WEAVEC_CLANG_RESOURCES "")
if(WEAVEC_REFERENCE_CLANG)
  execute_process(
    COMMAND "${WEAVEC_REFERENCE_CLANG}" -print-resource-dir
    OUTPUT_VARIABLE _weavec_resource_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _weavec_resource_result
    ERROR_QUIET)
  if(_weavec_resource_result EQUAL 0 AND IS_DIRECTORY "${_weavec_resource_dir}")
    set(WEAVEC_CLANG_RESOURCES "${_weavec_resource_dir}")
  endif()
endif()
if(NOT WEAVEC_CLANG_RESOURCES
   AND IS_DIRECTORY "${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}")
  set(WEAVEC_CLANG_RESOURCES "${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}")
endif()
if(WEAVEC_CLANG_RESOURCES)
  message(STATUS "Clang resource dir: ${WEAVEC_CLANG_RESOURCES}")
  set(_weavec_build_resources
      "${PROJECT_BINARY_DIR}/lib/clang/${LLVM_VERSION_MAJOR}")
  if(NOT EXISTS "${_weavec_build_resources}")
    file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/lib/clang")
    file(CREATE_LINK "${WEAVEC_CLANG_RESOURCES}" "${_weavec_build_resources}"
         SYMBOLIC)
  endif()
else()
  message(WARNING "Could not determine Clang's resource directory; "
                  "weavec-cc will not find builtin headers such as <stddef.h>")
endif()

# ---------------------------------------------------------------------------
# weavec::llvm interface target
# ---------------------------------------------------------------------------
add_library(weavec_llvm INTERFACE)
add_library(weavec::llvm ALIAS weavec_llvm)

# SYSTEM so that warnings from LLVM/Clang headers are suppressed.
foreach(_dir IN LISTS LLVM_INCLUDE_DIRS CLANG_INCLUDE_DIRS)
  target_include_directories(weavec_llvm SYSTEM INTERFACE ${_dir})
endforeach()

separate_arguments(_llvm_definitions NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_definitions(weavec_llvm INTERFACE ${_llvm_definitions})

# Match LLVM's RTTI / exception configuration; mismatches cause link errors for
# any class deriving from an LLVM/Clang polymorphic type.
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(
    weavec_llvm INTERFACE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-fno-rtti>
                          $<$<CXX_COMPILER_ID:MSVC>:/GR->)
endif()
if(NOT LLVM_ENABLE_EH)
  target_compile_options(
    weavec_llvm
    INTERFACE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-fno-exceptions>)
endif()

# ---------------------------------------------------------------------------
# Linking helpers
# ---------------------------------------------------------------------------

# RFC 0038 section 6: WEAVEC_SELF_CONTAINED builds a weavec-cc that needs no
# LLVM installation at run time. On Linux it links LLVM's and Clang's static
# libraries. The installations' CMake packages describe those in terms of the
# shared libraries (Clang's link "LLVM"; LLVMSupport links z3 and zstd), so
# the archives are linked directly: every libLLVM*.a, libPolly*.a and
# libclang*.a but the C API's, as one group, with the system's zlib and zstd.
# Members are taken only when referenced, so what weavec-cc does not use (the
# static analyzer, and z3 with it) stays out. On Darwin, whose Homebrew static
# Clang lacks the code generator's target builtins, the install copies the
# shared libraries into the toolchain instead (cmake/WeaveCBundleLLVM.cmake).
option(WEAVEC_SELF_CONTAINED
       "Build a weavec-cc that needs no LLVM installation" OFF)
set(WEAVEC_STATIC_LLVM OFF)
if(WEAVEC_SELF_CONTAINED AND NOT APPLE)
  set(WEAVEC_STATIC_LLVM ON)
endif()
if(WEAVEC_STATIC_LLVM)
  file(GLOB _weavec_llvm_archives "${LLVM_LIBRARY_DIR}/libLLVM*.a"
       "${LLVM_LIBRARY_DIR}/libPolly*.a")
  file(GLOB _weavec_clang_archives "${LLVM_LIBRARY_DIR}/libclang*.a")
  list(FILTER _weavec_clang_archives EXCLUDE REGEX "/libclang\\.a$")
  if(NOT _weavec_llvm_archives OR NOT _weavec_clang_archives)
    message(FATAL_ERROR "WEAVEC_STATIC_LLVM: no static LLVM or Clang libraries "
                        "in ${LLVM_LIBRARY_DIR}")
  endif()
  # zlib and zstd from the system, by their runtime names when no development
  # package is installed.
  find_library(WEAVEC_ZSTD_SHARED NAMES zstd libzstd.so.1)
  find_library(WEAVEC_ZLIB NAMES z libz.so.1 REQUIRED)
  add_library(weavec_llvm_static INTERFACE)
  target_link_libraries(
    weavec_llvm_static INTERFACE "-Wl,--start-group" ${_weavec_clang_archives}
                                 ${_weavec_llvm_archives} "-Wl,--end-group")
  target_link_libraries(weavec_llvm_static INTERFACE rt dl m pthread)
  if(WEAVEC_ZSTD_SHARED)
    target_link_libraries(weavec_llvm_static INTERFACE "${WEAVEC_ZSTD_SHARED}")
  endif()
  target_link_libraries(weavec_llvm_static INTERFACE "${WEAVEC_ZLIB}")
  list(LENGTH _weavec_llvm_archives _weavec_llvm_count)
  list(LENGTH _weavec_clang_archives _weavec_clang_count)
  message(STATUS "Static LLVM: ${_weavec_llvm_count} LLVM and "
                 "${_weavec_clang_count} Clang archives")
endif()

# weavec_link_llvm(<target> <component...>)
#   Links LLVM components, respecting LLVM_LINK_LLVM_DYLIB.
function(weavec_link_llvm target)
  if(WEAVEC_STATIC_LLVM)
    target_link_libraries(${target} PUBLIC weavec_llvm_static)
  elseif(LLVM_LINK_LLVM_DYLIB)
    target_link_libraries(${target} PUBLIC LLVM)
  else()
    llvm_map_components_to_libnames(_libs ${ARGN})
    target_link_libraries(${target} PUBLIC ${_libs})
  endif()
endfunction()

# weavec_link_clang(<target> <clang-lib...>)
#   Links Clang libraries, respecting CLANG_LINK_CLANG_DYLIB.
function(weavec_link_clang target)
  if(WEAVEC_STATIC_LLVM)
    target_link_libraries(${target} PUBLIC weavec_llvm_static)
  elseif(CLANG_LINK_CLANG_DYLIB)
    target_link_libraries(${target} PUBLIC clang-cpp)
  else()
    target_link_libraries(${target} PUBLIC ${ARGN})
  endif()
endfunction()
