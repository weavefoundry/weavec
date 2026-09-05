# weavec::Core -- the frontend-independent ownership/borrow/lifetime model.
#
# This library MUST NOT depend on LLVM or Clang. Keeping it standard-library
# only is what allows the model to be reused outside the Clang integration.
weavec_add_library(
  Core
  SOURCES AliasRelation.cpp
          AnalysisState.cpp
          Borrow.cpp
          Diagnostic.cpp
          Lifetime.cpp
          Moves.cpp
          Nullness.cpp
          Offset.cpp
          Ownership.cpp
          Place.cpp
          Raw.cpp
          Relation.cpp
          Resource.cpp
          Scalar.cpp
          Scc.cpp
          SourceLocation.cpp
          Spatial.cpp
          Summary.cpp
          SummaryIO.cpp)
