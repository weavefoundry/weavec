weavec_add_unittest(
  WeaveCFrontendTests
  SOURCES DiagnosticControlTest.cpp
          GuardPassTest.cpp
          AnalysisSummaryTest.cpp
          InterfaceFactsTest.cpp
          ProgramAnalysisTest.cpp
          ProgramChecksTest.cpp
          RetainedUnitTest.cpp
  DEPS weavec::Frontend)

# RFC 0035: the guard pass tests parse IR and run the pass on it.
weavec_link_llvm(WeaveCFrontendTests AsmParser Passes)
