weavec_add_unittest(
  WeaveCFrontendTests
  SOURCES CheckedReportTest.cpp DiagnosticControlTest.cpp
          ProgramAnalysisTest.cpp SidecarTest.cpp
  DEPS weavec::Frontend)
