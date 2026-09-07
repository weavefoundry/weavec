weavec_add_unittest(
  WeaveCAnalysisTests
  SOURCES AllocatorsTest.cpp
          AnnotationsTest.cpp
          ArrayOwnershipTest.cpp
          CallContextTest.cpp
          DataflowTest.cpp
          DynamicExtentsTest.cpp
          FunctionAnalysisTest.cpp
          HeapStateTest.cpp
          IntegerSemanticsTest.cpp
          NumericInputsTest.cpp
          PointerValidityTest.cpp
          PointerIdentityTest.cpp
          ProgramDatabaseTest.cpp
          ResourceLifecycleTest.cpp
          SharedOwnershipTest.cpp
          SignatureInferenceTest.cpp
          SpatialSafetyTest.cpp
          StringsAndFieldsTest.cpp
          SummariesTest.cpp
          ValueConditionalTest.cpp
          LoopRequirementsTest.cpp
          GuardCompletenessTest.cpp
  DEPS weavec::Analysis weavec::Frontend)
