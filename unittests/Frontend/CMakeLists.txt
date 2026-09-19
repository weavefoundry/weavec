weavec_add_unittest(
  WeaveCFrontendTests
  SOURCES DiagnosticControlTest.cpp
          LedgerWriterTest.cpp
          ProgramAnalysisTest.cpp
          RetainedUnitTest.cpp
          SidecarTest.cpp
          UnitRecordTest.cpp
  DEPS weavec::Frontend)
