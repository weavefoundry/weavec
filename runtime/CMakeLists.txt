# The runtime libraries of RFC 0030, built for the host and installed under
# lib/weavec next to include/weavec.h:
#
#   libweavec_rt.a   __weavec_rt_report, for -fweavec-checks=report (10.7)
#   libweavec_chk.a  out-of-line check helpers, for precompiled headers and
#                    modules (10.9), generated from the prelude itself by
#                    `weavec-cc -fweavec-print-prelude=out-of-line`
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

add_library(weavec_rt STATIC weavec_rt.c)
add_library(weavec_chk STATIC weavec_chk.c weavec_chk_report.c
                              "${_weavec_chk_inc}" "${_weavec_chk_report_inc}")
target_include_directories(weavec_chk PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

foreach(_weavec_runtime IN ITEMS weavec_rt weavec_chk)
  # C, so none of weavec::options' C++ flags.
  set_target_properties(
    ${_weavec_runtime} PROPERTIES ARCHIVE_OUTPUT_DIRECTORY
                                  "${WEAVEC_RUNTIME_OUTPUT_DIR}")
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${_weavec_runtime} PRIVATE -Wall -Wextra)
    if(WEAVEC_WARNINGS_AS_ERRORS)
      target_compile_options(${_weavec_runtime} PRIVATE -Werror)
    endif()
  endif()
endforeach()

if(WEAVEC_INSTALL)
  install(TARGETS weavec_rt weavec_chk
          ARCHIVE DESTINATION "${WEAVEC_RESOURCE_DIR_RELATIVE}"
                  COMPONENT WeaveC_Runtime)
endif()
