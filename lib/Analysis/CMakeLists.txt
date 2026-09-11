# weavec::Analysis -- bridges Clang's AST to the core ownership model.
weavec_add_library(
  Analysis
  SOURCES Allocators.cpp
          Annotations.cpp
          Builtins.cpp
          ClangLocation.cpp
          Dataflow.cpp
          DataflowSafety.cpp
          DataflowObjectTypes.cpp
          DataflowCheckedCallbacks.cpp
          DataflowSafetyMemory.cpp
          DataflowSafetyCalls.cpp
          DataflowSafetyContracts.cpp
          DataflowSafetyLoops.cpp
          DataflowPointerOperations.cpp
          DataflowTraversalRelations.cpp
          DataflowCursors.cpp
          DataflowStringTraversal.cpp
          DataflowArrays.cpp
          DataflowArrayMemory.cpp
          DataflowArrayRanges.cpp
          DataflowArrayCleanup.cpp
          DataflowArrayFill.cpp
          DataflowIntegers.cpp
          DataflowIntegerExpressions.cpp
          DataflowIntegerStatements.cpp
          DataflowCheckedIntegers.cpp
          DataflowIntegerProofs.cpp
          DataflowNumericOutputs.cpp
          DataflowNumericInputs.cpp
          DataflowDynamicExtents.cpp
          DataflowHeap.cpp
          DataflowMemory.cpp
          DataflowViews.cpp
          DataflowCallbacks.cpp
          CallbackSummaries.cpp
          CallContextSummaries.cpp
          DataflowCallContext.cpp
          DataflowValues.cpp
          DataflowSizedFields.cpp
          DataflowStrings.cpp
          FunctionAnalysis.cpp
          PlaceBuilder.cpp
          ProgramDatabase.cpp
          Summaries.cpp
          SummaryDependencies.cpp
          TranslationUnitAnalysis.cpp
          DataflowLoopRequirements.cpp
          DataflowGuardCompleteness.cpp
  PUBLIC_DEPS weavec::Core
  USES_LLVM)

weavec_link_llvm(weavecAnalysis Support)
weavec_link_clang(weavecAnalysis clangAnalysis clangAST clangBasic clangLex)
