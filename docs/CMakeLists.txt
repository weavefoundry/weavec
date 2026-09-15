# Documentation targets (enabled with -DWEAVEC_BUILD_DOCS=ON).
#
# API reference via Doxygen when available; the Markdown docs in this
# directory need no build step.
find_package(Doxygen QUIET)

if(NOT DOXYGEN_FOUND)
  message(STATUS "Doxygen not found; `docs` target unavailable")
  return()
endif()

set(DOXYGEN_PROJECT_NAME "WeaveC")
set(DOXYGEN_PROJECT_NUMBER "${WEAVEC_VERSION_STRING}")
set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/api")
set(DOXYGEN_GENERATE_HTML YES)
set(DOXYGEN_GENERATE_LATEX NO)
set(DOXYGEN_EXTRACT_ALL YES)
set(DOXYGEN_RECURSIVE YES)
set(DOXYGEN_EXCLUDE_PATTERNS "*.in")
# Website dependencies and generated content are not C++ API documentation.
set(DOXYGEN_EXCLUDE
    "${CMAKE_CURRENT_SOURCE_DIR}/node_modules"
    "${CMAKE_CURRENT_SOURCE_DIR}/.generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/.astro"
    "${CMAKE_CURRENT_SOURCE_DIR}/dist"
    "${CMAKE_CURRENT_SOURCE_DIR}/test-results"
    "${CMAKE_CURRENT_SOURCE_DIR}/playwright-report")
set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "${PROJECT_SOURCE_DIR}/README.md")
set(DOXYGEN_WARN_IF_UNDOCUMENTED NO)
set(DOXYGEN_QUIET YES)

doxygen_add_docs(
  docs "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/README.md"
  "${CMAKE_CURRENT_SOURCE_DIR}" COMMENT "Generating API documentation")
