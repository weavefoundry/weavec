weavec_add_unittest(
  WeaveCCoreTests
  SOURCES SafetyTest.cpp
          CallContextTest.cpp
          CallTargetsTest.cpp
          AliasRelationTest.cpp
          AnalysisStateTest.cpp
          ArrayTest.cpp
          BorrowTest.cpp
          DiagnosticTest.cpp
          HeapSummaryTest.cpp
          IntegerTest.cpp
          IntegerExpressionTest.cpp
          LifetimeTest.cpp
          MovesTest.cpp
          NullnessTest.cpp
          NumericContractsTest.cpp
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
