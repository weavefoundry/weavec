weavec_add_unittest(
  WeaveCAnalysisTests
  SOURCES AnnotationsTest.cpp FunctionAnalysisTest.cpp
  DEPS weavec::Analysis weavec::Frontend)
