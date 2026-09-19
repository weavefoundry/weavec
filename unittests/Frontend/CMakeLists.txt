weavec_add_unittest(
  WeaveCFrontendTests
  SOURCES DiagnosticControlTest.cpp ProgramAnalysisTest.cpp RetainedUnitTest.cpp
          SidecarTest.cpp
  DEPS weavec::Frontend)
