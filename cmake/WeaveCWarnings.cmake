# Strict compiler warnings for WeaveC targets.
#
# Usage:
#   weavec_apply_warnings(<interface-or-regular-target>)
#
# LLVM/Clang headers are always added as SYSTEM includes so that their
# warnings do not leak into our build; see WeaveCLLVM.cmake.

function(weavec_apply_warnings target)
  get_target_property(_type ${target} TYPE)
  if(_type STREQUAL "INTERFACE_LIBRARY")
    set(_scope INTERFACE)
  else()
    set(_scope PRIVATE)
  endif()

  set(_gnu_like_warnings
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wcast-align
      -Wcast-qual
      -Wconversion
      -Wsign-conversion
      -Wdouble-promotion
      -Wformat=2
      -Wimplicit-fallthrough
      -Wmissing-declarations
      -Wnon-virtual-dtor
      -Wnull-dereference
      -Wold-style-cast
      -Woverloaded-virtual
      -Wundef
      -Wunused
      -Wno-unused-parameter)

  set(_clang_warnings
      ${_gnu_like_warnings}
      -Wcovered-switch-default
      -Wdocumentation
      -Wextra-semi
      -Wheader-hygiene
      -Winconsistent-missing-override
      -Wnewline-eof
      -Wstring-conversion
      -Wthread-safety
      -Wunreachable-code
      -Wzero-as-null-pointer-constant)

  set(_gcc_warnings
      ${_gnu_like_warnings}
      -Wduplicated-branches
      -Wduplicated-cond
      -Wlogical-op
      -Wuseless-cast
      -Wextra-semi
      # GCC's maybe-uninitialized is notoriously noisy on LLVM's ADT.
      -Wno-maybe-uninitialized)

  set(_msvc_warnings
      /W4
      /permissive-
      /w14242
      /w14254
      /w14263
      /w14265
      /w14287
      /we4289
      /w14296
      /w14311
      /w14545
      /w14546
      /w14547
      /w14549
      /w14555
      /w14619
      /w14640
      /w14826
      /w14905
      /w14906
      /w14928)

  if(WEAVEC_WARNINGS_AS_ERRORS)
    list(APPEND _clang_warnings -Werror)
    list(APPEND _gcc_warnings -Werror)
    list(APPEND _msvc_warnings /WX)
  endif()

  target_compile_options(
    ${target}
    ${_scope}
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:${_clang_warnings}>
    $<$<CXX_COMPILER_ID:GNU>:${_gcc_warnings}>
    $<$<CXX_COMPILER_ID:MSVC>:${_msvc_warnings}>)
endfunction()
