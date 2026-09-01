weavec_add_unittest(
  WeaveCCoreTests
  SOURCES AliasRelationTest.cpp
          AnalysisStateTest.cpp
          BorrowTest.cpp
          DiagnosticTest.cpp
          LifetimeTest.cpp
          MovesTest.cpp
          OwnershipTest.cpp
          PlaceTest.cpp
  DEPS weavec::Core)
