# weavec::Analysis -- bridges Clang's AST to the core ownership model.
weavec_add_library(
  Analysis
  SOURCES Allocators.cpp
          Annotations.cpp
          Builtins.cpp
          ClangLocation.cpp
          Dataflow.cpp
          FunctionAnalysis.cpp
          PlaceBuilder.cpp
          Summaries.cpp
          TranslationUnitAnalysis.cpp
  PUBLIC_DEPS weavec::Core
  USES_LLVM)

weavec_link_llvm(weavecAnalysis Support)
weavec_link_clang(weavecAnalysis clangAnalysis clangAST clangBasic clangLex)
