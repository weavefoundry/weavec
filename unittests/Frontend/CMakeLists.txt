weavec_add_unittest(
  WeaveCFrontendTests
  SOURCES DiagnosticControlTest.cpp ProgramAnalysisTest.cpp SidecarTest.cpp
  DEPS weavec::Frontend)
