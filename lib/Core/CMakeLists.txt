# weavec::Core -- the frontend-independent ownership/borrow/lifetime model.
#
# This library MUST NOT depend on LLVM or Clang. Keeping it standard-library
# only is what allows the model to be reused outside the Clang integration.
weavec_add_library(
  Core
  SOURCES AliasRelation.cpp
          AnalysisState.cpp
          Array.cpp
          Borrow.cpp
          CallContext.cpp
          CallTargets.cpp
          Diagnostic.cpp
          Integer.cpp
          Interface.cpp
          CheckedInteger.cpp
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
          Traversal.cpp
          Summary.cpp
          SummarySteps.cpp
          SummaryIO.cpp
          # RFC 0030: the ledger, pointer kinds, check plans and slots.
          CheckPlan.cpp
          FnSlots.cpp
          Ledger.cpp
          PointerKind.cpp)
