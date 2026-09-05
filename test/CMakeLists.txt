# Integration tests driven by LLVM's `lit` and `FileCheck`.
#
# `lit` is located via (in order) LLVM_EXTERNAL_LIT, `llvm-lit` in the LLVM
# tools directory, or a `lit` on PATH (pip install lit / brew install lit).
# FileCheck, not and count come from the LLVM installation.

find_program(
  WEAVEC_LIT
  NAMES llvm-lit lit lit.py
  HINTS "${WEAVEC_LLVM_TOOLS_DIR}" "${LLVM_EXTERNAL_LIT}"
  DOC "Path to the lit test runner")
find_program(
  WEAVEC_FILECHECK
  NAMES FileCheck
  HINTS "${WEAVEC_LLVM_TOOLS_DIR}"
  DOC "Path to FileCheck")
find_program(
  WEAVEC_NOT
  NAMES not
  HINTS "${WEAVEC_LLVM_TOOLS_DIR}"
  NO_DEFAULT_PATH
  DOC "Path to LLVM's `not` utility")
find_program(
  WEAVEC_COUNT
  NAMES count
  HINTS "${WEAVEC_LLVM_TOOLS_DIR}"
  NO_DEFAULT_PATH
  DOC "Path to LLVM's `count` utility")

# Aggregate target that runs every test suite.
add_custom_target(
  check-weavec
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
  WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
  COMMENT "Running all WeaveC tests"
  USES_TERMINAL)
add_dependencies(check-weavec check-weavec-unit)

# The recall set (RFC 0011, *Recall check*): Juliet-shaped cases under
# test/recall/, one bug each, run by scripts/recall.py. Needs only Python.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(
    NAME recall
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/recall.py"
            --weavec "$<TARGET_FILE:weavec>")
  set_tests_properties(recall PROPERTIES LABELS "integration")
else()
  message(
    WARNING "Python 3 not found; the recall set (test/recall) is disabled.")
endif()

if(NOT WEAVEC_LIT
   OR NOT WEAVEC_FILECHECK
   OR NOT WEAVEC_NOT
   OR NOT WEAVEC_COUNT)
  message(
    WARNING
      "lit/FileCheck/not/count not found; integration tests are disabled. "
      "Install lit (pip install lit) and ensure your LLVM build ships its "
      "test utilities (LLVM_INSTALL_UTILS=ON).")
  return()
endif()

set(WEAVEC_LIT_TOOLS_DIR "${WEAVEC_LLVM_TOOLS_DIR}")
get_filename_component(_filecheck_dir "${WEAVEC_FILECHECK}" DIRECTORY)
if(NOT _filecheck_dir STREQUAL WEAVEC_LLVM_TOOLS_DIR)
  set(WEAVEC_LIT_TOOLS_DIR "${_filecheck_dir}")
endif()

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in"
               "${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py" @ONLY)

set(WEAVEC_LIT_ARGS
    "-sv"
    CACHE STRING "Arguments passed to lit")

add_custom_target(
  check-weavec-lit
  COMMAND "${WEAVEC_LIT}" ${WEAVEC_LIT_ARGS} "${CMAKE_CURRENT_BINARY_DIR}"
  DEPENDS weavec weavec-cc
  COMMENT "Running WeaveC lit tests"
  USES_TERMINAL)
add_dependencies(check-weavec check-weavec-lit)

# Expose the lit suite to CTest as a single test so `ctest` runs everything.
add_test(NAME lit COMMAND "${WEAVEC_LIT}" ${WEAVEC_LIT_ARGS}
                          "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(lit PROPERTIES LABELS "integration")
