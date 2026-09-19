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
  # RFC 0013: fixed-denominator evaluation, including known misses.
  add_test(
    NAME evaluation
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/evaluate.py"
            --weavec "$<TARGET_FILE:weavec>")
  set_tests_properties(evaluation PROPERTIES LABELS "integration" TIMEOUT 180)
  # RFC 0017 regression pairs are a separate, fixed population.
  add_test(
    NAME evaluation-rfc0017
    COMMAND
      "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/evaluate.py"
      --weavec "$<TARGET_FILE:weavec>" --manifest
      "${PROJECT_SOURCE_DIR}/test/evaluation/rfc0017/manifest.json")
  set_tests_properties(evaluation-rfc0017 PROPERTIES LABELS "integration"
                                                     TIMEOUT 180)
  add_test(
    NAME incremental-evaluation-rfc0020
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/scripts/incremental-evaluation.py" --weavec
      "$<TARGET_FILE:weavec>" --cc "$<TARGET_FILE:weavec-cc>")
  set_tests_properties(incremental-evaluation-rfc0020
                       PROPERTIES LABELS "integration" TIMEOUT 180)

  add_test(
    NAME checked-build-evaluation
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/scripts/checked-build-evaluation.py" --weavec
      "$<TARGET_FILE:weavec>" --cc "$<TARGET_FILE:weavec-cc>")
  set_tests_properties(checked-build-evaluation PROPERTIES LABELS "integration"
                                                           TIMEOUT 300)
  # Statement annotations require the matching LLVM toolchain, which may differ
  # from an older, unversioned `clang` on PATH (notably on Linux CI runners).
  add_test(
    NAME checked-evaluation
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/scripts/checked-evaluation.py" --weavec
      "$<TARGET_FILE:weavec>" --clang "${WEAVEC_CLANG_EXECUTABLE}")
  set_tests_properties(checked-evaluation PROPERTIES LABELS "integration"
                                                     TIMEOUT 180)
  add_test(
    NAME checked-evaluation-rfc0019
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/scripts/checked-evaluation.py" --weavec
      "$<TARGET_FILE:weavec>" --clang "${WEAVEC_CLANG_EXECUTABLE}" --manifest
      "${PROJECT_SOURCE_DIR}/test/evaluation/rfc0019/manifest.json")
  set_tests_properties(checked-evaluation-rfc0019
                       PROPERTIES LABELS "integration" TIMEOUT 300)
  add_test(
    NAME checked-evaluation-rfc0021
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/scripts/checked-evaluation.py" --weavec
      "$<TARGET_FILE:weavec>" --clang "${WEAVEC_CLANG_EXECUTABLE}" --manifest
      "${PROJECT_SOURCE_DIR}/test/evaluation/rfc0021/manifest.json")
  set_tests_properties(checked-evaluation-rfc0021
                       PROPERTIES LABELS "integration" TIMEOUT 300)
  add_test(
    NAME checked-evaluation-rfc0022
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/scripts/checked-evaluation.py" --weavec
      "$<TARGET_FILE:weavec>" --clang "${WEAVEC_CLANG_EXECUTABLE}" --manifest
      "${PROJECT_SOURCE_DIR}/test/evaluation/rfc0022/manifest.json")
  set_tests_properties(checked-evaluation-rfc0022
                       PROPERTIES LABELS "integration" TIMEOUT 300)
  foreach(kind IN ITEMS transport adversarial)
    add_test(
      NAME checked-${kind}-rfc0022
      COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/scripts/checked-evaluation.py" --weavec
        "$<TARGET_FILE:weavec>" --clang "${WEAVEC_CLANG_EXECUTABLE}" --manifest
        "${PROJECT_SOURCE_DIR}/test/evaluation/rfc0022/${kind}-manifest.json")
    set_tests_properties(checked-${kind}-rfc0022
                         PROPERTIES LABELS "integration" TIMEOUT 300)
  endforeach()
  add_test(
    NAME checked-objects-rfc0022
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/scripts/checked-memory-build.py" --cc
      "$<TARGET_FILE:weavec-cc>" --manifest
      "${PROJECT_SOURCE_DIR}/test/evaluation/rfc0022/transport-manifest.json")
  set_tests_properties(checked-objects-rfc0022 PROPERTIES LABELS "integration"
                                                          TIMEOUT 300)
  add_test(
    NAME checked-evaluation-rfc0023
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/scripts/checked-evaluation.py" --weavec
      "$<TARGET_FILE:weavec>" --clang "${WEAVEC_CLANG_EXECUTABLE}" --manifest
      "${PROJECT_SOURCE_DIR}/test/evaluation/rfc0023/manifest.json")
  set_tests_properties(checked-evaluation-rfc0023
                       PROPERTIES LABELS "integration" TIMEOUT 300)
  foreach(kind IN ITEMS transport adversarial audit callback-audit)
    add_test(
      NAME checked-${kind}-rfc0023
      COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/scripts/checked-evaluation.py" --weavec
        "$<TARGET_FILE:weavec>" --clang "${WEAVEC_CLANG_EXECUTABLE}" --manifest
        "${PROJECT_SOURCE_DIR}/test/evaluation/rfc0023/${kind}/manifest.json")
    set_tests_properties(checked-${kind}-rfc0023
                         PROPERTIES LABELS "integration" TIMEOUT 300)
  endforeach()
  add_test(
    NAME checked-objects-rfc0023
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/scripts/checked-containers.py" --population
      objects --weavec "$<TARGET_FILE:weavec>" --cc "$<TARGET_FILE:weavec-cc>"
      --clang "${WEAVEC_CLANG_EXECUTABLE}" --output
      "${CMAKE_CURRENT_BINARY_DIR}/rfc0023-objects")
  set_tests_properties(checked-objects-rfc0023 PROPERTIES LABELS "integration"
                                                          TIMEOUT 300)
  add_test(
    NAME checked-cache-rfc0023
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/scripts/checked-containers.py" --population cache
      --weavec "$<TARGET_FILE:weavec>" --cc "$<TARGET_FILE:weavec-cc>" --clang
      "${WEAVEC_CLANG_EXECUTABLE}" --output
      "${CMAKE_CURRENT_BINARY_DIR}/rfc0023-cache")
  set_tests_properties(checked-cache-rfc0023 PROPERTIES LABELS "integration"
                                                        TIMEOUT 180)
  add_test(
    NAME checked-transport-rfc0019
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/scripts/checked-evaluation.py" --weavec
      "$<TARGET_FILE:weavec>" --clang "${WEAVEC_CLANG_EXECUTABLE}" --manifest
      "${PROJECT_SOURCE_DIR}/test/evaluation/rfc0019/transport-manifest.json")
  set_tests_properties(checked-transport-rfc0019
                       PROPERTIES LABELS "integration" TIMEOUT 300)
  add_test(
    NAME checked-objects-rfc0019
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/scripts/checked-memory-build.py" --cc
      "$<TARGET_FILE:weavec-cc>")
  set_tests_properties(checked-objects-rfc0019 PROPERTIES LABELS "integration"
                                                          TIMEOUT 300)
  add_test(NAME checked-evaluation-harness
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/scripts/test_checked_evaluation.py")
  foreach(population source transport)
    if(population STREQUAL "source")
      set(runtime_manifest
          "${PROJECT_SOURCE_DIR}/test/evaluation/rfc0024/manifest.json")
    else()
      set(runtime_manifest
          "${PROJECT_SOURCE_DIR}/test/evaluation/rfc0024/transport/manifest.json"
      )
    endif()
    add_test(
      NAME checked-${population}-rfc0024
      COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/scripts/checked-evaluation.py" --weavec
        "$<TARGET_FILE:weavec>" --clang "${WEAVEC_CLANG_EXECUTABLE}" --manifest
        "${runtime_manifest}")
    set_tests_properties(checked-${population}-rfc0024
                         PROPERTIES LABELS "integration" TIMEOUT 300)
  endforeach()
  foreach(population objects cache)
    add_test(
      NAME checked-${population}-rfc0024
      COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/scripts/checked-runtime.py" --population
        "${population}" --weavec "$<TARGET_FILE:weavec>" --cc
        "$<TARGET_FILE:weavec-cc>" --output
        "${CMAKE_CURRENT_BINARY_DIR}/rfc0024-${population}")
    set_tests_properties(checked-${population}-rfc0024
                         PROPERTIES LABELS "integration" TIMEOUT 300)
  endforeach()
  foreach(population source transport regressions objects cache)
    add_test(
      NAME checked-${population}-rfc0025
      COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/scripts/checked-cases.py" --population
        "${population}" --weavec "$<TARGET_FILE:weavec>" --cc
        "$<TARGET_FILE:weavec-cc>" --output
        "${CMAKE_CURRENT_BINARY_DIR}/rfc0025-${population}")
    set_tests_properties(checked-${population}-rfc0025
                         PROPERTIES LABELS "integration" TIMEOUT 300)
  endforeach()
  set_tests_properties(checked-evaluation-harness PROPERTIES LABELS "unit")
  foreach(population source transport objects cache oracle)
    add_test(
      NAME checked-${population}-rfc0026
      COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/scripts/checked-buffers.py" --population
        "${population}" --weavec "$<TARGET_FILE:weavec>" --cc
        "$<TARGET_FILE:weavec-cc>" --output
        "${CMAKE_CURRENT_BINARY_DIR}/rfc0026-${population}")
    set_tests_properties(checked-${population}-rfc0026
                         PROPERTIES LABELS "integration" TIMEOUT 600)
  endforeach()
  foreach(population source transport objects cache oracle scaling)
    add_test(
      NAME checked-${population}-rfc0027
      COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/scripts/checked-recursive.py" --population
        "${population}" --weavec "$<TARGET_FILE:weavec>" --cc
        "$<TARGET_FILE:weavec-cc>" --clang "${WEAVEC_CLANG_EXECUTABLE}"
        --output "${CMAKE_CURRENT_BINARY_DIR}/rfc0027-${population}")
    set_tests_properties(checked-${population}-rfc0027
                         PROPERTIES LABELS "integration" TIMEOUT 600)
  endforeach()
  foreach(population source objects cache regressions regression-objects)
    add_test(
      NAME checked-${population}-rfc0028
      COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/scripts/checked-opaque-interfaces.py"
        --population "${population}" --weavec "$<TARGET_FILE:weavec>" --cc
        "$<TARGET_FILE:weavec-cc>" --clang "${WEAVEC_CLANG_EXECUTABLE}"
        --output "${CMAKE_CURRENT_BINARY_DIR}/rfc0028-${population}")
    set_tests_properties(checked-${population}-rfc0028
                         PROPERTIES LABELS "integration" TIMEOUT 600)
  endforeach()
  foreach(
    population
    source
    transport
    serializer
    readers
    offsets
    traversal
    construction
    mutual-construction
    construction-helpers
    output-construction
    reader-construction
    mutable-reader-construction
    numeric-input
    cursor-readers
    cursor-cache
    byte-cache
    payload-cache
    cursor-oracle
    recursive-contexts
    recursive-state-cases
    recursive-output-cases
    mutual-cases
    record-arrays
    writer-return-aliases
    writer-return-expressions
    writer-position-bounds
    writer-forwarding
    container-local-frames
    container-call-frames
    mixed-allocation-ledger
    reallocation-ledger-failures
    scalar-write-offsets
    bounded-string-cursor
    cast-reader-intervals
    reader-index-loops
    paired-reader-counters
    character-pointer-slots
    pointer-reader-offsets
    initialized-spans
    span-outputs
    reverse-byte-writes
    span-counts
    span-count-joins
    local-callee-copies
    reverse-initialization
    conditional-count-arguments
    initialized-advance
    advance-outcomes
    counter-reset-ranges-reviewed
    guarded-advance
    guarded-cursor-bounds
    span-advances
    paired-cursor-loops
    independent-cursors
    fixed-span-steps
    helper-cursor-pairs
    byte-cursor-content
    byte-cursor-static
    byte-cursor-forwarding
    byte-cursor-frames
    byte-global-frames
    byte-helper-frames
    byte-comparisons
    byte-loop-partitions
    byte-switch-partitions-reviewed
    byte-callee-frames
    comparison-container-frames-reviewed
    numeric-container-frames
    temporary-release-frames
    container-outcome-frames
    string-length-copies
    container-alias-outputs
    floating-call-premises
    singleton-link-ownership
    pointer-difference-sizes
    payload-publication
    payload-write-frames
    payload-early-exits
    payload-reader-returns
    payload-relocation
    numeric-text
    numeric-scans
    numeric-scan-locals
    numeric-short-circuit
    cursor-envelope-joins-reviewed
    pointer-count-readers
    shifted-pointee-facts
    buffer-entry-intervals-reviewed
    in-place-extension
    anonymous-private-state
    container-value-guards
    reader-index-oracle
    zero-counters
    zero-frames
    combined-callback-cases
    recursive-cases
    recursive-writer-reviewed
    writer-transport
    writer-cache
    writer-oracle
    output-transport
    output-cache
    construction-oracle
    corpus-regressions
    recursive-transport
    recursive-cache
    cache)
    add_test(
      NAME checked-${population}-rfc0029
      COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/scripts/checked-workflows.py" --population
        "${population}" --weavec "$<TARGET_FILE:weavec>" --cc
        "$<TARGET_FILE:weavec-cc>" --clang "${WEAVEC_CLANG_EXECUTABLE}"
        --output "${CMAKE_CURRENT_BINARY_DIR}/rfc0029-${population}")
    set_tests_properties(checked-${population}-rfc0029
                         PROPERTIES LABELS "integration" TIMEOUT 600)
  endforeach()
  # Each immutable object population keeps the same 600-second deadline.
  # Combining every RFC 0029 population into one test exceeded that deadline
  # even when all individual checked links completed successfully.
  foreach(
    population
    transport
    serializer
    readers
    construction
    mutual-construction
    construction-helpers
    output-construction
    reader-construction
    mutable-reader-construction
    numeric-input
    cursor-readers
    recursive-contexts
    recursive-state-cases
    recursive-output-cases
    record-arrays
    writer-return-aliases
    writer-return-expressions
    writer-position-bounds
    writer-forwarding
    container-local-frames
    container-call-frames
    mixed-allocation-ledger
    reallocation-ledger-failures
    scalar-write-offsets
    bounded-string-cursor
    cast-reader-intervals
    reader-index-loops
    paired-reader-counters
    character-pointer-slots
    pointer-reader-offsets
    initialized-spans
    span-outputs
    reverse-byte-writes
    span-counts
    span-count-joins
    local-callee-copies
    reverse-initialization
    conditional-count-arguments
    initialized-advance
    advance-outcomes
    counter-reset-ranges-reviewed
    guarded-advance
    guarded-cursor-bounds
    span-advances
    paired-cursor-loops
    independent-cursors
    fixed-span-steps
    helper-cursor-pairs
    byte-cursor-content
    byte-cursor-static
    byte-cursor-forwarding
    byte-cursor-frames
    byte-global-frames
    byte-helper-frames
    byte-comparisons
    byte-loop-partitions
    byte-switch-partitions-reviewed
    byte-callee-frames
    comparison-container-frames-reviewed
    numeric-container-frames
    temporary-release-frames
    container-outcome-frames
    string-length-copies
    container-alias-outputs
    floating-call-premises
    singleton-link-ownership
    pointer-difference-sizes
    payload-publication
    payload-write-frames
    payload-early-exits
    payload-reader-returns
    payload-relocation
    numeric-text
    numeric-scans
    numeric-scan-locals
    numeric-short-circuit
    cursor-envelope-joins-reviewed
    pointer-count-readers
    shifted-pointee-facts
    buffer-entry-intervals-reviewed
    in-place-extension
    anonymous-private-state
    container-value-guards
    zero-counters
    zero-frames
    combined-callback-cases
    mutual-cases
    recursive-cases
    recursive-writer-reviewed
    writer-transport
    corpus-regressions
    recursive-transport
    output-transport)
    add_test(
      NAME checked-objects-${population}-rfc0029
      COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/scripts/checked-workflows.py" --population
        objects --object-population "${population}" --weavec
        "$<TARGET_FILE:weavec>" --cc "$<TARGET_FILE:weavec-cc>" --clang
        "${WEAVEC_CLANG_EXECUTABLE}" --output
        "${CMAKE_CURRENT_BINARY_DIR}/rfc0029-objects-${population}")
    set_tests_properties(checked-objects-${population}-rfc0029
                         PROPERTIES LABELS "integration" TIMEOUT 600)
  endforeach()
  add_test(NAME recall-harness
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/scripts/test_recall.py")
  set_tests_properties(recall-harness PROPERTIES LABELS "unit")
  add_test(NAME evaluation-harness
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/scripts/test_evaluate.py")
  set_tests_properties(evaluation-harness PROPERTIES LABELS "unit")
  add_test(NAME corpus-harness
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/scripts/test_corpus.py")
  set_tests_properties(corpus-harness PROPERTIES LABELS "unit")
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
add_subdirectory(cases)
