weavec_add_unittest(
  WeaveCAnalysisTests
  SOURCES AllocatorsTest.cpp
          AnnotationsTest.cpp
          DataflowTest.cpp
          FunctionAnalysisTest.cpp
          HeapStateTest.cpp
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
  DEPS weavec::Analysis weavec::Frontend)
