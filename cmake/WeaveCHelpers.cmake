# Target-creation helpers that apply project conventions uniformly.
#
#   weavec_add_library(<name> [STATIC|SHARED|OBJECT]
#                      SOURCES <src...>
#                      [PUBLIC_DEPS <target...>] [PRIVATE_DEPS <target...>]
#                      [USES_LLVM])
#
#   weavec_add_executable(<name> SOURCES <src...> [DEPS <target...>]
#                         [USES_LLVM])
#
# Every target created here gets weavec::options, the public include dirs,
# an ALIAS of the form weavec::<Component>, and is added to the export set.

include(GNUInstallDirs)

set_property(GLOBAL PROPERTY WEAVEC_EXPORTED_TARGETS "")

function(_weavec_common_setup target)
  target_link_libraries(${target} PRIVATE weavec::options)
  target_include_directories(
    ${target}
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
           $<BUILD_INTERFACE:${WEAVEC_GENERATED_INCLUDE_DIR}>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  set_property(GLOBAL APPEND PROPERTY WEAVEC_EXPORTED_TARGETS ${target})
endfunction()

function(weavec_add_library name)
  cmake_parse_arguments(ARG "STATIC;SHARED;OBJECT;USES_LLVM" ""
                        "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "weavec_add_library: unknown args "
                        "${ARG_UNPARSED_ARGUMENTS}")
  endif()

  set(_kind "")
  if(ARG_STATIC)
    set(_kind STATIC)
  elseif(ARG_SHARED)
    set(_kind SHARED)
  elseif(ARG_OBJECT)
    set(_kind OBJECT)
  endif()

  set(_target weavec${name})
  add_library(${_target} ${_kind} ${ARG_SOURCES})
  add_library(weavec::${name} ALIAS ${_target})
  _weavec_common_setup(${_target})

  set_target_properties(
    ${_target}
    PROPERTIES EXPORT_NAME ${name}
               OUTPUT_NAME weavec${name}
               VERSION ${PROJECT_VERSION}
               SOVERSION ${PROJECT_VERSION_MAJOR})

  if(ARG_PUBLIC_DEPS)
    target_link_libraries(${_target} PUBLIC ${ARG_PUBLIC_DEPS})
  endif()
  if(ARG_PRIVATE_DEPS)
    target_link_libraries(${_target} PRIVATE ${ARG_PRIVATE_DEPS})
  endif()
  if(ARG_USES_LLVM)
    target_link_libraries(${_target} PUBLIC weavec::llvm)
  endif()
endfunction()

function(weavec_add_executable name)
  cmake_parse_arguments(ARG "USES_LLVM" "" "SOURCES;DEPS" ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "weavec_add_executable: unknown args "
                        "${ARG_UNPARSED_ARGUMENTS}")
  endif()

  add_executable(${name} ${ARG_SOURCES})
  add_executable(weavec::${name} ALIAS ${name})
  _weavec_common_setup(${name})
  set_target_properties(
    ${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
                       EXPORT_NAME ${name})

  if(ARG_DEPS)
    target_link_libraries(${name} PRIVATE ${ARG_DEPS})
  endif()
  if(ARG_USES_LLVM)
    target_link_libraries(${name} PRIVATE weavec::llvm)
  endif()
endfunction()
