weavec_add_unittest(
  WeaveCCoreTests
  SOURCES DiagnosticTest.cpp
          IntegerTest.cpp
          LibrarySpecTest.cpp
          # RFC 0030.
          FnSlotsTest.cpp
          # RFC 0031.
          EffectsTest.cpp
          HeapTest.cpp
          LedgerTest.cpp
          PointerKindTest.cpp
  DEPS weavec::Core)
