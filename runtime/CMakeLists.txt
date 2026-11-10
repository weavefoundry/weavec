# The runtime of RFC 0035, installed under lib/weavec next to
# include/weavec.h. RFC 0038, section 1: one shared library that every image
# weavec-cc links depends on, so that a process has one runtime:
# libweavec_rt.so.<abi> on Linux, libweavec_rt.dylib (@rpath) on Darwin,
# exporting what weavec_rt.exports lists; and libweavec_rt.a, the same
# objects, for -static Linux programs and links another driver makes.
set(WEAVEC_RUNTIME_OUTPUT_DIR
    "${PROJECT_BINARY_DIR}/${WEAVEC_RESOURCE_DIR_RELATIVE}")
# Its interface version, WEAVEC_RUNTIME_ABI, comes from weavec_rt.h.

# The guard pass calls the slow paths with Clang's preserve_all convention
# (weavec_rt.h).
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "The WeaveC runtime must be built with Clang (CC).")
endif()

find_package(Threads REQUIRED)

add_library(
  weavec_rt_objects OBJECT
  weavec_alloc.c
  weavec_asan.c
  weavec_guard.c
  weavec_libc.c
  weavec_malloc.c
  weavec_report.c
  weavec_stack.c
  weavec_sweep.c
  weavec_symbolize.c
  weavec_rt.h)
# RFC 0039: what the library and the archive (WEAVEC_RT_ARCHIVE) define
# differently: the unwinding interposers, and Darwin's weak allocation
# functions, which ld64 would take from the archive (amendment 2).
add_library(weavec_rt_shared_objects OBJECT weavec_cxx.c weavec_unwind.c)
add_library(weavec_rt_archive_objects OBJECT weavec_cxx.c weavec_unwind.c)
target_compile_definitions(weavec_rt_archive_objects
                           PRIVATE WEAVEC_RT_ARCHIVE=1)
foreach(_weavec_objects IN ITEMS weavec_rt_objects weavec_rt_shared_objects
                                 weavec_rt_archive_objects)
  # C, so none of weavec::options' C++ flags. Position-independent, never
  # LTO bitcode (the per-configuration properties too: the top level sets
  # CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE, which outranks the plain
  # one), and everything but the entry points hidden.
  set_target_properties(
    ${_weavec_objects}
    PROPERTIES POSITION_INDEPENDENT_CODE ON
               INTERPROCEDURAL_OPTIMIZATION OFF
               INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF
               INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO OFF
               C_STANDARD 11
               C_VISIBILITY_PRESET hidden)
  # The allocator and the guards run in every enforcing program: always
  # optimized, whatever the build type, and never treated as calls to the
  # functions they implement. Exceptions unwind through the interposers.
  target_compile_options(
    ${_weavec_objects}
    PRIVATE -Wall
            -Wextra
            -O2
            # RFC 0040 section 1: the unwinder follows frame records.
            -fno-omit-frame-pointer
            -funwind-tables
            -fno-builtin-malloc
            -fno-builtin-calloc
            -fno-builtin-realloc
            -fno-builtin-free)
  if(WEAVEC_WARNINGS_AS_ERRORS)
    target_compile_options(${_weavec_objects} PRIVATE -Werror)
  endif()
  if(NOT APPLE)
    # Its thread-local variables are in the C library's static TLS, which a
    # library loaded by dlopen can still use (weavec_guard.c).
    target_compile_options(${_weavec_objects} PRIVATE -ftls-model=initial-exec)
  endif()
endforeach()

# The export list, in each linker's form: Darwin's names, and a version
# script whose entries for symbols defined only with some C libraries are
# one-name patterns (an unmatched name is an error to LLD).
file(STRINGS weavec_rt.exports _weavec_export_lines REGEX "^[A-Za-z_]")
set(_weavec_exports)
foreach(_weavec_line IN LISTS _weavec_export_lines)
  string(REGEX MATCH "^[A-Za-z_0-9]+" _weavec_name "${_weavec_line}")
  string(REGEX MATCH " [a-z-]+$" _weavec_platform "${_weavec_line}")
  string(STRIP "${_weavec_platform}" _weavec_platform)
  if(APPLE AND (_weavec_platform STREQUAL "" OR _weavec_platform STREQUAL
                                                "darwin"))
    list(APPEND _weavec_exports "_${_weavec_name}")
  elseif(NOT APPLE AND _weavec_platform STREQUAL "linux-if-defined")
    string(SUBSTRING "${_weavec_name}" 0 1 _weavec_first)
    string(SUBSTRING "${_weavec_name}" 1 -1 _weavec_rest)
    list(APPEND _weavec_exports "[${_weavec_first}]${_weavec_rest}")
  elseif(NOT APPLE AND (_weavec_platform STREQUAL "" OR _weavec_platform
                                                        STREQUAL "linux"))
    list(APPEND _weavec_exports "${_weavec_name}")
  endif()
