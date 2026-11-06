# RFC 0038 section 6: on Darwin a self-contained toolchain carries the shared
# libraries weavec-cc loads (LLVM's, Clang's, and what they need outside the
# system: Homebrew's z3 and zstd) in <prefix>/lib/weavec/llvm, and finds them
# through an rpath relative to its executable. Run at install time
# (`install(SCRIPT)`) with WEAVEC_BUNDLE_EXECUTABLE set to the installed
# weavec-cc.
#
# Each library outside /usr/lib and /System is copied, its install name made
# @rpath/<name>, each reference to another copied library rewritten to
# @rpath/<name>, and every changed file signed again (ad hoc), which arm64
# requires of an image that loads.

cmake_policy(VERSION 3.21)
set(_bin "$ENV{DESTDIR}${WEAVEC_BUNDLE_EXECUTABLE}")
get_filename_component(_prefix "${_bin}" DIRECTORY)
get_filename_component(_prefix "${_prefix}" DIRECTORY)
set(_dest "${_prefix}/lib/weavec/llvm")
file(MAKE_DIRECTORY "${_dest}")

# The libraries an image names that are not the system's: (reference, path).
function(_weavec_dependencies image out)
  execute_process(COMMAND otool -L "${image}" OUTPUT_VARIABLE _listing)
  string(REPLACE "\n" ";" _lines "${_listing}")
  list(REMOVE_AT _lines 0)
  set(_found)
  foreach(_line IN LISTS _lines)
    string(STRIP "${_line}" _line)
    string(REGEX REPLACE " \\(compatibility.*$" "" _reference "${_line}")
    if(_reference STREQUAL "" OR _reference MATCHES "^(/usr/lib/|/System/)")
      continue()
    endif()
    list(APPEND _found "${_reference}")
  endforeach()
  set(${out}
      ${_found}
      PARENT_SCOPE)
endfunction()

# The file a reference names: absolute, or @rpath/@loader_path relative to
# the library that names it (Homebrew's libclang-cpp names @rpath/libLLVM).
function(_weavec_resolve reference from out)
  get_filename_component(_from_dir "${from}" DIRECTORY)
  if(reference MATCHES "^@(rpath|loader_path)/(.*)$")
    set(_name "${CMAKE_MATCH_2}")
    foreach(_dir "${_from_dir}" "${WEAVEC_BUNDLE_LLVM_LIBRARY_DIR}")
      if(EXISTS "${_dir}/${_name}")
        set(${out}
            "${_dir}/${_name}"
            PARENT_SCOPE)
        return()
      endif()
    endforeach()
    set(${out}
        ""
        PARENT_SCOPE)
  else()
    set(${out}
        "${reference}"
        PARENT_SCOPE)
  endif()
endfunction()

set(_queue "${_bin}")
set(_done)
while(_queue)
  list(POP_FRONT _queue _image)
  if(_image IN_LIST _done)
    continue()
  endif()
  list(APPEND _done "${_image}")
  _weavec_dependencies("${_image}" _references)
  foreach(_reference IN LISTS _references)
    if(_image STREQUAL _bin AND _reference MATCHES "^@rpath/libweavec_rt")
      continue()
    endif()
    _weavec_resolve("${_reference}" "${_image}" _path)
    if(NOT _path OR NOT EXISTS "${_path}")
      if(NOT _reference MATCHES "^@rpath/")
        message(FATAL_ERROR "weavec: cannot find ${_reference}, which "
                            "${_image} loads")
      endif()
      continue()
    endif()
    get_filename_component(_real "${_path}" REALPATH)
    get_filename_component(_name "${_reference}" NAME)
    set(_copy "${_dest}/${_name}")
    if(NOT EXISTS "${_copy}")
      file(COPY_FILE "${_real}" "${_copy}")
      file(
        CHMOD
        "${_copy}"
        PERMISSIONS
        OWNER_READ
        OWNER_WRITE
        GROUP_READ
        WORLD_READ)
      execute_process(COMMAND install_name_tool -id "@rpath/${_name}" "${_copy}"
                      ERROR_QUIET)
      list(APPEND _queue "${_copy}")
    endif()
    execute_process(COMMAND install_name_tool -change "${_reference}"
                            "@rpath/${_name}" "${_image}" ERROR_QUIET)
  endforeach()
endwhile()

# weavec-cc finds the copies, and its runtime, beside itself; each copy finds
# the others in its own directory.
execute_process(
  COMMAND install_name_tool -add_rpath "@executable_path/../lib/weavec/llvm"
          "${_bin}" ERROR_QUIET)
file(GLOB _copies "${_dest}/*.dylib")
foreach(_copy IN LISTS _copies)
  execute_process(COMMAND install_name_tool -add_rpath "@loader_path" "${_copy}"
                  ERROR_QUIET)
endforeach()
foreach(_image IN LISTS _copies _bin)
  execute_process(
    COMMAND codesign --force --sign - "${_image}"
    RESULT_VARIABLE _signed
    ERROR_QUIET)
  if(NOT _signed EQUAL 0)
    message(FATAL_ERROR "weavec: cannot sign ${_image}")
  endif()
endforeach()
list(LENGTH _copies _count)
message(STATUS "weavec: ${_count} LLVM libraries bundled in ${_dest}")
