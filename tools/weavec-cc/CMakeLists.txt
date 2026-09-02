# The `weavec-cc` compiler driver (RFC 0005).
weavec_add_executable(
  weavec-cc
  SOURCES main.cpp
  DEPS weavec::Frontend
  USES_LLVM)

weavec_link_llvm(weavec-cc Support)
weavec_link_clang(weavec-cc clangDriver clangBasic)
