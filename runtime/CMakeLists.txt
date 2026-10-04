# The runtime libraries of RFC 0030 and RFC 0032, built for the host and
# installed under lib/weavec next to include/weavec.h:
#
#   libweavec_rt.a     the runtime every enforcing link carries (RFC 0032):
#                      the arena allocator, the object table (heap, stack and
#                      global objects), the guards, and __weavec_rt_report
#   libweavec_alloc.a  malloc, free and the rest of the standard allocation
#                      functions over the arena; left out of a link whose
#                      program defines the allocator itself (RFC 0032, 2.6)
#   libweavec_chk.a    out-of-line check helpers, for precompiled headers and
#                      modules (RFC 0030, 10.9), generated from the prelude
#                      itself by `weavec-cc -fweavec-print-prelude=out-of-line`
set(WEAVEC_RUNTIME_OUTPUT_DIR
    "${PROJECT_BINARY_DIR}/${WEAVEC_RESOURCE_DIR_RELATIVE}")

set(_weavec_chk_inc "${CMAKE_CURRENT_BINARY_DIR}/weavec_chk_helpers.inc")
set(_weavec_chk_report_inc
    "${CMAKE_CURRENT_BINARY_DIR}/weavec_chk_report_helpers.inc")
add_custom_command(
  OUTPUT "${_weavec_chk_inc}" "${_weavec_chk_report_inc}"
  COMMAND weavec-cc -fweavec-print-prelude=out-of-line -fweavec-checks=verify -o
          "${_weavec_chk_inc}"
  COMMAND weavec-cc -fweavec-print-prelude=out-of-line -fweavec-checks=report -o
          "${_weavec_chk_report_inc}"
  DEPENDS weavec-cc
  COMMENT "Generating the out-of-line check helpers")

add_library(weavec_rt STATIC weavec_alloc.c weavec_objects.c weavec_owner.c
                             weavec_report.c weavec_rt.h)
add_library(weavec_alloc STATIC weavec_malloc.c weavec_rt.h)
add_library(weavec_chk STATIC weavec_chk.c weavec_chk_report.c
                              "${_weavec_chk_inc}" "${_weavec_chk_report_inc}")
target_include_directories(weavec_chk PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

foreach(_weavec_runtime IN ITEMS weavec_rt weavec_alloc weavec_chk)
  # C, so none of weavec::options' C++ flags.
  set_target_properties(
    ${_weavec_runtime} PROPERTIES ARCHIVE_OUTPUT_DIRECTORY
                                  "${WEAVEC_RUNTIME_OUTPUT_DIR}")
  # The archives are linked into executables and shared libraries alike, by
  # the system linker: position-independent, and never LTO bitcode (the
  # per-configuration properties too: the top level sets
  # CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE, which outranks the plain one).
  set_target_properties(
    ${_weavec_runtime}
    PROPERTIES POSITION_INDEPENDENT_CODE ON
               INTERPROCEDURAL_OPTIMIZATION OFF
               INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF
               INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO OFF
               C_STANDARD 11)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${_weavec_runtime} PRIVATE -Wall -Wextra)
    if(WEAVEC_WARNINGS_AS_ERRORS)
      target_compile_options(${_weavec_runtime} PRIVATE -Werror)
    endif()
  endif()
endforeach()

# The allocator and the guards run in every enforcing program: always
# optimised, whatever the build type, and never treated as calls to the
# functions they implement.
foreach(_weavec_runtime IN ITEMS weavec_rt weavec_alloc)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
      ${_weavec_runtime} PRIVATE -O2 -fno-builtin-malloc -fno-builtin-calloc
                                 -fno-builtin-realloc -fno-builtin-free)
  endif()
endforeach()

if(WEAVEC_BUILD_TESTS)
  # RFC 0032, section 12: the runtime alone, with the image's allocator
  # replaced as in a WeaveC-built program. The test frees and indexes what no
  # correct program would, so the compiler's own diagnostics are off for it.
  find_package(Threads REQUIRED)
  add_executable(weavec_rt_test test/rt_test.c)
  target_link_libraries(weavec_rt_test PRIVATE weavec_alloc weavec_rt
                                               Threads::Threads)
  set_target_properties(
    weavec_rt_test
    PROPERTIES C_STANDARD 11
               INTERPROCEDURAL_OPTIMIZATION OFF
               INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF
               INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO OFF)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(weavec_rt_test PRIVATE -w)
  endif()
endif()

if(WEAVEC_INSTALL)
  install(TARGETS weavec_rt weavec_alloc weavec_chk
          ARCHIVE DESTINATION "${WEAVEC_RESOURCE_DIR_RELATIVE}"
                  COMPONENT WeaveC_Runtime)
endif()
