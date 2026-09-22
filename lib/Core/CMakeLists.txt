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
          CheckPlan.cpp
          CheckedInteger.cpp
          Diagnostic.cpp
          FnSlots.cpp
          Integer.cpp
          Interface.cpp
          Ledger.cpp
          LibrarySpec.cpp
          Lifetime.cpp
          Moves.cpp
          Nullness.cpp
          Offset.cpp
          Ownership.cpp
          Place.cpp
          PointerKind.cpp
          Raw.cpp
          Relation.cpp
          Resource.cpp
          Scalar.cpp
          Scc.cpp
          SourceLocation.cpp
          Spatial.cpp
          Summary.cpp
          SummaryIO.cpp
          SummarySteps.cpp
          Traversal.cpp)

# RFC 0030 §8: the library table is embedded in the library as a byte array.
# Generated at configure time; CMAKE_CONFIGURE_DEPENDS re-runs the configure
# step (and file(CONFIGURE) rewrites the .inc only when its content changes)
# whenever LibrarySpec.txt is edited, so LibrarySpec.cpp rebuilds.
set(_weavec_library_spec "${CMAKE_CURRENT_SOURCE_DIR}/LibrarySpec.txt")
set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS "${_weavec_library_spec}")
file(READ "${_weavec_library_spec}" _weavec_library_spec_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "'\\\\x\\1',"
                     _weavec_library_spec_bytes "${_weavec_library_spec_hex}")
file(
  CONFIGURE
  OUTPUT
  "${CMAKE_CURRENT_BINARY_DIR}/LibrarySpecText.inc"
  CONTENT
  "// Generated from lib/Core/LibrarySpec.txt by lib/Core/CMakeLists.txt.
// NOLINTNEXTLINE(*-avoid-c-arrays)
static constexpr char LibrarySpecTextData[] = {@_weavec_library_spec_bytes@ '\\0'};
static constexpr std::size_t LibrarySpecTextSize =
    sizeof(LibrarySpecTextData) - 1;
"
  @ONLY)
target_include_directories(weavecCore PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
