# weavec::Core -- the C library table the guards read (RFC 0036 §6).
#
# This library MUST NOT depend on LLVM or Clang.
weavec_add_library(Core SOURCES LibrarySpec.cpp)

# RFC 0036 §6: the library table is embedded in the library as a byte array.
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
