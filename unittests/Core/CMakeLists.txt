weavec_add_unittest(
  WeaveCCoreTests
  SOURCES CallTargetsTest.cpp
          AliasRelationTest.cpp
          AnalysisStateTest.cpp
          BorrowTest.cpp
          DiagnosticTest.cpp
          HeapSummaryTest.cpp
          LifetimeTest.cpp
          MovesTest.cpp
          NullnessTest.cpp
          OffsetTest.cpp
          OwnershipTest.cpp
          PlaceTest.cpp
          RawTest.cpp
          RelationTest.cpp
          ResourceTest.cpp
          ScalarTest.cpp
          SpatialTest.cpp
          SummaryIOTest.cpp
          SummaryTest.cpp
  DEPS weavec::Core)
