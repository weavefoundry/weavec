weavec_add_unittest(
  WeaveCFrontendTests
  SOURCES CheckEmitterTest.cpp
          DeferredCodeGenConsumerTest.cpp
          DiagnosticControlTest.cpp
          LedgerOutputTest.cpp
          LedgerWriterTest.cpp
          LinkStepTest.cpp
          PreludeTest.cpp
          ProgramAnalysisTest.cpp
          RecordFactsTest.cpp
          RecordPayloadTest.cpp
          RetainedUnitTest.cpp
          UnitRecordTest.cpp
  DEPS weavec::Frontend)

# RFC 0030, section 10.5: the LLVM-upgrade checklist test reads the
# ASTConsumer and SemaConsumer headers WeaveC is built against.
target_compile_definitions(
  WeaveCFrontendTests
  PRIVATE "WEAVEC_CLANG_INCLUDE_DIRS=\"${CLANG_INCLUDE_DIRS}\"")