endforeach()
set(_weavec_exports_file "${CMAKE_CURRENT_BINARY_DIR}/weavec_rt.exported")
if(APPLE)
  list(JOIN _weavec_exports "\n" _weavec_exports_text)
else()
  # An anonymous version: the allocator's symbols must be unversioned, or
  # the C library's own references (malloc@GLIBC_2.17) would skip them. (A
  # semicolon cannot be part of a CMake list's element; a quoted string
  # keeps one.)
  list(JOIN _weavec_exports "@SEMICOLON@\n    " _weavec_exports_text)
  string(REPLACE "@SEMICOLON@" ";" _weavec_exports_text
                 "${_weavec_exports_text}")
  set(_weavec_exports_text
      "{\n  global:\n    ${_weavec_exports_text};\n  local: *;\n};")
endif()
file(
  GENERATE
  OUTPUT "${_weavec_exports_file}"
  CONTENT "${_weavec_exports_text}\n")
set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS weavec_rt.exports weavec_rt.h)

add_library(weavec_rt SHARED $<TARGET_OBJECTS:weavec_rt_objects>
                             $<TARGET_OBJECTS:weavec_rt_shared_objects>)
target_link_libraries(weavec_rt PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(APPLE)
  # __cxa_begin_catch's interposer names libc++abi's (every process has it).
  target_link_libraries(weavec_rt PRIVATE c++abi)
endif()
set_target_properties(
  weavec_rt
  PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${WEAVEC_RUNTIME_OUTPUT_DIR}"
             LINKER_LANGUAGE C
             LINK_DEPENDS "${_weavec_exports_file}")
# libweavec_rt.so.<abi> (libweavec_rt.<abi>.dylib), and libweavec_rt.so
# (.dylib) naming it: an image records the versioned name, so a program
# never loads a runtime of another ABI (RFC 0038, amendment 19).
set_target_properties(weavec_rt PROPERTIES SOVERSION "${WEAVEC_RUNTIME_ABI}")
if(APPLE)
  set_target_properties(
    weavec_rt
    PROPERTIES INSTALL_NAME_DIR "@rpath"
               BUILD_WITH_INSTALL_NAME_DIR ON
               MACHO_COMPATIBILITY_VERSION "${WEAVEC_RUNTIME_ABI}"
               MACHO_CURRENT_VERSION "${WEAVEC_RUNTIME_ABI}")
  target_link_options(weavec_rt PRIVATE
                      "LINKER:-exported_symbols_list,${_weavec_exports_file}")
else()
  target_link_options(
    weavec_rt PRIVATE "LINKER:--version-script=${_weavec_exports_file}"
    "LINKER:-Bsymbolic-functions" "LINKER:-z,defs")
endif()

# The same objects as an archive: a -static Linux program is one image; a
# link made by another driver from a weavec-cc link's implicit libraries
# (CMake's mixed-language targets) takes it; and rt_test calls the runtime's
# internal functions.
add_library(weavec_rt_static STATIC $<TARGET_OBJECTS:weavec_rt_objects>
                                    $<TARGET_OBJECTS:weavec_rt_archive_objects>)
set_target_properties(
  weavec_rt_static
  PROPERTIES OUTPUT_NAME weavec_rt ARCHIVE_OUTPUT_DIRECTORY
                                   "${WEAVEC_RUNTIME_OUTPUT_DIR}")

if(WEAVEC_INSTALL)
  install(
    TARGETS weavec_rt
    LIBRARY DESTINATION "${WEAVEC_RESOURCE_DIR_RELATIVE}"
            COMPONENT WeaveC_Runtime
            NAMELINK_COMPONENT WeaveC_Runtime)
  install(TARGETS weavec_rt_static
          ARCHIVE DESTINATION "${WEAVEC_RESOURCE_DIR_RELATIVE}"
                  COMPONENT WeaveC_Runtime)
endif()
