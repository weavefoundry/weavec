# The test/cases suites (RFC 0030 section 17.4): one CTest test per top-level
# directory, run by scripts/run-cases.py against this build tree's weavec and
# weavec-cc, so `ctest -j` runs the suites in parallel. WEAVEC_CASES_ARGS adds
# runner options to every suite, for example `--no-run` in the ASan job or
# `--no-emission` before checks are emitted. The golden binaries are not needed.

find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
  message(WARNING "Python 3 not found; the test/cases suites are disabled.")
  return()
endif()

set(WEAVEC_CASES_ARGS
    ""
    CACHE STRING "Extra scripts/run-cases.py options for the cases-* tests")
separate_arguments(_weavec_cases_args UNIX_COMMAND "${WEAVEC_CASES_ARGS}")

file(
  GLOB _weavec_case_suites
  LIST_DIRECTORIES true
  RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
  CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*")
list(SORT _weavec_case_suites)
foreach(suite IN LISTS _weavec_case_suites)
  if(NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${suite}" OR suite STREQUAL
                                                                "Inputs")
    continue()
  endif()
  add_test(
    NAME cases-${suite}
    COMMAND
      "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/run-cases.py"
      --weavec "$<TARGET_FILE:weavec>" --weavec-cc "$<TARGET_FILE:weavec-cc>"
      --filter "${suite}/**" --json
      "${CMAKE_CURRENT_BINARY_DIR}/cases-${suite}.json" ${_weavec_cases_args})
  set_tests_properties(cases-${suite} PROPERTIES LABELS "integration;cases"
                                                 TIMEOUT 900)
endforeach()

add_test(NAME cases-runner-harness
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/scripts/test_run_cases.py")
set_tests_properties(cases-runner-harness PROPERTIES LABELS "unit")
