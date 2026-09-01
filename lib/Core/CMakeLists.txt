# weavec::Core -- the frontend-independent ownership/borrow/lifetime model.
#
# This library MUST NOT depend on LLVM or Clang. Keeping it standard-library
# only is what allows the model to be reused outside the Clang integration.
weavec_add_library(
  Core
  SOURCES Borrow.cpp
          Diagnostic.cpp
          Lifetime.cpp
          Moves.cpp
          Ownership.cpp
          Place.cpp
          SourceLocation.cpp)
