weavec_add_unittest(
  WeaveCAnalysisTests
  SOURCES AllocatorsTest.cpp
          AnnotationsTest.cpp
          DataflowTest.cpp
          FunctionAnalysisTest.cpp
          PointerValidityTest.cpp
          ProgramDatabaseTest.cpp
          ResourceLifecycleTest.cpp
          SharedOwnershipTest.cpp
          SignatureInferenceTest.cpp
          SpatialSafetyTest.cpp
          SummariesTest.cpp
          ValueConditionalTest.cpp
  DEPS weavec::Analysis weavec::Frontend)
