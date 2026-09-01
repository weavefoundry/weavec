# Install rules and CMake package export for WeaveC.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

get_property(_weavec_targets GLOBAL PROPERTY WEAVEC_EXPORTED_TARGETS)

install(
  TARGETS ${_weavec_targets} weavec_options weavec_llvm
  EXPORT WeaveCTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT WeaveC_Runtime
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          COMPONENT WeaveC_Runtime
          NAMELINK_COMPONENT WeaveC_Development
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT WeaveC_Development
  INCLUDES
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Public C++ headers.
install(
  DIRECTORY "${PROJECT_SOURCE_DIR}/include/weavec"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  COMPONENT WeaveC_Development
  FILES_MATCHING
  PATTERN "*.h"
  PATTERN "*.in" EXCLUDE)
install(
  DIRECTORY "${WEAVEC_GENERATED_INCLUDE_DIR}/weavec"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  COMPONENT WeaveC_Development)

# Resource directory: the C-facing annotation header consumed by user code.
install(
  DIRECTORY "${WEAVEC_BUILD_RESOURCE_DIR}/include"
  DESTINATION "${WEAVEC_RESOURCE_DIR_RELATIVE}"
  COMPONENT WeaveC_Runtime)

# CMake package config.
set(_weavec_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/WeaveC")

install(
  EXPORT WeaveCTargets
  NAMESPACE weavec::
  DESTINATION "${_weavec_cmake_dir}"
  COMPONENT WeaveC_Development)

configure_package_config_file(
  "${PROJECT_SOURCE_DIR}/cmake/WeaveCConfig.cmake.in"
  "${PROJECT_BINARY_DIR}/WeaveCConfig.cmake"
  INSTALL_DESTINATION "${_weavec_cmake_dir}"
  PATH_VARS WEAVEC_RESOURCE_DIR_RELATIVE)

write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/WeaveCConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(
  FILES "${PROJECT_BINARY_DIR}/WeaveCConfig.cmake"
        "${PROJECT_BINARY_DIR}/WeaveCConfigVersion.cmake"
  DESTINATION "${_weavec_cmake_dir}"
  COMPONENT WeaveC_Development)

install(
  FILES "${PROJECT_SOURCE_DIR}/LICENSE"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/weavec
  COMPONENT WeaveC_Runtime)

# Packaging via CPack (e.g. `cpack -G TGZ`).
set(CPACK_PACKAGE_NAME "weavec")
set(CPACK_PACKAGE_VENDOR "WeaveFoundry")
set(CPACK_PACKAGE_VERSION "${WEAVEC_VERSION_STRING}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ")
set(CPACK_STRIP_FILES ON)
include(CPack)
