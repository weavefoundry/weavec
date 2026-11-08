# The `weavec-cc` compiler driver (RFC 0005).
weavec_add_executable(
  weavec-cc
  SOURCES main.cpp
  DEPS weavec::Frontend
  USES_LLVM)

weavec_link_llvm(weavec-cc Support)
weavec_link_clang(weavec-cc clangDriver clangBasic)

# RFC 0039 §1: `weavec-c++`, the C++ driver, is a link to `weavec-cc`, which
# chooses Clang's g++ mode by the name it is invoked by.
add_custom_command(
  TARGET weavec-cc
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E create_symlink weavec-cc
          "$<TARGET_FILE_DIR:weavec-cc>/weavec-c++"
  VERBATIM)
