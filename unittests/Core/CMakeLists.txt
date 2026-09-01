weavec_add_unittest(
  WeaveCCoreTests
  SOURCES BorrowTest.cpp DiagnosticTest.cpp LifetimeTest.cpp MovesTest.cpp
          OwnershipTest.cpp PlaceTest.cpp
  DEPS weavec::Core)
