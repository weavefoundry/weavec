# weavec::Analysis -- bridges Clang's AST to the core ownership model.
weavec_add_library(
  Analysis
  SOURCES Allocators.cpp
          Annotations.cpp
          LibrarySummaries.cpp
          ClangLocation.cpp
          InterfaceTypes.cpp
          Dataflow.cpp
          DataflowIntervalProofs.cpp
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
          DataflowWitnesses.cpp
          DataflowLibraryRequirements.cpp
          DataflowLibraryEffects.cpp
          KindSeeding.cpp
          DataflowCallbacks.cpp
          DataflowUnknown.cpp
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
          KindTable.cpp
          AttributeReader.cpp
          KindInference.cpp
          KindInferenceCollect.cpp
          KindInferenceDump.cpp
          KindInferenceFields.cpp
          KindInferenceMustAccess.cpp
          SlotCollector.cpp
          Concurrency.cpp
          SiteCollector.cpp
          CheckPlanner.cpp
          BoundaryInvariants.cpp
          LedgerAdapter.cpp
          SafetyEngine.cpp
          DataflowEngine.cpp
          UnitPipeline.cpp
  PUBLIC_DEPS weavec::Core
  USES_LLVM)

weavec_link_llvm(weavecAnalysis Support)
weavec_link_clang(weavecAnalysis clangAnalysis clangAST clangBasic clangLex)
