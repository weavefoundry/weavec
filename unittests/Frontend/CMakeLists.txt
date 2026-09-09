weavec_add_unittest(
  WeaveCFrontendTests
  SOURCES AnalysisCacheTest.cpp CheckedReportTest.cpp DiagnosticControlTest.cpp
          ProgramAnalysisTest.cpp SidecarTest.cpp
  DEPS weavec::Frontend)
