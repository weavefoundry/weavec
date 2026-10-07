# The runtime libraries of RFC 0035, built for the host and installed under
# lib/weavec next to include/weavec.h:
#
#   libweavec_rt.a     the runtime every enforcing link carries: the arena
#                      allocator and its shadow memory, the guards' slow
#                      paths, the checked library calls, frame and global
#                      registration, and the reports
#   libweavec_alloc.a  malloc, free and the rest of the standard allocation
#                      functions over the arena; left out of a link whose
#                      program defines the allocator itself (RFC 0032, 2.6)
set(WEAVEC_RUNTIME_OUTPUT_DIR
    "${PROJECT_BINARY_DIR}/${WEAVEC_RESOURCE_DIR_RELATIVE}")

# The guard pass calls the slow paths with Clang's preserve_all convention
# (weavec_rt.h).
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "The WeaveC runtime must be built with Clang (CC).")
endif()

add_library(weavec_rt STATIC weavec_alloc.c weavec_guard.c weavec_libc.c
                             weavec_owner.c weavec_report.c weavec_rt.h)
add_library(weavec_alloc STATIC weavec_malloc.c weavec_rt.h)

foreach(_weavec_runtime IN ITEMS weavec_rt weavec_alloc)
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
  install(TARGETS weavec_rt weavec_alloc
          ARCHIVE DESTINATION "${WEAVEC_RESOURCE_DIR_RELATIVE}"
                  COMPONENT WeaveC_Runtime)
endif()
