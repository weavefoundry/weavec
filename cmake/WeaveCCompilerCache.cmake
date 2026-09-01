# Enables ccache or sccache as a compiler launcher when available.
#
# Controlled by WEAVEC_ENABLE_CCACHE. Respects an explicitly provided
# CMAKE_CXX_COMPILER_LAUNCHER.

set(WEAVEC_COMPILER_CACHE_SUMMARY "none")

if(NOT WEAVEC_ENABLE_CCACHE)
  return()
endif()

if(CMAKE_CXX_COMPILER_LAUNCHER OR CMAKE_C_COMPILER_LAUNCHER)
  set(WEAVEC_COMPILER_CACHE_SUMMARY
      "${CMAKE_CXX_COMPILER_LAUNCHER} (user-provided)")
  return()
endif()

find_program(WEAVEC_CCACHE_EXE NAMES ccache sccache)
mark_as_advanced(WEAVEC_CCACHE_EXE)

if(WEAVEC_CCACHE_EXE)
  set(CMAKE_C_COMPILER_LAUNCHER
      "${WEAVEC_CCACHE_EXE}"
      CACHE FILEPATH "Compiler launcher" FORCE)
  set(CMAKE_CXX_COMPILER_LAUNCHER
      "${WEAVEC_CCACHE_EXE}"
      CACHE FILEPATH "Compiler launcher" FORCE)
  set(WEAVEC_COMPILER_CACHE_SUMMARY "${WEAVEC_CCACHE_EXE}")
endif()
