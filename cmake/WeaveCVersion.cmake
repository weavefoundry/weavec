# Computes version metadata for WeaveC, including the git revision when the
# source tree is a git checkout.
#
# Sets:
#   WEAVEC_VERSION_MAJOR / MINOR / PATCH
#   WEAVEC_VERSION_STRING     e.g. "0.1.0" or "0.1.0-dev"
#   WEAVEC_GIT_REVISION       short hash, or "unknown"
#   WEAVEC_GIT_DIRTY          ON if the working tree has local modifications

set(WEAVEC_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(WEAVEC_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(WEAVEC_VERSION_PATCH ${PROJECT_VERSION_PATCH})

# Pre-release suffix; cleared when cutting a release.
set(WEAVEC_VERSION_SUFFIX
    "dev"
    CACHE STRING "Version suffix appended to the release version (e.g. dev)")
mark_as_advanced(WEAVEC_VERSION_SUFFIX)

set(WEAVEC_VERSION_STRING "${PROJECT_VERSION}")
if(WEAVEC_VERSION_SUFFIX)
  string(APPEND WEAVEC_VERSION_STRING "-${WEAVEC_VERSION_SUFFIX}")
endif()

set(WEAVEC_GIT_REVISION "unknown")
set(WEAVEC_GIT_DIRTY OFF)

find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    RESULT_VARIABLE _weavec_git_result
    OUTPUT_VARIABLE _weavec_git_rev
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_weavec_git_result EQUAL 0 AND _weavec_git_rev)
    set(WEAVEC_GIT_REVISION "${_weavec_git_rev}")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      OUTPUT_VARIABLE _weavec_git_status
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_weavec_git_status)
      set(WEAVEC_GIT_DIRTY ON)
    endif()
  endif()
  # Re-run configure when HEAD moves so the embedded revision stays current.
  set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/.git/HEAD")
endif()

if(WEAVEC_GIT_DIRTY)
  set(WEAVEC_GIT_DIRTY_INT 1)
else()
  set(WEAVEC_GIT_DIRTY_INT 0)
endif()
