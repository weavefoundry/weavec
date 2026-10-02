weavec_add_unittest(
  WeaveCAnalysisTests
  SOURCES AnnotationsTest.cpp
          ArrayOwnershipTest.cpp
          DynamicExtentsTest.cpp
          HeapStateTest.cpp
          IntegerSemanticsTest.cpp
          NumericInputsTest.cpp
          PointerValidityTest.cpp
          PointerIdentityTest.cpp
          ProgramDatabaseTest.cpp
          ResourceLifecycleTest.cpp
          SharedOwnershipTest.cpp
          SpatialSafetyTest.cpp
          StringsAndFieldsTest.cpp
          ValueConditionalTest.cpp
          LoopRequirementsTest.cpp
          GuardCompletenessTest.cpp
          AttributeReaderTest.cpp
          KindInferenceTest.cpp
          SlotCollectorTest.cpp
          SiteCollectorTest.cpp
          CheckPlannerTest.cpp
          LedgerAdapterTest.cpp
          EngineDecisionsTest.cpp
          SoundDefaultsTest.cpp
  DEPS weavec::Analysis weavec::Frontend)
