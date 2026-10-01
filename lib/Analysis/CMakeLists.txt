# weavec::Analysis -- bridges Clang's AST to the core ownership model.
weavec_add_library(
  Analysis
  SOURCES Annotations.cpp
          BypassedDeclarations.cpp
          ClangLocation.cpp
          ProgramDatabase.cpp
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
          # RFC 0031.
          EngineAnnotations.cpp
          EngineCalls.cpp
          EngineContexts.cpp
          EngineDecide.cpp
          EngineExpr.cpp
          EngineInvariants.cpp
          EngineKinds.cpp
          EngineLibrary.cpp
          EngineLifetimes.cpp
          EngineRun.cpp
          EngineStrings.cpp
          EngineSummary.cpp
          EngineUnit.cpp
          UnitPipeline.cpp
  PUBLIC_DEPS weavec::Core
  USES_LLVM)

weavec_link_llvm(weavecAnalysis Support)
weavec_link_clang(weavecAnalysis clangAnalysis clangAST clangBasic clangLex)
