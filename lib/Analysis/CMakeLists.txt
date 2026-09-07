# weavec::Analysis -- bridges Clang's AST to the core ownership model.
weavec_add_library(
  Analysis
  SOURCES Allocators.cpp
          Annotations.cpp
          Builtins.cpp
          ClangLocation.cpp
          Dataflow.cpp
          DataflowArrays.cpp
          DataflowArrayMemory.cpp
          DataflowArrayRanges.cpp
          DataflowArrayCleanup.cpp
          DataflowArrayFill.cpp
          DataflowHeap.cpp
          DataflowMemory.cpp
          DataflowViews.cpp
          DataflowCallbacks.cpp
          CallbackSummaries.cpp
          DataflowValues.cpp
          DataflowSizedFields.cpp
          DataflowStrings.cpp
          FunctionAnalysis.cpp
          PlaceBuilder.cpp
          ProgramDatabase.cpp
          Summaries.cpp
          TranslationUnitAnalysis.cpp
  PUBLIC_DEPS weavec::Core
  USES_LLVM)

weavec_link_llvm(weavecAnalysis Support)
weavec_link_clang(weavecAnalysis clangAnalysis clangAST clangBasic clangLex)
