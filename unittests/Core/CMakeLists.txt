weavec_add_unittest(
  WeaveCCoreTests
  SOURCES DiagnosticTest.cpp
          IntegerTest.cpp
          LibrarySpecTest.cpp
          OwnershipTest.cpp
          # RFC 0030.
          CheckPlanTest.cpp
          FnSlotsTest.cpp
          # RFC 0031.
          EffectsIOTest.cpp
          HeapTest.cpp
          LedgerTest.cpp
          PointerKindTest.cpp
  DEPS weavec::Core)
