weavec_add_unittest(
  WeaveCAnalysisTests
  SOURCES AllocatorsTest.cpp AnnotationsTest.cpp DataflowTest.cpp
          FunctionAnalysisTest.cpp
  DEPS weavec::Analysis weavec::Frontend)
