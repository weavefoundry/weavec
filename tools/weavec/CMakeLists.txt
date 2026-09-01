# The `weavec` command-line tool.
weavec_add_executable(
  weavec
  SOURCES main.cpp
  DEPS weavec::Frontend
  USES_LLVM)

weavec_link_llvm(weavec Support)
weavec_link_clang(weavec clangTooling clangBasic)
