# Sanitizer support.
#
# Set WEAVEC_SANITIZERS to a semicolon-separated list drawn from:
#   address, undefined, thread, memory, leak
#
# Usage:
#   weavec_apply_sanitizers(<interface-or-regular-target>)

function(weavec_apply_sanitizers target)
  if(NOT WEAVEC_SANITIZERS)
    return()
  endif()

  if(MSVC)
    message(
      WARNING "WEAVEC_SANITIZERS is only supported with Clang/GCC; ignoring")
    return()
  endif()

  get_target_property(_type ${target} TYPE)
  if(_type STREQUAL "INTERFACE_LIBRARY")
    set(_scope INTERFACE)
  else()
    set(_scope PRIVATE)
  endif()

  set(_valid address undefined thread memory leak)
  set(_flags "")
  foreach(_san IN LISTS WEAVEC_SANITIZERS)
    if(NOT _san IN_LIST _valid)
      message(FATAL_ERROR "Unknown sanitizer '${_san}' in WEAVEC_SANITIZERS. "
                          "Valid values: ${_valid}")
    endif()
    list(APPEND _flags "-fsanitize=${_san}")
  endforeach()

  if("thread" IN_LIST WEAVEC_SANITIZERS
     AND ("address" IN_LIST WEAVEC_SANITIZERS OR "memory" IN_LIST
                                                 WEAVEC_SANITIZERS))
    message(FATAL_ERROR "ThreadSanitizer cannot be combined with ASan/MSan")
  endif()

  list(APPEND _flags -fno-omit-frame-pointer -fno-optimize-sibling-calls)
  if("undefined" IN_LIST WEAVEC_SANITIZERS)
    list(APPEND _flags -fno-sanitize-recover=undefined)
  endif()

  target_compile_options(${target} ${_scope} ${_flags})
  target_link_options(${target} ${_scope} ${_flags})
endfunction()
