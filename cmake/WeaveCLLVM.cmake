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

# Clang's resource directory (builtin headers such as stddef.h). libTooling
# would otherwise guess it relative to *our* executable, which is wrong.
set(WEAVEC_CLANG_RESOURCE_DIR
    ""
    CACHE PATH "Clang resource directory used by the weavec tool (auto)")
mark_as_advanced(WEAVEC_CLANG_RESOURCE_DIR)
if(NOT WEAVEC_CLANG_RESOURCE_DIR)
  find_program(
    _weavec_clang_exe
    NAMES clang clang-${LLVM_VERSION_MAJOR}
    HINTS "${LLVM_TOOLS_BINARY_DIR}"
    NO_DEFAULT_PATH)
  if(_weavec_clang_exe)
    execute_process(
      COMMAND "${_weavec_clang_exe}" -print-resource-dir
      OUTPUT_VARIABLE _weavec_resource_dir
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _weavec_resource_result
      ERROR_QUIET)
    if(_weavec_resource_result EQUAL 0 AND IS_DIRECTORY
                                           "${_weavec_resource_dir}")
      set(WEAVEC_CLANG_RESOURCE_DIR "${_weavec_resource_dir}")
    endif()
  endif()
  if(NOT WEAVEC_CLANG_RESOURCE_DIR
     AND IS_DIRECTORY "${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}")
    set(WEAVEC_CLANG_RESOURCE_DIR
        "${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}")
  endif()
endif()
if(WEAVEC_CLANG_RESOURCE_DIR)
  message(STATUS "Clang resource dir: ${WEAVEC_CLANG_RESOURCE_DIR}")
else()
  message(WARNING "Could not determine Clang's resource directory; "
                  "`weavec` will not find builtin headers such as <stddef.h>")
endif()

# ---------------------------------------------------------------------------
# weavec::llvm interface target
# ---------------------------------------------------------------------------
add_library(weavec_llvm INTERFACE)
add_library(weavec::llvm ALIAS weavec_llvm)

# SYSTEM so that warnings from LLVM/Clang headers are suppressed. Only the
# build interface carries absolute paths; the installed package re-derives them
# from find_dependency(LLVM/Clang) in WeaveCConfig.cmake.
foreach(_dir IN LISTS LLVM_INCLUDE_DIRS CLANG_INCLUDE_DIRS)
  target_include_directories(weavec_llvm SYSTEM
                             INTERFACE $<BUILD_INTERFACE:${_dir}>)
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

# weavec_link_llvm(<target> <component...>)
#   Links LLVM components, respecting LLVM_LINK_LLVM_DYLIB.
function(weavec_link_llvm target)
  if(LLVM_LINK_LLVM_DYLIB)
    target_link_libraries(${target} PUBLIC LLVM)
  else()
    llvm_map_components_to_libnames(_libs ${ARGN})
    target_link_libraries(${target} PUBLIC ${_libs})
  endif()
endfunction()

# weavec_link_clang(<target> <clang-lib...>)
#   Links Clang libraries, respecting CLANG_LINK_CLANG_DYLIB.
function(weavec_link_clang target)
  if(CLANG_LINK_CLANG_DYLIB)
    target_link_libraries(${target} PUBLIC clang-cpp)
  else()
    target_link_libraries(${target} PUBLIC ${ARGN})
  endif()
endfunction()
