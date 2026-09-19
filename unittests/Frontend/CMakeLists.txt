weavec_add_unittest(
  WeaveCFrontendTests
  SOURCES DeferredCodeGenConsumerTest.cpp
          DiagnosticControlTest.cpp
          LedgerOutputTest.cpp
          LedgerWriterTest.cpp
          PreludeTest.cpp
          ProgramAnalysisTest.cpp
          RetainedUnitTest.cpp
          SidecarTest.cpp
          UnitRecordTest.cpp
  DEPS weavec::Frontend)

# RFC 0030, section 10.5: the LLVM-upgrade checklist test reads the
# ASTConsumer and SemaConsumer headers WeaveC is built against.
target_compile_definitions(
  WeaveCFrontendTests
  PRIVATE "WEAVEC_CLANG_INCLUDE_DIRS=\"${CLANG_INCLUDE_DIRS}\"")
