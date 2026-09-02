# Unit tests (GoogleTest).
#
# GoogleTest is taken from the system if a compatible package is installed,
# otherwise fetched at the pinned version below.
include(FetchContent)

set(WEAVEC_GTEST_VERSION
    "v1.18.0"
    CACHE STRING "GoogleTest tag to fetch when not found on the system")
mark_as_advanced(WEAVEC_GTEST_VERSION)

# Disable gmock/install for the fetched copy; keep gtest's own tests off.
set(BUILD_GMOCK
    OFF
    CACHE BOOL "" FORCE)
set(INSTALL_GTEST
    OFF
    CACHE BOOL "" FORCE)
set(gtest_force_shared_crt
    ON
    CACHE BOOL "" FORCE)

fetchcontent_declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG ${WEAVEC_GTEST_VERSION}
  GIT_SHALLOW TRUE
  FIND_PACKAGE_ARGS NAMES GTest)
fetchcontent_makeavailable(googletest)

include(GoogleTest)

# weavec_add_unittest(<name> SOURCES <src...> DEPS <target...>)
function(weavec_add_unittest name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE ${ARG_DEPS} weavec::options
                                        GTest::gtest_main)
  # gtest includes <cxxabi.h> for type-name demangling. With LLVM's include
  # directory on the search path (e.g. the apt.llvm.org packages, which ship
  # libc++abi's cxxabi.h alongside the LLVM headers) that resolves to the
  # libc++abi header even when building against libstdc++, and the two
  # declarations of __cxa_init_primary_exception conflict. Demangled type names
  # in test output are not worth the portability hazard.
  target_compile_definitions(${name} PRIVATE GTEST_HAS_CXXABI_H_=0)
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                           "${PROJECT_BINARY_DIR}/unittests")
  gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST PROPERTIES LABELS "unit")
  add_dependencies(check-weavec-unit ${name})
endfunction()

# Aggregate targets: `check-weavec-unit` builds and runs only the unit tests;
# `check-weavec` (defined in test/) runs everything.
add_custom_target(
  check-weavec-unit
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L unit
  WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
  COMMENT "Running WeaveC unit tests"
  USES_TERMINAL)

add_subdirectory(Core)
add_subdirectory(Analysis)
add_subdirectory(Frontend)
