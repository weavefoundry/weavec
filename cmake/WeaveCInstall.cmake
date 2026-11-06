# Install rules (RFC 0038, section 6): a toolchain tree that may be moved
# anywhere, since weavec-cc finds everything relative to its own executable.
#
#   bin/weavec-cc
#   lib/weavec/                     the runtime (runtime/CMakeLists.txt) and
#                                   include/weavec.h; on Darwin, llvm/ with
#                                   the LLVM libraries weavec-cc loads
#   lib/clang/<llvm-major>/include  Clang's resource headers
#   lib/clang/<llvm-major>/lib/...  the compiler-rt builtins Clang's driver
#                                   links (Darwin)
#   lib/libLTO.dylib                LLVM's, for -flto links (Darwin)
#   share/doc/weavec/               LICENSE
#
# `cpack` packs it as weavec-<version>-<os>-<arch>.tar.xz.

include(GNUInstallDirs)

install(TARGETS weavec-cc RUNTIME DESTINATION bin COMPONENT WeaveC_Runtime)
# Darwin: the LLVM libraries weavec-cc loads, beside it.
if(APPLE AND WEAVEC_SELF_CONTAINED)
  install(
    CODE "set(WEAVEC_BUNDLE_EXECUTABLE \"\${CMAKE_INSTALL_PREFIX}/bin/weavec-cc\")
          set(WEAVEC_BUNDLE_LLVM_LIBRARY_DIR \"${LLVM_LIBRARY_DIR}\")"
    COMPONENT WeaveC_Runtime)
  install(SCRIPT "${PROJECT_SOURCE_DIR}/cmake/WeaveCBundleLLVM.cmake"
          COMPONENT WeaveC_Runtime)
endif()

# The C-facing annotation header consumed by user code.
install(
  DIRECTORY "${WEAVEC_BUILD_RESOURCE_DIR}/include"
  DESTINATION "${WEAVEC_RESOURCE_DIR_RELATIVE}"
  COMPONENT WeaveC_Runtime)

# Clang's resource directory, where Clang's driver looks for it from the
# executable: <prefix>/lib/clang/<major>.
if(WEAVEC_CLANG_RESOURCES)
  set(_weavec_clang_dest "lib/clang/${LLVM_VERSION_MAJOR}")
  install(
    DIRECTORY "${WEAVEC_CLANG_RESOURCES}/include"
    DESTINATION "${_weavec_clang_dest}"
    COMPONENT WeaveC_Runtime)
  file(GLOB _weavec_builtins
       "${WEAVEC_CLANG_RESOURCES}/lib/darwin/libclang_rt.osx.a"
       "${WEAVEC_CLANG_RESOURCES}/lib/*/libclang_rt.builtins*.a")
  foreach(_weavec_builtin IN LISTS _weavec_builtins)
    file(RELATIVE_PATH _weavec_relative "${WEAVEC_CLANG_RESOURCES}"
         "${_weavec_builtin}")
    get_filename_component(_weavec_relative_dir "${_weavec_relative}" DIRECTORY)
    install(
      FILES "${_weavec_builtin}"
      DESTINATION "${_weavec_clang_dest}/${_weavec_relative_dir}"
      COMPONENT WeaveC_Runtime)
  endforeach()
endif()

install(
  FILES "${PROJECT_SOURCE_DIR}/LICENSE"
  DESTINATION share/doc/weavec
  COMPONENT WeaveC_Runtime)

# Release archives: weavec-<version>-<os>-<arch>.tar.xz.
if(APPLE)
  set(_weavec_os darwin)
else()
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" _weavec_os)
endif()
set(_weavec_arch "${CMAKE_SYSTEM_PROCESSOR}")
if(_weavec_arch MATCHES "^(aarch64|arm64|ARM64)$")
  if(APPLE)
    set(_weavec_arch arm64)
  else()
    set(_weavec_arch aarch64)
  endif()
elseif(_weavec_arch MATCHES "^(x86_64|AMD64|amd64)$")
  set(_weavec_arch x86_64)
endif()
set(CPACK_PACKAGE_NAME "weavec")
set(CPACK_PACKAGE_VENDOR "WeaveFoundry")
set(CPACK_PACKAGE_VERSION "${WEAVEC_VERSION_STRING}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TXZ")
set(CPACK_PACKAGE_FILE_NAME
    "weavec-${WEAVEC_VERSION_STRING}-${_weavec_os}-${_weavec_arch}")
set(CPACK_STRIP_FILES ON)
include(CPack)
