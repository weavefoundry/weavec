//===- LibrarySpecTest.cpp - The C library table of the guards ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0036 §6: every row of LibrarySpec.txt is checked against an
// independent expectation table, written from the C standard, POSIX and the
// platform manuals in a notation of its own (`render` below), so that a
// wrong row fails. The other tests cover terms, parser errors, names,
// overloads and the fortified forms with their object-size argument.
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/LibrarySpec.h"

#include <gtest/gtest.h>

#include <array>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::core {

//===----------------------------------------------------------------------===//
// The expectation notation
//===----------------------------------------------------------------------===//

static std::string renderParam(const LibraryParam &param) {
  using Access = LibraryParam::Access;
  std::string text;
  switch (param.type) {
  case LibraryParam::Type::Int:
    return "int";
  case LibraryParam::Type::Other:
    return "other";
  case LibraryParam::Type::Function:
    return "fn";
  case LibraryParam::Type::Pointer:
    break;
  }
  switch (param.access) {
  case Access::None:
    text = "none";
    break;
  case Access::Read:
    text = "R";
    break;
  case Access::Write:
    text = "W";
    break;
  case Access::ReadWrite:
    text = "RW";
    break;
  }
  if (param.string)
    text += " str";
  if (param.bytes)
    text += " bytes=" + param.bytes->str();
  if (param.count)
    text += " count=" + param.count->str();
  if (param.null == LibraryParam::Null::Allowed)
    text += " nullable";
  if (param.null == LibraryParam::Null::AllowedIfZero)
    text += " nullable-if=" + (param.zeroTerm ? param.zeroTerm->str() : "?");
  if (param.effect == LibraryParam::Effect::Release)
    text += " releases";
  if (param.effect == LibraryParam::Effect::Realloc)
    text += " reallocs";
  return text;
}

static std::string renderParams(const LibraryEntry &entry) {
  std::string text;
  for (const LibraryParam &param : entry.params)
    text += (text.empty() ? "" : ", ") + renderParam(param);
  if (entry.variadic)
    text += text.empty() ? "..." : ", ...";
  return text;
}

static std::string renderResult(const LibraryEntry &entry) {
  switch (entry.result) {
  case LibraryEntry::Result::Void:
    return "void";
  case LibraryEntry::Result::Int:
    return entry.value ? "int value=" + entry.value->str() : "int";
  case LibraryEntry::Result::Pointer:
    return "ptr";
  }
  return "?";
}

static std::string renderClauses(const LibraryEntry &entry) {
  std::vector<std::string> clauses;
  clauses.reserve(entry.disjoint.size());
  for (const LibDisjoint &d : entry.disjoint)
    clauses.push_back("disjoint=" + std::to_string(d.first) + "/" +
                      std::to_string(d.second) + "/" + d.length.str());
  if (entry.wide)
    clauses.emplace_back("wide");
  if (entry.format)
    clauses.push_back(std::string(entry.format->kind == LibFormat::Kind::Printf
                                      ? "printf="
                                      : "scanf=") +
                      std::to_string(entry.format->format) + "/" +
                      std::to_string(entry.format->first));
  if (entry.wrapper)
    clauses.push_back("wrapper=" + entry.wrapper->name);
  for (const LibraryChk &alias : entry.chk) {
    std::string text = "chk=" + alias.name + ":";
    for (std::size_t i = 0; i < alias.argumentOf.size(); ++i) {
      if (i != 0)
        text += '/';
      if (std::cmp_equal(i, alias.sizeArgument))
        text += "size";
      else if (alias.argumentOf[i] < 0)
        text += '_';
      else
        text += std::to_string(alias.argumentOf[i]);
    }
    clauses.push_back(text);
  }
  std::string text;
  for (const std::string &clause : clauses)
    text += (text.empty() ? "" : " ") + clause;
  return text;
}

namespace {
/// One row of the expectation table.
struct Expectation {
  /// The function; `name#2` for its second overload in file order.
  std::string_view name;
  std::string_view params;
  std::string_view result;
  std::string_view clauses;
};
} // namespace

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

static std::string parseError(std::string_view text) {
  std::string error;
  const auto spec = LibrarySpec::parse(text, error);
  EXPECT_FALSE(spec.has_value()) << text;
  return error;
}

static LibrarySpec parseOk(std::string_view text) {
  std::string error;
  auto spec = LibrarySpec::parse(text, error);
  EXPECT_TRUE(spec.has_value()) << error;
  return spec ? std::move(*spec) : LibrarySpec{};
}

//===----------------------------------------------------------------------===//
// The shipped table and the syntax
//===----------------------------------------------------------------------===//

TEST(LibrarySpecTest, ShippedTableParses) {
  std::string error;
  EXPECT_TRUE(LibrarySpec::parse(LibrarySpec::shippedText(), error)) << error;
  EXPECT_TRUE(LibrarySpec::shippedText().starts_with("# LibrarySpec.txt"));
  EXPECT_GE(LibrarySpec::shipped().entries().size(), 600U);
}

TEST(LibrarySpecTest, ContinuationsAndComments) {
  const LibrarySpec spec = parseOk("# a comment\n"
                                   "f (r:str, \\\n"
                                   "   int) -> int;   # trailing comment\n"
                                   "\n"
                                   "g (w:bytes(a1), int) -> void;\n");
  ASSERT_EQ(spec.entries().size(), 2U);
  EXPECT_EQ(spec.entries()[0].name, "f");
  EXPECT_EQ(spec.entries()[0].params.size(), 2U);
  EXPECT_EQ(spec.entries()[0].line, 2U);
  EXPECT_EQ(spec.entries()[1].line, 5U);
}

TEST(LibrarySpecTest, TermsParsePrintAndEvaluate) {
  const auto roundTrip = [](std::string_view text) {
    std::string error;
    const auto term = LibTerm::parse(text, error);
    EXPECT_TRUE(term.has_value()) << text << ": " << error;
    return term ? term->str() : error;
  };
  EXPECT_EQ(roundTrip("a0*a1+1"), "a0*a1+1");
  EXPECT_EQ(roundTrip("a0 + a1 * a2"), "a0+a1*a2");
  EXPECT_EQ(roundTrip("(a0+a1)*a2"), "(a0+a1)*a2");
  EXPECT_EQ(roundTrip("min(a2, strlen(a1)+1)"), "min(a2,strlen(a1)+1)");
  EXPECT_EQ(roundTrip("a3-1"), "a3-1");
  EXPECT_EQ(roundTrip("(a0+7)/8"), "(a0+7)/8");
  EXPECT_EQ(roundTrip("a0*4/3"), "a0*4/3");
  std::string error;
  const auto sum = LibTerm::parse("a0+a1*a2", error);
  ASSERT_TRUE(sum.has_value());
  EXPECT_EQ(sum->kind, LibTerm::Kind::Sum);
  EXPECT_EQ(sum->operands[1].kind, LibTerm::Kind::Product);
  for (const char *bad :
       {"", "a", "a0+", "min(a0)", "a0 a1", "a256", "strlen(x)", "fmtlen(a1)",
        "BUFSIZ", "a1-b", "x0", "a0/0", "a0/a1", "a0/-2"})
    EXPECT_FALSE(LibTerm::parse(bad, error).has_value()) << bad;

  const LibTerm::Values values{
      .argument = [](unsigned i) -> std::optional<std::int64_t> {
        return i < 3 ? std::optional<std::int64_t>(10 * (i + 1)) : std::nullopt;
      },
      .stringLength = [](unsigned) -> std::optional<std::int64_t> {
        return 5;
      }};
  const auto eval = [&](std::string_view text) {
    std::string why;
    return LibTerm::parse(text, why)->evaluate(values);
  };
  EXPECT_EQ(eval("a0*a1+1"), 201);
  EXPECT_EQ(eval("min(a2,strlen(a1)+1)"), 6);
  EXPECT_EQ(eval("a2-31"), -1);
  EXPECT_EQ(eval("(a0+7)/8"), 2);
  EXPECT_EQ(eval("(a0-17)/8"), -1);
  EXPECT_EQ(eval("a3"), std::nullopt);
  EXPECT_EQ(eval("9223372036854775807+a0"), std::nullopt);
  EXPECT_EQ(eval("4611686018427387904*a1"), std::nullopt);
}

TEST(LibrarySpecTest, ParserErrorsNameTheLineAndTheProblem) {
  struct Case {
    const char *text;
    const char *error;
  };
  const std::vector<Case> cases = {
      {"f (int -> int;", "LibrarySpec.txt:1: 'f': expected ')' after the "
                         "parameters"},
      {"f (int, \\\n   bogus) -> int;",
       "LibrarySpec.txt:2: 'f': expected a parameter, found 'bogus'"},
      {"f (r:bytez(a0)) -> int;", "unknown parameter flag 'bytez'"},
      {"f (r:null-ok:null-if-zero(a1), int) -> int;",
       "'null-ok' and 'null-if-zero' exclude each other"},
      {"f (w:bytes(a3), int) -> int;",
       "argument a3 of parameter 0 is past the row's parameters"},
      {"f (w:bytes(a1):count(a1), int) -> int wide;",
       "'bytes' and 'count' exclude each other"},
      {"f (w:count(a1), int) -> int;", "'count' needs a wide row"},
      {"f (w:str) -> int;", "'str' needs 'r' or 'rw'"},
      {"f (r:str:str) -> int;", "duplicate flag 'str'"},
      {"f (int) -> fresh(free);", "expected 'void', 'int' or 'ptr'"},
      {"f (r) -> int:extent(1);", "expected 'value(…)' after 'int:'"},
      {"f (r) -> int:value(a3);",
       "argument a3 of the result is past the row's parameters"},
      {"f (w:bytes(a1), int) -> void wrapper(f,room);",
       "after the wrapper name"},
      {"f (r:str) -> int wrapper(g);", "'wrapper(g)' must name the row"},
      {"f (w:bytes(a1), int) -> void wrapper(f) wrapper(f);",
       "duplicate clause 'wrapper'"},
      {"f () -> int", "expected ';' at the end of the entry"},
      {"f () -> int; g () -> int;", "unexpected text after ';'"},
      {"f (r, int) -> int chk(__f_chk:0,5);",
       "alias '__f_chk' names an argument past the row's parameters"},
      {"f (r, int) -> int chk(__f_chk:0,1,size,size);",
       "an alias has at most one 'size' argument"},
      {"f (int) -> int chk(g:0);\ng (int) -> int;",
       "alias 'g' is already a row or an alias"},
      {"f (r:str, ...) -> int printf(0,1) scanf(0,1);",
       "a row has at most one format clause"},
      {"f (w, r) -> int disjoint(0,5,1);",
       "'disjoint' names an argument past the row's parameters"},
      {"f () -> int frobnicate;", "unknown clause 'frobnicate'"},
  };
  for (const Case &c : cases) {
    const std::string error = parseError(c.text);
    EXPECT_NE(error.find(c.error), std::string::npos)
        << c.text << "\n  gave: " << error;
  }
}

//===----------------------------------------------------------------------===//
// Names, aliases and fortified forms
//===----------------------------------------------------------------------===//

/// The row arguments that call arguments 0..count-1 become ('size' -2).
static std::vector<int> remap(std::string_view callee, unsigned count) {
  const auto match = LibrarySpec::shipped().lookup(callee);
  EXPECT_TRUE(match.has_value()) << callee;
  std::vector<int> rows;
  for (unsigned i = 0; match && i < count; ++i)
    rows.push_back(
        std::cmp_equal(i, match->sizeArgument()) ? -2 : match->rowArgument(i));
  return rows;
}

static std::string resolvedName(std::string_view callee) {
  const auto match = LibrarySpec::shipped().lookup(callee);
  return match ? match->entry->name : "<none>";
}

TEST(LibrarySpecTest, NamesResolveDirectlyAndBehindBuiltinPrefix) {
  const LibrarySpec &spec = LibrarySpec::shipped();
  EXPECT_EQ(resolvedName("memcpy"), "memcpy");
  EXPECT_EQ(resolvedName("__builtin_memcpy"), "memcpy");
  EXPECT_EQ(resolvedName("__builtin_strlen"), "strlen");
  EXPECT_EQ(resolvedName("json_decref"), "<none>");
  EXPECT_EQ(resolvedName("__builtin_json_decref"), "<none>");
  EXPECT_EQ(spec.find("__builtin_memcpy"), nullptr);
  const auto plain = spec.lookup("__builtin_memcpy");
  ASSERT_TRUE(plain.has_value());
  EXPECT_EQ(plain->alias, nullptr);
  EXPECT_EQ(plain->sizeArgument(), -1);
  EXPECT_EQ(plain->rowArgument(2), 2);
  EXPECT_EQ(plain->callArgument(1), 1);
}

TEST(LibrarySpecTest, MacOSFortifiedSpellingsCarryTheObjectSize) {
  // secure/_string.h: __builtin___X_chk(dst, ..., __darwin_obsz(dst)).
  EXPECT_EQ(remap("__builtin___memcpy_chk", 4),
            (std::vector<int>{0, 1, 2, -2}));
  EXPECT_EQ(remap("__builtin___memset_chk", 4),
            (std::vector<int>{0, 1, 2, -2}));
  EXPECT_EQ(remap("__builtin___strcpy_chk", 3), (std::vector<int>{0, 1, -2}));
  EXPECT_EQ(remap("__builtin___stpcpy_chk", 3), (std::vector<int>{0, 1, -2}));
  EXPECT_EQ(remap("__builtin___strcat_chk", 3), (std::vector<int>{0, 1, -2}));
  EXPECT_EQ(remap("__builtin___strncpy_chk", 4),
            (std::vector<int>{0, 1, 2, -2}));
  EXPECT_EQ(remap("__builtin___strlcpy_chk", 4),
            (std::vector<int>{0, 1, 2, -2}));
  EXPECT_EQ(remap("__builtin___memccpy_chk", 5),
            (std::vector<int>{0, 1, 2, 3, -2}));
  // secure/_stdio.h: the flag and the object size precede the format.
  EXPECT_EQ(remap("__builtin___sprintf_chk", 6),
            (std::vector<int>{0, -1, -2, 1, 2, 3}));
  EXPECT_EQ(remap("__builtin___snprintf_chk", 7),
            (std::vector<int>{0, 1, -1, -2, 2, 3, 4}));
  EXPECT_EQ(remap("__builtin___vsprintf_chk", 5),
            (std::vector<int>{0, -1, -2, 1, 2}));
  EXPECT_EQ(remap("__builtin___vsnprintf_chk", 6),
            (std::vector<int>{0, 1, -1, -2, 2, 3}));
  EXPECT_EQ(resolvedName("__builtin___sprintf_chk"), "sprintf");
  EXPECT_EQ(remap("__snprintf_chk", 6), (std::vector<int>{0, 1, -1, -2, 2, 3}));
  const auto sprintfChk =
      LibrarySpec::shipped().lookup("__builtin___sprintf_chk");
  ASSERT_TRUE(sprintfChk.has_value());
  EXPECT_EQ(sprintfChk->callArgument(1), 3);
  EXPECT_EQ(sprintfChk->callArgument(2), 4);
  EXPECT_EQ(sprintfChk->sizeArgument(), 2);
}

TEST(LibrarySpecTest, GlibcFortifiedSpellingsCarryTheObjectSize) {
  EXPECT_EQ(remap("__memcpy_chk", 4), (std::vector<int>{0, 1, 2, -2}));
  EXPECT_EQ(remap("__strcpy_chk", 3), (std::vector<int>{0, 1, -2}));
  EXPECT_EQ(remap("__explicit_bzero_chk", 3), (std::vector<int>{0, 1, -2}));
  // bits/stdio2.h: a flag, not a size, precedes the format.
  EXPECT_EQ(remap("__printf_chk", 3), (std::vector<int>{-1, 0, 1}));
  EXPECT_EQ(remap("__fprintf_chk", 4), (std::vector<int>{0, -1, 1, 2}));
  EXPECT_EQ(remap("__asprintf_chk", 4), (std::vector<int>{0, -1, 1, 2}));
  EXPECT_EQ(remap("__syslog_chk", 4), (std::vector<int>{0, -1, 1, 2}));
  EXPECT_EQ(remap("__fgets_chk", 4), (std::vector<int>{0, -2, 1, 2}));
  EXPECT_EQ(remap("__fread_chk", 5), (std::vector<int>{0, -2, 1, 2, 3}));
  EXPECT_EQ(remap("__read_chk", 4), (std::vector<int>{0, 1, 2, -2}));
  EXPECT_EQ(remap("__pread64_chk", 5), (std::vector<int>{0, 1, 2, 3, -2}));
  EXPECT_EQ(remap("__getcwd_chk", 3), (std::vector<int>{0, 1, -2}));
  EXPECT_EQ(remap("__recv_chk", 5), (std::vector<int>{0, 1, 2, -2, 3}));
  EXPECT_EQ(remap("__wmemcpy_chk", 4), (std::vector<int>{0, 1, 2, -2}));
  // __mbstowcs_chk's size counts wide characters the row's bytes do not.
  EXPECT_EQ(remap("__mbstowcs_chk", 4), (std::vector<int>{0, 1, 2, -1}));
  EXPECT_EQ(remap("__open_2", 2), (std::vector<int>{0, 1}));
  // Renames rather than fortified forms.
  EXPECT_EQ(resolvedName("__isoc99_sscanf"), "sscanf");
  EXPECT_EQ(resolvedName("__isoc23_strtol"), "strtol");
  EXPECT_EQ(resolvedName("__xpg_basename"), "basename");
  EXPECT_EQ(remap("__isoc99_fscanf", 4), (std::vector<int>{0, 1, 2, 3}));
}

TEST(LibrarySpecTest, OverloadsFollowTheArity) {
  const LibrarySpec &spec = LibrarySpec::shipped();
  // glibc's pthread_setname_np(thread, name); macOS's pthread_setname_np(name).
  const auto glibc = spec.lookup("pthread_setname_np", 2);
  ASSERT_TRUE(glibc.has_value());
  EXPECT_EQ(glibc->entry->params.size(), 2U);
  EXPECT_TRUE(glibc->entry->params[1].string);
  const auto darwin = spec.lookup("pthread_setname_np", 1);
  ASSERT_TRUE(darwin.has_value());
  EXPECT_EQ(darwin->entry->params.size(), 1U);
  EXPECT_TRUE(darwin->entry->params[0].string);
  // An arity no row has takes the first; a variadic row takes more.
  EXPECT_EQ(spec.lookup("pthread_setname_np", 3)->entry, glibc->entry);
  EXPECT_EQ(spec.lookup("printf", 5)->entry->name, "printf");
}

TEST(LibrarySpecTest, NumberParsersHaveWrappers) {
  for (const char *name :
       {"atoi", "atol", "atoll", "atof", "strtol", "strtoll", "strtoul",
        "strtoull", "strtod", "strtof", "strtold", "strtoimax", "strtoumax"}) {
    const LibraryEntry *entry = LibrarySpec::shipped().find(name);
    ASSERT_NE(entry, nullptr) << name;
    ASSERT_TRUE(entry->wrapper.has_value()) << name;
    EXPECT_EQ(entry->wrapper->name, name);
  }
}

//===----------------------------------------------------------------------===//
// Every row, independently
//===----------------------------------------------------------------------===//

// Heap allocation: every heap result may fail, is released by free, and
// carries its §11 zero-initialisation decision.
static constexpr auto HeapExpectations = std::to_array<Expectation>({
    {"posix_memalign", "W bytes=8, int, int", "int", ""},
    {"canonicalize_file_name", "R str nullable", "ptr", ""},
    {"realpath", "R str nullable, W nullable", "ptr",
     "chk=__realpath_chk:0/1/size"},
    {"strdup", "R str", "ptr", ""},
    {"strndup", "R bytes=min(a1,strlen(a0)+1) nullable-if=a1, int", "ptr", ""},
    {"wcsdup", "R str", "ptr", "wide"},
    {"asprintf", "W bytes=8, R str, ...", "int",
     "printf=1/2 chk=__asprintf_chk:0/_/1/2"},
    {"vasprintf", "W bytes=8, R str, other", "int",
     "printf=1/2 chk=__vasprintf_chk:0/_/1/2"},
    {"getline", "RW bytes=8 nullable, RW nullable, RW", "int", ""},
    {"getdelim", "RW bytes=8 nullable, RW nullable, int, RW", "int", ""},
    {"tempnam", "R str nullable, R str nullable", "ptr", ""},
    {"getcwd", "W bytes=a1 nullable, int", "ptr", "chk=__getcwd_chk:0/1/size"},
    {"backtrace", "W bytes=a1*8, int", "int", ""},
    {"backtrace_symbols", "R bytes=a1*8, int", "ptr", ""},
    {"backtrace_symbols_fd", "R bytes=a1*8, int, int", "void", ""},
});

// <string.h>, <strings.h>: C17 7.24, POSIX, glibc and BSD extensions.
static constexpr auto StringExpectations = std::to_array<Expectation>({
    {"memcpy", "W bytes=a2 nullable-if=a2, R bytes=a2 nullable-if=a2, int",
     "ptr",
     "disjoint=0/1/a2 chk=__builtin___memcpy_chk:0/1/2/size "
     "chk=__memcpy_chk:0/1/2/size"},
    {"memmove", "W bytes=a2 nullable-if=a2, R bytes=a2 nullable-if=a2, int",
     "ptr",
     "chk=__builtin___memmove_chk:0/1/2/size chk=__memmove_chk:0/1/2/size"},
    {"memset", "W bytes=a2 nullable-if=a2, int, int", "ptr",
     "chk=__builtin___memset_chk:0/1/2/size chk=__memset_chk:0/1/2/size"},
    {"memset_explicit", "W bytes=a2 nullable-if=a2, int, int", "ptr", ""},
    {"memcmp", "R bytes=a2 nullable-if=a2, R bytes=a2 nullable-if=a2, int",
     "int", ""},
    {"memchr", "R bytes=min(a2,strlen(a0)+1) nullable-if=a2, int, int", "ptr",
     "wrapper=memchr"},
    {"memrchr", "R bytes=a2 nullable-if=a2, int, int", "ptr", ""},
    {"rawmemchr", "R, int", "ptr", ""},
    {"memmem", "R bytes=a1 nullable-if=a1, int, R bytes=a3 nullable-if=a3, int",
     "ptr", ""},
    {"mempcpy", "W bytes=a2 nullable-if=a2, R bytes=a2 nullable-if=a2, int",
     "ptr",
     "disjoint=0/1/a2 chk=__builtin___mempcpy_chk:0/1/2/size "
     "chk=__mempcpy_chk:0/1/2/size"},
    {"memccpy",
     "W bytes=min(a3,strlen(a1)+1) nullable-if=a3, R "
     "bytes=min(a3,strlen(a1)+1) nullable-if=a3, int, int",
     "ptr",
     "disjoint=0/1/min(a3,strlen(a1)+1) "
     "chk=__builtin___memccpy_chk:0/1/2/3/size"},
    {"strcpy", "W bytes=strlen(a1)+1, R str", "ptr",
     "disjoint=0/1/strlen(a1)+1 wrapper=strcpy "
     "chk=__builtin___strcpy_chk:0/1/size chk=__strcpy_chk:0/1/size"},
    {"strncpy",
     "W bytes=a2 nullable-if=a2, R bytes=min(a2,strlen(a1)+1) nullable-if=a2, "
     "int",
     "ptr",
     "disjoint=0/1/min(a2,strlen(a1)+1) chk=__builtin___strncpy_chk:0/1/2/size "
     "chk=__strncpy_chk:0/1/2/size"},
    {"stpcpy", "W bytes=strlen(a1)+1, R str", "ptr",
     "disjoint=0/1/strlen(a1)+1 wrapper=stpcpy "
     "chk=__builtin___stpcpy_chk:0/1/size chk=__stpcpy_chk:0/1/size"},
    {"stpncpy",
     "W bytes=a2 nullable-if=a2, R bytes=min(a2,strlen(a1)+1) nullable-if=a2, "
     "int",
     "ptr",
     "disjoint=0/1/min(a2,strlen(a1)+1) chk=__builtin___stpncpy_chk:0/1/2/size "
     "chk=__stpncpy_chk:0/1/2/size"},
    {"strcat", "RW str bytes=strlen(a0)+strlen(a1)+1, R str", "ptr",
     "disjoint=0/1/strlen(a0)+strlen(a1)+1 wrapper=strcat "
     "chk=__builtin___strcat_chk:0/1/size chk=__strcat_chk:0/1/size"},
    {"strncat",
     "RW str bytes=strlen(a0)+min(a2,strlen(a1))+1, R "
     "bytes=min(a2,strlen(a1)+1) nullable-if=a2, int",
     "ptr",
     "disjoint=0/1/strlen(a0)+min(a2,strlen(a1))+1 "
     "chk=__builtin___strncat_chk:0/1/2/size chk=__strncat_chk:0/1/2/size"},
    {"strlcpy", "W bytes=a2 nullable-if=a2, R str, int", "int value=strlen(a1)",
     "disjoint=0/1/min(a2,strlen(a1)+1) chk=__builtin___strlcpy_chk:0/1/2/size "
     "chk=__strlcpy_chk:0/1/2/size"},
    {"strlcat", "RW bytes=a2 nullable-if=a2, R str, int", "int",
     "chk=__builtin___strlcat_chk:0/1/2/size chk=__strlcat_chk:0/1/2/size"},
    {"strlen", "R str", "int value=strlen(a0)", ""},
    {"strnlen", "R bytes=min(a1,strlen(a0)+1) nullable-if=a1, int",
     "int value=min(a1,strlen(a0))", ""},
    {"strcmp", "R str, R str", "int", "wrapper=strcmp"},
    {"strncmp",
     "R bytes=min(a2,strlen(a0)+1) nullable-if=a2, R "
     "bytes=min(a2,strlen(a1)+1) nullable-if=a2, int",
     "int", "wrapper=strncmp"},
    {"strcoll", "R str, R str", "int", ""},
    {"strcoll_l", "R str, R str, none nullable", "int", ""},
    {"strxfrm", "W bytes=a2 nullable-if=a2, R str, int", "int", ""},
    {"strxfrm_l", "W bytes=a2 nullable-if=a2, R str, int, none nullable", "int",
     ""},
    {"strverscmp", "R str, R str", "int", ""},
    {"strchr", "R str, int", "ptr", "wrapper=strchr"},
    {"strrchr", "R str, int", "ptr", ""},
    {"strchrnul", "R str, int", "ptr", ""},
    {"strstr", "R str, R str", "ptr", ""},
    {"strcasestr", "R str, R str", "ptr", ""},
    {"strpbrk", "R str, R str", "ptr", ""},
    {"strspn", "R str, R str", "int", ""},
    {"strcspn", "R str, R str", "int", ""},
    {"strtok", "RW str nullable, R str", "ptr", ""},
    {"strtok_r", "RW str nullable, R str, RW bytes=8", "ptr", ""},
    {"strsep", "RW, R str", "ptr", ""},
    {"strerror_r", "int, W bytes=a2 nullable-if=a2, int", "ptr", ""},
    {"strerror_r#2", "int, W bytes=a2 nullable-if=a2, int", "int", ""},
    {"explicit_bzero", "W bytes=a1 nullable-if=a1, int", "void",
     "chk=__explicit_bzero_chk:0/1/size"},
    {"memset_s", "W bytes=min(a1,a3) nullable, int, int, int", "int", ""},
    {"bzero", "W bytes=a1 nullable-if=a1, int", "void", ""},
    {"bcopy", "R bytes=a2 nullable-if=a2, W bytes=a2 nullable-if=a2, int",
     "void", ""},
    {"bcmp", "R bytes=a2 nullable-if=a2, R bytes=a2 nullable-if=a2, int", "int",
     ""},
    {"index", "R str, int", "ptr", ""},
    {"rindex", "R str, int", "ptr", ""},
    {"strcasecmp", "R str, R str", "int", "wrapper=strcasecmp"},
    {"strncasecmp",
     "R bytes=min(a2,strlen(a0)+1) nullable-if=a2, R "
     "bytes=min(a2,strlen(a1)+1) nullable-if=a2, int",
     "int", "wrapper=strncasecmp"},
    {"strcasecmp_l", "R str, R str, none nullable", "int", ""},
    {"strncasecmp_l",
     "R bytes=min(a2,strlen(a0)+1) nullable-if=a2, R "
     "bytes=min(a2,strlen(a1)+1) nullable-if=a2, int, none nullable",
     "int", ""},
});

// <stdio.h>: C17 7.21, POSIX and glibc/BSD extensions. Streams are read and
// written by every operation except the pure queries.
static constexpr auto StdioExpectations = std::to_array<Expectation>({
    {"fopen", "R str, R str", "ptr", ""},
    {"fdopen", "int, R str", "ptr", ""},
    {"freopen", "R str nullable, R str, RW", "ptr", ""},
    {"fmemopen", "W bytes=a1 nullable, int, R str", "ptr", ""},
    {"open_memstream", "W bytes=8 nullable, W nullable", "ptr", ""},
    {"popen", "R str, R str", "ptr", ""},
    {"fflush", "RW nullable", "int", ""},
    {"fpurge", "RW", "int", ""},
    {"setvbuf", "RW, W bytes=a3 nullable, int, int", "int", ""},
    {"setbuf", "RW, W nullable", "void", ""},
    {"setbuffer", "RW, W bytes=a2 nullable, int", "void", ""},
    {"setlinebuf", "RW", "void", ""},
    {"fileno", "R", "int", ""},
    {"fileno_unlocked", "R", "int", ""},
    {"feof", "R", "int", ""},
    {"ferror", "R", "int", ""},
    {"clearerr", "RW", "void", ""},
    {"fseek", "RW, int, int", "int", ""},
    {"fseeko", "RW, int, int", "int", ""},
    {"ftell", "R", "int", ""},
    {"ftello", "R", "int", ""},
    {"rewind", "RW", "void", ""},
    {"fgetpos", "RW, W", "int", ""},
    {"fsetpos", "RW, R", "int", ""},
    {"flockfile", "RW", "void", ""},
    {"funlockfile", "RW", "void", ""},
    {"ftrylockfile", "RW", "int", ""},
    {"fgetc", "RW", "int", ""},
    {"getc", "RW", "int", ""},
    {"getc_unlocked", "RW", "int", ""},
    {"ungetc", "int, RW", "int", ""},
    {"fputc", "int, RW", "int", ""},
    {"putc", "int, RW", "int", ""},
    {"putc_unlocked", "int, RW", "int", ""},
    {"fgets", "W bytes=a1 nullable-if=a1, int, RW", "ptr",
     "chk=__fgets_chk:0/size/1/2"},
    {"gets", "W", "ptr", "wrapper=gets chk=__gets_chk:0/size"},
    {"fgetln", "RW, W", "ptr", ""},
    {"fread", "W bytes=a1*a2 nullable-if=a1*a2, int, int, RW", "int",
     "chk=__fread_chk:0/size/1/2/3"},
    {"fread_unlocked", "W bytes=a1*a2 nullable-if=a1*a2, int, int, RW", "int",
     "chk=__fread_unlocked_chk:0/size/1/2/3"},
    {"fwrite", "R bytes=a1*a2 nullable-if=a1*a2, int, int, RW", "int", ""},
    {"fwrite_unlocked", "R bytes=a1*a2 nullable-if=a1*a2, int, int, RW", "int",
     ""},
    {"puts", "R str nullable", "int", ""},
    {"fputs", "R str nullable, RW", "int", ""},
    {"perror", "R str nullable", "void", ""},
    {"printf", "R str, ...", "int",
     "printf=0/1 chk=__builtin___printf_chk:_/0/1 chk=__printf_chk:_/0/1"},
    {"fprintf", "RW, R str, ...", "int",
     "printf=1/2 chk=__builtin___fprintf_chk:0/_/1/2 "
     "chk=__fprintf_chk:0/_/1/2"},
    {"dprintf", "int, R str, ...", "int",
     "printf=1/2 chk=__dprintf_chk:0/_/1/2"},
    {"sprintf", "W, R str, ...", "int",
     "printf=1/2 wrapper=sprintf chk=__builtin___sprintf_chk:0/_/size/1/2 "
     "chk=__sprintf_chk:0/_/size/1/2"},
    {"snprintf", "W bytes=a1 nullable-if=a1, int, R str, ...", "int",
     "printf=2/3 wrapper=snprintf chk=__builtin___snprintf_chk:0/1/_/size/2/3 "
     "chk=__snprintf_chk:0/1/_/size/2/3"},
    {"vprintf", "R str, other", "int",
     "printf=0/1 chk=__builtin___vprintf_chk:_/0/1 chk=__vprintf_chk:_/0/1"},
    {"vfprintf", "RW, R str, other", "int",
     "printf=1/2 chk=__builtin___vfprintf_chk:0/_/1/2 "
     "chk=__vfprintf_chk:0/_/1/2"},
    {"vdprintf", "int, R str, other", "int",
     "printf=1/2 chk=__vdprintf_chk:0/_/1/2"},
    {"vsprintf", "W, R str, other", "int",
     "printf=1/2 wrapper=vsprintf chk=__builtin___vsprintf_chk:0/_/size/1/2 "
     "chk=__vsprintf_chk:0/_/size/1/2"},
    {"vsnprintf", "W bytes=a1 nullable-if=a1, int, R str, other", "int",
     "printf=2/3 wrapper=vsnprintf "
     "chk=__builtin___vsnprintf_chk:0/1/_/size/2/3 "
     "chk=__vsnprintf_chk:0/1/_/size/2/3"},
    {"scanf", "R str, ...", "int",
     "scanf=0/1 wrapper=scanf chk=__isoc99_scanf:0/1 chk=__isoc23_scanf:0/1"},
    {"fscanf", "RW, R str, ...", "int",
     "scanf=1/2 wrapper=fscanf chk=__isoc99_fscanf:0/1/2 "
     "chk=__isoc23_fscanf:0/1/2"},
    {"sscanf", "R str, R str, ...", "int",
     "scanf=1/2 wrapper=sscanf chk=__isoc99_sscanf:0/1/2 "
     "chk=__isoc23_sscanf:0/1/2"},
    {"vscanf", "R str, other", "int",
     "scanf=0/1 wrapper=vscanf chk=__isoc99_vscanf:0/1 "
     "chk=__isoc23_vscanf:0/1"},
    {"vfscanf", "RW, R str, other", "int",
     "scanf=1/2 wrapper=vfscanf chk=__isoc99_vfscanf:0/1/2 "
     "chk=__isoc23_vfscanf:0/1/2"},
    {"vsscanf", "R str, R str, other", "int",
     "scanf=1/2 wrapper=vsscanf chk=__isoc99_vsscanf:0/1/2 "
     "chk=__isoc23_vsscanf:0/1/2"},
    {"remove", "R str", "int", ""},
    {"rename", "R str, R str", "int", ""},
    {"renameat", "int, R str, int, R str", "int", ""},
    {"tmpnam", "W nullable", "ptr", ""},
    {"ctermid", "W nullable", "ptr", ""},
});

// <stdlib.h>, <inttypes.h>: conversions, environment, sorting and exit.
static constexpr auto StdlibExpectations = std::to_array<Expectation>({
    {"atoi", "R str", "int", "wrapper=atoi"},
    {"atol", "R str", "int", "wrapper=atol"},
    {"atoll", "R str", "int", "wrapper=atoll"},
    {"atof", "R str", "int", "wrapper=atof"},
    {"strtol", "R str, W bytes=8 nullable, int", "int",
     "wrapper=strtol chk=__isoc23_strtol:0/1/2"},
    {"strtoll", "R str, W bytes=8 nullable, int", "int",
     "wrapper=strtoll chk=__isoc23_strtoll:0/1/2"},
    {"strtoul", "R str, W bytes=8 nullable, int", "int",
     "wrapper=strtoul chk=__isoc23_strtoul:0/1/2"},
    {"strtoull", "R str, W bytes=8 nullable, int", "int",
     "wrapper=strtoull chk=__isoc23_strtoull:0/1/2"},
    {"strtod", "R str, W bytes=8 nullable", "int", "wrapper=strtod"},
    {"strtof", "R str, W bytes=8 nullable", "int", "wrapper=strtof"},
    {"strtold", "R str, W bytes=8 nullable", "int", "wrapper=strtold"},
    {"strtoimax", "R str, W bytes=8 nullable, int", "int",
     "wrapper=strtoimax chk=__isoc23_strtoimax:0/1/2"},
    {"strtoumax", "R str, W bytes=8 nullable, int", "int",
     "wrapper=strtoumax chk=__isoc23_strtoumax:0/1/2"},
    {"getenv", "R str nullable", "ptr", ""},
    {"secure_getenv", "R str", "ptr", ""},
    {"setenv", "R str nullable, R str, int", "int", ""},
    {"unsetenv", "R str nullable", "int", ""},
    {"putenv", "R str nullable", "int", ""},
    {"system", "R str nullable", "int", ""},
    {"qsort", "RW bytes=a1*a2 nullable-if=a1*a2, int, int, fn", "void", ""},
    {"qsort_r", "RW bytes=a1*a2 nullable-if=a1*a2, int, int, fn, none nullable",
     "void", ""},
    {"qsort_r#2",
     "RW bytes=a1*a2 nullable-if=a1*a2, int, int, none nullable, fn", "void",
     ""},
    {"bsearch", "none nullable, R bytes=a2*a3 nullable-if=a2*a3, int, int, fn",
     "ptr", ""},
    {"mblen", "R bytes=min(a1,strlen(a0)+1) nullable, int", "int", ""},
    {"mbtowc", "W nullable, R bytes=min(a2,strlen(a1)+1) nullable, int", "int",
     ""},
    {"wctomb", "W nullable, int", "int", "chk=__wctomb_chk:0/1/size"},
    {"mbstowcs", "W bytes=a2*4 nullable, R str, int", "int",
     "chk=__mbstowcs_chk:0/1/2/_"},
    {"wcstombs", "W bytes=a2 nullable, R str, int", "int",
     "chk=__wcstombs_chk:0/1/2/size"},
    {"mkstemp", "RW str", "int", ""},
    {"mkostemp", "RW str, int", "int", ""},
    {"mkstemps", "RW str, int", "int", ""},
    {"mkdtemp", "RW str", "ptr", ""},
    {"mktemp", "RW str", "ptr", ""},
    {"rand_r", "RW", "int", ""},
    {"initstate", "int, W bytes=a2, int", "ptr", ""},
    {"setstate", "RW", "ptr", ""},
    {"erand48", "RW bytes=6", "int", ""},
    {"nrand48", "RW bytes=6", "int", ""},
    {"jrand48", "RW bytes=6", "int", ""},
    {"seed48", "R bytes=6", "ptr", ""},
    {"lcong48", "R bytes=14", "void", ""},
    {"arc4random_buf", "W bytes=a1, int", "void", ""},
    {"getsubopt", "RW, R, W bytes=8", "int", ""},
    {"a64l", "R str", "int", ""},
    {"getloadavg", "W bytes=min(a1,3)*8 nullable-if=a1, int", "int", ""},
    {"setprogname", "R str", "void", ""},
    {"ptsname_r", "int, W bytes=a2 nullable, int", "int", ""},
});

// <unistd.h>, <fcntl.h>, <sys/stat.h> and friends: POSIX.1-2024.
static constexpr auto UnistdExpectations = std::to_array<Expectation>({
    {"read", "int, W bytes=a2 nullable-if=a2, int", "int",
     "chk=__read_chk:0/1/2/size"},
    {"write", "int, R bytes=a2 nullable-if=a2, int", "int", ""},
    {"pread", "int, W bytes=a2 nullable-if=a2, int, int", "int",
     "chk=__pread_chk:0/1/2/3/size chk=__pread64_chk:0/1/2/3/size"},
    {"pwrite", "int, R bytes=a2 nullable-if=a2, int, int", "int", ""},
    {"pipe", "W bytes=8", "int", ""},
    {"pipe2", "W bytes=8, int", "int", ""},
    {"access", "R str, int", "int", ""},
    {"faccessat", "int, R str, int, int", "int", ""},
    {"unlink", "R str", "int", ""},
    {"unlinkat", "int, R str, int", "int", ""},
    {"rmdir", "R str", "int", ""},
    {"chdir", "R str", "int", ""},
    {"getwd", "W", "ptr", ""},
    {"readlink", "R str, W bytes=a2 nullable-if=a2, int", "int",
     "chk=__readlink_chk:0/1/2/size"},
    {"readlinkat", "int, R str, W bytes=a3 nullable-if=a3, int", "int",
     "chk=__readlinkat_chk:0/1/2/3/size"},
    {"symlink", "R str, R str", "int", ""},
    {"symlinkat", "R str, int, R str", "int", ""},
    {"link", "R str, R str", "int", ""},
    {"linkat", "int, R str, int, R str, int", "int", ""},
    {"chown", "R str, int, int", "int", ""},
    {"lchown", "R str, int, int", "int", ""},
    {"fchownat", "int, R str, int, int, int", "int", ""},
    {"truncate", "R str, int", "int", ""},
    {"ttyname_r", "int, W bytes=a2 nullable, int", "int",
     "chk=__ttyname_r_chk:0/1/2/size"},
    {"gethostname", "W bytes=a1, int", "int", "chk=__gethostname_chk:0/1/size"},
    {"sethostname", "R bytes=a1, int", "int", ""},
    {"getdomainname", "W bytes=a1, int", "int",
     "chk=__getdomainname_chk:0/1/size"},
    {"getlogin_r", "W bytes=a1, int", "int", "chk=__getlogin_r_chk:0/1/size"},
    {"getopt", "int, RW bytes=a0*8, R str", "int", ""},
    {"execv", "R str, R nullable", "int", ""},
    {"execve", "R str, R nullable, R nullable", "int", ""},
    {"execvp", "R str, R nullable", "int", ""},
    {"execvpe", "R str, R nullable, R nullable", "int", ""},
    {"execl", "R str, R str nullable, ...", "int", ""},
    {"execlp", "R str, R str nullable, ...", "int", ""},
    {"execle", "R str, R str nullable, ...", "int", ""},
    {"fexecve", "int, R nullable, R nullable", "int", ""},
    {"pathconf", "R str, int", "int", ""},
    {"confstr", "int, W bytes=a2 nullable-if=a2, int", "int",
     "chk=__confstr_chk:0/1/2/size"},
    {"chroot", "R str", "int", ""},
    {"getentropy", "W bytes=a1 nullable-if=a1, int", "int", ""},
    {"getgroups", "int, W bytes=a0*4 nullable-if=a0", "int",
     "chk=__getgroups_chk:0/1/size"},
    {"swab", "R bytes=a2 nullable-if=a2, W bytes=a2 nullable-if=a2, int",
     "void", "disjoint=0/1/a2"},
    {"getopt_long", "int, RW bytes=a0*8, R str, R nullable, W nullable", "int",
     ""},
    {"getopt_long_only", "int, RW bytes=a0*8, R str, R nullable, W nullable",
     "int", ""},
    {"open", "R str, int, ...", "int", "chk=__open_2:0/1"},
    {"openat", "int, R str, int, ...", "int", "chk=__openat_2:0/1/2"},
    {"creat", "R str, int", "int", ""},
    {"stat", "R str, W", "int", ""},
    {"fstat", "int, W", "int", ""},
    {"lstat", "R str, W", "int", ""},
    {"fstatat", "int, R str, W, int", "int", ""},
    {"chmod", "R str, int", "int", ""},
    {"fchmodat", "int, R str, int, int", "int", ""},
    {"mkdir", "R str, int", "int", ""},
    {"mkdirat", "int, R str, int", "int", ""},
    {"mkfifo", "R str, int", "int", ""},
    {"mkfifoat", "int, R str, int", "int", ""},
    {"mknod", "R str, int, int", "int", ""},
    {"mknodat", "int, R str, int, int", "int", ""},
    {"utimensat", "int, R str nullable, R bytes=32 nullable, int", "int", ""},
    {"futimens", "int, R bytes=32 nullable", "int", ""},
    {"statvfs", "R str, W", "int", ""},
    {"fstatvfs", "int, W", "int", ""},
    {"statfs", "R str, W", "int", ""},
    {"fstatfs", "int, W", "int", ""},
    {"utime", "R str, R nullable", "int", ""},
    {"utimes", "R str, R bytes=32 nullable", "int", ""},
    {"futimes", "int, R bytes=32 nullable", "int", ""},
    {"lutimes", "R str, R bytes=32 nullable", "int", ""},
    {"gettimeofday", "W nullable, W nullable", "int", ""},
    {"settimeofday", "R nullable, R nullable", "int", ""},
    {"getitimer", "int, W", "int", ""},
    {"setitimer", "int, R nullable, W nullable", "int", ""},
});

// <dirent.h>, <time.h>, <sys/mman.h>, <dlfcn.h>.
static constexpr auto SystemExpectations = std::to_array<Expectation>({
    {"opendir", "R str", "ptr", ""},
    {"readdir", "RW", "ptr", ""},
    {"readdir_r", "RW, W, W bytes=8", "int", ""},
    {"rewinddir", "RW", "void", ""},
    {"seekdir", "RW, int", "void", ""},
    {"telldir", "R", "int", ""},
    {"dirfd", "R", "int", ""},
    {"scandir", "R str, W bytes=8, fn, fn", "int", ""},
    {"alphasort", "R, R", "int", ""},
    {"versionsort", "R, R", "int", ""},
    {"time", "W nullable", "int", ""},
    {"mktime", "RW", "int", ""},
    {"timegm", "RW", "int", ""},
    {"asctime", "R", "ptr", ""},
    {"ctime", "R", "ptr", ""},
    {"gmtime", "R", "ptr", ""},
    {"localtime", "R", "ptr", ""},
    {"asctime_r", "R, W bytes=26", "ptr", ""},
    {"ctime_r", "R, W bytes=26", "ptr", ""},
    {"gmtime_r", "R, W", "ptr", ""},
    {"localtime_r", "R, W", "ptr", ""},
    {"strftime", "W bytes=a1 nullable-if=a1, int, R str, R", "int", ""},
    {"strftime_l", "W bytes=a1 nullable-if=a1, int, R str, R, none nullable",
     "int", ""},
    {"strptime", "R str, R str, W", "ptr", ""},
    {"timespec_get", "W, int", "int", ""},
    {"timespec_getres", "W nullable, int", "int", ""},
    {"clock_gettime", "int, W", "int", ""},
    {"clock_getres", "int, W nullable", "int", ""},
    {"clock_settime", "int, R", "int", ""},
    {"clock_nanosleep", "int, int, R, W nullable", "int", ""},
    {"nanosleep", "R, W nullable", "int", ""},
    {"timer_create", "int, R nullable, W", "int", ""},
    {"timer_settime", "none nullable, int, R, W nullable", "int", ""},
    {"timer_gettime", "none nullable, W", "int", ""},
    {"mmap", "none nullable, int, int, int, int, int", "ptr", "wrapper=mmap"},
    {"munmap", "none nullable releases, int", "int", "wrapper=munmap"},
    {"mincore", "none nullable, int, W nullable-if=a1", "int", ""},
    {"shm_open", "R str, int, int", "int", ""},
    {"shm_unlink", "R str", "int", ""},
    {"memfd_create", "R str, int", "int", ""},
    {"dlopen", "R str nullable, int", "ptr", ""},
    {"dlsym", "none nullable, R str", "ptr", ""},
    {"dladdr", "none nullable, W", "int", ""},
});

// Users and groups, locales, iconv, paths, patterns and messages.
static constexpr auto UsersExpectations = std::to_array<Expectation>({
    {"getpwnam", "R str", "ptr", ""},
    {"getpwnam_r", "R str, W, W bytes=a3, int, W bytes=8", "int", ""},
    {"getpwuid_r", "int, W, W bytes=a3, int, W bytes=8", "int", ""},
    {"getgrnam", "R str", "ptr", ""},
    {"getgrnam_r", "R str, W, W bytes=a3, int, W bytes=8", "int", ""},
    {"getgrgid_r", "int, W, W bytes=a3, int, W bytes=8", "int", ""},
    {"setgroups", "int, R bytes=a0*4 nullable-if=a0", "int", ""},
    {"initgroups", "R str, int", "int", ""},
    {"setlocale", "int, R str nullable", "ptr", ""},
    {"newlocale", "int, R str nullable, none nullable reallocs", "ptr", ""},
    {"iconv_open", "R str, R str", "ptr", ""},
    {"iconv", "none, RW nullable, RW nullable, RW nullable, RW nullable", "int",
     ""},
    {"basename", "RW str nullable", "ptr", "chk=__xpg_basename:0"},
    {"dirname", "RW str nullable", "ptr", ""},
    {"regcomp", "W, R str, int", "int", ""},
    {"regexec", "R, R str, int, W bytes=a2*8 nullable, int", "int", ""},
    {"regerror", "int, R nullable, W bytes=a3 nullable-if=a3, int", "int", ""},
    {"regfree", "RW", "void", ""},
    {"fnmatch", "R str, R str, int", "int", ""},
    {"glob", "R str, int, fn, RW", "int", ""},
    {"globfree", "RW", "void", ""},
    {"wordexp", "R str, RW, int", "int", ""},
    {"wordfree", "RW nullable", "void", ""},
    {"err", "int, R str nullable, ...", "void", "printf=1/2"},
    {"errx", "int, R str nullable, ...", "void", "printf=1/2"},
    {"verr", "int, R str nullable, other", "void", "printf=1/2"},
    {"verrx", "int, R str nullable, other", "void", "printf=1/2"},
    {"warn", "R str nullable, ...", "void", "printf=0/1"},
    {"warnx", "R str nullable, ...", "void", "printf=0/1"},
    {"vwarn", "R str nullable, other", "void", "printf=0/1"},
    {"vwarnx", "R str nullable, other", "void", "printf=0/1"},
    {"error", "int, int, R str, ...", "void", "printf=2/3"},
    {"error_at_line", "int, int, R str nullable, int, R str, ...", "void",
     "printf=4/5"},
    {"openlog", "R str nullable, int, int", "void", ""},
    {"syslog", "int, R str, ...", "void",
     "printf=1/2 chk=__syslog_chk:0/_/1/2"},
    {"vsyslog", "int, R str, other", "void",
     "printf=1/2 chk=__vsyslog_chk:0/_/1/2"},
    {"uname", "W", "int", ""},
    {"getrlimit", "int, W", "int", ""},
    {"setrlimit", "int, R", "int", ""},
    {"getrusage", "int, W", "int", ""},
    {"tcgetattr", "int, W", "int", ""},
    {"tcsetattr", "int, int, R", "int", ""},
    {"cfmakeraw", "W", "void", ""},
    {"cfgetispeed", "R", "int", ""},
    {"cfgetospeed", "R", "int", ""},
    {"cfsetispeed", "W, int", "int", ""},
    {"cfsetospeed", "W, int", "int", ""},
});

// <pthread.h>, <threads.h> (§5.3: thread starts and destructors are entry
// points; once-routines run synchronously).
static constexpr auto ThreadExpectations = std::to_array<Expectation>({
    {"pthread_create", "W, R nullable, fn, none nullable", "int", ""},
    {"pthread_join", "other, W nullable", "int", ""},
    {"pthread_setcancelstate", "int, W nullable", "int", ""},
    {"pthread_setcanceltype", "int, W nullable", "int", ""},
    {"pthread_attr_init", "W", "int", ""},
    {"pthread_attr_destroy", "RW", "int", ""},
    {"pthread_attr_setdetachstate", "RW, int", "int", ""},
    {"pthread_attr_getdetachstate", "R, W", "int", ""},
    {"pthread_attr_setstacksize", "RW, int", "int", ""},
    {"pthread_attr_getstacksize", "R, W", "int", ""},
    {"pthread_attr_setstack", "RW, none, int", "int", ""},
    {"pthread_mutex_init", "W, R nullable", "int", ""},
    {"pthread_mutex_destroy", "RW", "int", ""},
    {"pthread_mutex_lock", "RW", "int", ""},
    {"pthread_mutex_trylock", "RW", "int", ""},
    {"pthread_mutex_timedlock", "RW, R", "int", ""},
    {"pthread_mutex_unlock", "RW", "int", ""},
    {"pthread_mutexattr_init", "W", "int", ""},
    {"pthread_mutexattr_destroy", "RW", "int", ""},
    {"pthread_mutexattr_settype", "RW, int", "int", ""},
    {"pthread_mutexattr_gettype", "R, W", "int", ""},
    {"pthread_mutexattr_setpshared", "RW, int", "int", ""},
    {"pthread_cond_init", "W, R nullable", "int", ""},
    {"pthread_cond_destroy", "RW", "int", ""},
    {"pthread_cond_wait", "RW, RW", "int", ""},
    {"pthread_cond_timedwait", "RW, RW, R nullable", "int", ""},
    {"pthread_cond_signal", "RW", "int", ""},
    {"pthread_cond_broadcast", "RW", "int", ""},
    {"pthread_condattr_init", "W", "int", ""},
    {"pthread_condattr_destroy", "RW", "int", ""},
    {"pthread_condattr_setclock", "RW, int", "int", ""},
    {"pthread_rwlock_init", "W, R nullable", "int", ""},
    {"pthread_rwlock_destroy", "RW", "int", ""},
    {"pthread_rwlock_rdlock", "RW", "int", ""},
    {"pthread_rwlock_wrlock", "RW", "int", ""},
    {"pthread_rwlock_tryrdlock", "RW", "int", ""},
    {"pthread_rwlock_trywrlock", "RW", "int", ""},
    {"pthread_rwlock_unlock", "RW", "int", ""},
    {"pthread_spin_init", "W, int", "int", ""},
    {"pthread_spin_destroy", "RW", "int", ""},
    {"pthread_spin_lock", "RW", "int", ""},
    {"pthread_spin_trylock", "RW", "int", ""},
    {"pthread_spin_unlock", "RW", "int", ""},
    {"pthread_barrier_init", "W, R nullable, int", "int", ""},
    {"pthread_barrier_wait", "RW", "int", ""},
    {"pthread_barrier_destroy", "RW", "int", ""},
    {"pthread_once", "RW, fn", "int", ""},
    {"pthread_key_create", "W, fn", "int", ""},
    {"pthread_sigmask", "int, R nullable, W nullable", "int", ""},
    {"pthread_setname_np", "other, R str", "int", ""},
    {"pthread_setname_np#2", "R str", "int", ""},
    {"pthread_getname_np", "other, W bytes=a2, int", "int", ""},
    {"thrd_create", "W, fn, none nullable", "int", ""},
    {"thrd_join", "other, W nullable", "int", ""},
    {"thrd_sleep", "R, W nullable", "int", ""},
    {"mtx_init", "W, int", "int", ""},
    {"mtx_lock", "RW", "int", ""},
    {"mtx_trylock", "RW", "int", ""},
    {"mtx_timedlock", "RW, R", "int", ""},
    {"mtx_unlock", "RW", "int", ""},
    {"mtx_destroy", "RW", "void", ""},
    {"cnd_init", "W", "int", ""},
    {"cnd_signal", "RW", "int", ""},
    {"cnd_broadcast", "RW", "int", ""},
    {"cnd_wait", "RW, RW", "int", ""},
    {"cnd_timedwait", "RW, RW, R", "int", ""},
    {"cnd_destroy", "RW", "void", ""},
    {"tss_create", "W, fn", "int", ""},
    {"call_once", "RW, fn", "void", ""},
});

// Signals (handlers are entry points), non-local jumps (§5.4), scheduling,
// semaphores, trees and searches (synchronous comparators), walks, spawning
// and polling.
static constexpr auto SignalExpectations = std::to_array<Expectation>({
    {"sigaction", "int, R nullable, W nullable", "int", ""},
    {"sigemptyset", "W", "int", ""},
    {"sigfillset", "W", "int", ""},
    {"sigaddset", "RW, int", "int", ""},
    {"sigdelset", "RW, int", "int", ""},
    {"sigismember", "R, int", "int", ""},
    {"sigprocmask", "int, R nullable, W nullable", "int", ""},
    {"sigsuspend", "R", "int", ""},
    {"sigwait", "R, W", "int", ""},
    {"sigwaitinfo", "R, W nullable", "int", ""},
    {"sigtimedwait", "R, W nullable, R nullable", "int", ""},
    {"sigpending", "W", "int", ""},
    {"sigaltstack", "R nullable, W nullable", "int", ""},
    {"psignal", "int, R str nullable", "void", ""},
    {"psiginfo", "R, R str nullable", "void", ""},
    {"wait", "W nullable", "int", ""},
    {"waitpid", "int, W nullable, int", "int", ""},
    {"waitid", "int, int, W nullable, int", "int", ""},
    {"wait4", "int, W nullable, int, W nullable", "int", ""},
    {"sched_getaffinity", "int, int, W bytes=a1", "int", ""},
    {"sched_setaffinity", "int, int, R bytes=a1", "int", ""},
    {"sched_getparam", "int, W nullable", "int", ""},
    {"sched_setparam", "int, R nullable", "int", ""},
    {"sched_setscheduler", "int, int, R nullable", "int", ""},
    {"sem_open", "R str, int, ...", "ptr", ""},
    {"sem_unlink", "R str", "int", ""},
    {"sem_init", "W, int, int", "int", ""},
    {"sem_timedwait", "none, R", "int", ""},
    {"sem_getvalue", "none, W", "int", ""},
    {"tsearch", "none nullable, RW nullable, fn", "ptr", ""},
    {"tfind", "none nullable, R nullable, fn", "ptr", ""},
    {"tdelete", "none nullable, RW nullable, fn", "ptr", ""},
    {"lfind", "none nullable, R nullable, RW, int, fn", "ptr", ""},
    {"lsearch", "none nullable, RW, RW, int, fn", "ptr", ""},
    {"ftw", "R str, fn, int", "int", ""},
    {"nftw", "R str, fn, int, int", "int", ""},
    {"posix_spawn",
     "W nullable, R str, R nullable, R nullable, R nullable, R nullable", "int",
     ""},
    {"posix_spawnp",
     "W nullable, R str, R nullable, R nullable, R nullable, R nullable", "int",
     ""},
    {"poll", "RW bytes=a1*8 nullable-if=a1, int, int", "int",
     "chk=__poll_chk:0/1/2/size"},
    {"ppoll", "RW bytes=a1*8 nullable-if=a1, int, R nullable, R nullable",
     "int", "chk=__ppoll_chk:0/1/2/3/size"},
    {"select",
     "int, RW bytes=(a0+7)/8 nullable, RW bytes=(a0+7)/8 nullable, RW "
     "bytes=(a0+7)/8 nullable, RW nullable",
     "int", ""},
    {"pselect",
     "int, RW bytes=(a0+7)/8 nullable, RW bytes=(a0+7)/8 nullable, RW "
     "bytes=(a0+7)/8 nullable, R nullable, R nullable",
     "int", ""},
    {"epoll_ctl", "int, int, int, R nullable", "int", ""},
    {"epoll_wait", "int, W bytes=a2*12 nullable-if=a2, int, int", "int", ""},
    {"epoll_pwait", "int, W bytes=a2*12 nullable-if=a2, int, int, R nullable",
     "int", ""},
    {"kevent",
     "int, R bytes=a2*32 nullable-if=a2, int, W bytes=a4*32 nullable-if=a4, "
     "int, R nullable",
     "int", ""},
});

// Sockets, name resolution, interfaces and the remaining system interfaces.
static constexpr auto NetworkExpectations = std::to_array<Expectation>({
    {"socketpair", "int, int, int, W bytes=8", "int", ""},
    {"bind", "int, R bytes=a2 nullable-if=a2, int", "int", ""},
    {"connect", "int, R bytes=a2 nullable-if=a2, int", "int", ""},
    {"accept", "int, W nullable, RW nullable", "int", ""},
    {"accept4", "int, W nullable, RW nullable, int", "int", ""},
    {"send", "int, R bytes=a2 nullable-if=a2, int, int", "int", ""},
    {"sendto",
     "int, R bytes=a2 nullable-if=a2, int, int, R bytes=a5 nullable, int",
     "int", ""},
    {"sendmsg", "int, R, int", "int", ""},
    {"recv", "int, W bytes=a2 nullable-if=a2, int, int", "int",
     "chk=__recv_chk:0/1/2/size/3"},
    {"recvfrom",
     "int, W bytes=a2 nullable-if=a2, int, int, W nullable, RW nullable", "int",
     "chk=__recvfrom_chk:0/1/2/size/3/4/5"},
    {"recvmsg", "int, RW, int", "int", ""},
    {"getsockopt", "int, int, int, W nullable, RW", "int", ""},
    {"setsockopt", "int, int, int, R bytes=a4 nullable-if=a4, int", "int", ""},
    {"getsockname", "int, W, RW", "int", ""},
    {"getpeername", "int, W, RW", "int", ""},
    {"getaddrinfo", "R str nullable, R str nullable, R nullable, W bytes=8",
     "int", ""},
    {"getnameinfo",
     "R bytes=a1 nullable, int, W bytes=a3 nullable, int, W bytes=a5 nullable, "
     "int, int",
     "int", ""},
    {"gethostbyname", "R str", "ptr", ""},
    {"gethostbyname2", "R str, int", "ptr", ""},
    {"gethostbyaddr", "R bytes=a1, int, int", "ptr", ""},
    {"getservbyname", "R str, R str nullable", "ptr", ""},
    {"getservbyport", "int, R str nullable", "ptr", ""},
    {"getprotobyname", "R str", "ptr", ""},
    {"herror", "R str nullable", "void", ""},
    {"inet_ntop", "int, R, W bytes=a3, int", "ptr", ""},
    {"inet_pton", "int, R str, W", "int", ""},
    {"inet_addr", "R str", "int", ""},
    {"inet_aton", "R str, W nullable", "int", ""},
    {"inet_network", "R str", "int", ""},
    {"getifaddrs", "W bytes=8", "int", ""},
    {"if_nametoindex", "R str", "int", ""},
    {"if_indextoname", "int, W", "ptr", ""},
    {"readv", "int, R bytes=a2*16 nullable-if=a2, int", "int", ""},
    {"writev", "int, R bytes=a2*16 nullable-if=a2, int", "int", ""},
    {"preadv", "int, R bytes=a2*16 nullable-if=a2, int, int", "int", ""},
    {"pwritev", "int, R bytes=a2*16 nullable-if=a2, int, int", "int", ""},
    {"sendfile", "int, int, RW nullable, int", "int", ""},
    {"getrandom", "W bytes=a1 nullable-if=a1, int, int", "int", ""},
    {"inotify_add_watch", "int, R str, int", "int", ""},
    {"timerfd_settime", "int, int, R, W nullable", "int", ""},
    {"timerfd_gettime", "int, W", "int", ""},
    {"signalfd", "int, R, int", "int", ""},
    {"sysinfo", "W", "int", ""},
    {"sysctlbyname", "R str, W nullable, RW nullable, R nullable, int", "int",
     ""},
    {"shmctl", "int, int, RW nullable", "int", ""},
    {"mq_open", "R str, int, ...", "int", ""},
    {"mq_unlink", "R str", "int", ""},
    {"mq_send", "int, R bytes=a2 nullable-if=a2, int, int", "int", ""},
    {"mq_receive", "int, W bytes=a2, int, W nullable", "int", ""},
    {"aio_read", "RW", "int", ""},
    {"aio_write", "RW", "int", ""},
    {"aio_error", "R", "int", ""},
    {"aio_return", "RW", "int", ""},
    {"aio_suspend", "R bytes=a1*8, int, R nullable", "int", ""},
    {"aio_cancel", "int, RW nullable", "int", ""},
    {"setmntent", "R str, R str", "ptr", ""},
    {"getmntent", "RW", "ptr", ""},
    {"fts_open", "R, int, fn", "ptr", ""},
    {"fts_read", "RW", "ptr", ""},
    {"fts_children", "RW, int", "ptr", ""},
    {"fts_set", "RW, RW, int", "int", ""},
});

// <wchar.h>, <wctype.h>, <uchar.h>: wide lengths count wchar_t elements;
// multibyte buffers count bytes.
static constexpr auto WideExpectations = std::to_array<Expectation>({
    {"wcslen", "R str", "int value=strlen(a0)", "wide"},
    {"wcsnlen", "R count=min(a1,strlen(a0)+1) nullable-if=a1, int",
     "int value=min(a1,strlen(a0))", "wide"},
    {"wcscpy", "W count=strlen(a1)+1, R str", "ptr",
     "wide chk=__wcscpy_chk:0/1/size"},
    {"wcsncpy",
     "W count=a2 nullable-if=a2, R count=min(a2,strlen(a1)+1) nullable-if=a2, "
     "int",
     "ptr", "wide chk=__wcsncpy_chk:0/1/2/size"},
    {"wcpcpy", "W count=strlen(a1)+1, R str", "ptr",
     "wide chk=__wcpcpy_chk:0/1/size"},
    {"wcpncpy",
     "W count=a2 nullable-if=a2, R count=min(a2,strlen(a1)+1) nullable-if=a2, "
     "int",
     "ptr", "wide chk=__wcpncpy_chk:0/1/2/size"},
    {"wcscat", "RW str count=strlen(a0)+strlen(a1)+1, R str", "ptr",
     "wide chk=__wcscat_chk:0/1/size"},
    {"wcsncat",
     "RW str count=strlen(a0)+min(a2,strlen(a1))+1, R "
     "count=min(a2,strlen(a1)+1) nullable-if=a2, int",
     "ptr", "wide chk=__wcsncat_chk:0/1/2/size"},
    {"wcscmp", "R str, R str", "int", "wide"},
    {"wcsncmp",
     "R count=min(a2,strlen(a0)+1) nullable-if=a2, R "
     "count=min(a2,strlen(a1)+1) nullable-if=a2, int",
     "int", "wide"},
    {"wcscasecmp", "R str, R str", "int", "wide"},
    {"wcsncasecmp",
     "R count=min(a2,strlen(a0)+1) nullable-if=a2, R "
     "count=min(a2,strlen(a1)+1) nullable-if=a2, int",
     "int", "wide"},
    {"wcscoll", "R str, R str", "int", "wide"},
    {"wcsxfrm", "W count=a2 nullable-if=a2, R str, int", "int", "wide"},
    {"wcschr", "R str, int", "ptr", "wide"},
    {"wcsrchr", "R str, int", "ptr", "wide"},
    {"wcsstr", "R str, R str", "ptr", "wide"},
    {"wcspbrk", "R str, R str", "ptr", "wide"},
    {"wcsspn", "R str, R str", "int", "wide"},
    {"wcscspn", "R str, R str", "int", "wide"},
    {"wcstok", "RW str nullable, R str, RW bytes=8", "ptr", "wide"},
    {"wcswidth", "R count=min(a1,strlen(a0)+1) nullable-if=a1, int", "int",
     "wide"},
    {"wmemcpy", "W count=a2 nullable-if=a2, R count=a2 nullable-if=a2, int",
     "ptr", "wide chk=__wmemcpy_chk:0/1/2/size"},
    {"wmemmove", "W count=a2 nullable-if=a2, R count=a2 nullable-if=a2, int",
     "ptr", "wide chk=__wmemmove_chk:0/1/2/size"},
    {"wmemset", "W count=a2 nullable-if=a2, int, int", "ptr",
     "wide chk=__wmemset_chk:0/1/2/size"},
    {"wmemcmp", "R count=a2 nullable-if=a2, R count=a2 nullable-if=a2, int",
     "int", "wide"},
    {"wmemchr", "R count=min(a2,strlen(a0)+1) nullable-if=a2, int, int", "ptr",
     "wide"},
    {"wmempcpy", "W count=a2 nullable-if=a2, R count=a2 nullable-if=a2, int",
     "ptr", "wide chk=__wmempcpy_chk:0/1/2/size"},
    {"wcstol", "R str, W bytes=8 nullable, int", "int",
     "wide chk=__isoc23_wcstol:0/1/2"},
    {"wcstoll", "R str, W bytes=8 nullable, int", "int",
     "wide chk=__isoc23_wcstoll:0/1/2"},
    {"wcstoul", "R str, W bytes=8 nullable, int", "int",
     "wide chk=__isoc23_wcstoul:0/1/2"},
    {"wcstoull", "R str, W bytes=8 nullable, int", "int",
     "wide chk=__isoc23_wcstoull:0/1/2"},
    {"wcstod", "R str, W bytes=8 nullable", "int", "wide"},
    {"wcstof", "R str, W bytes=8 nullable", "int", "wide"},
    {"wcstold", "R str, W bytes=8 nullable", "int", "wide"},
    {"mbsinit", "R nullable", "int", "wide"},
    {"mbrlen", "R bytes=min(a1,strlen(a0)+1) nullable, int, RW nullable", "int",
     "wide"},
    {"mbrtowc",
     "W nullable, R bytes=min(a2,strlen(a1)+1) nullable, int, RW nullable",
     "int", "wide"},
    {"wcrtomb", "W nullable, int, RW nullable", "int",
     "wide chk=__wcrtomb_chk:0/1/2/size"},
    {"mbsrtowcs", "W count=a2 nullable, RW, int, RW nullable", "int",
     "wide chk=__mbsrtowcs_chk:0/1/2/3/size"},
    {"wcsrtombs", "W bytes=a2 nullable, RW, int, RW nullable", "int",
     "wide chk=__wcsrtombs_chk:0/1/2/3/size"},
    {"mbsnrtowcs", "W count=a3 nullable, RW, int, int, RW nullable", "int",
     "wide chk=__mbsnrtowcs_chk:0/1/2/3/4/size"},
    {"wcsnrtombs", "W bytes=a3 nullable, RW, int, int, RW nullable", "int",
     "wide chk=__wcsnrtombs_chk:0/1/2/3/4/size"},
    {"fgetwc", "RW", "int", "wide"},
    {"getwc", "RW", "int", "wide"},
    {"fputwc", "int, RW", "int", "wide"},
    {"putwc", "int, RW", "int", "wide"},
    {"ungetwc", "int, RW", "int", "wide"},
    {"fgetws", "W count=a1 nullable-if=a1, int, RW", "ptr",
     "wide chk=__fgetws_chk:0/size/1/2"},
    {"fputws", "R str, RW", "int", "wide"},
    {"fwide", "RW, int", "int", "wide"},
    {"wprintf", "R str, ...", "int", "wide printf=0/1 chk=__wprintf_chk:_/0/1"},
    {"fwprintf", "RW, R str, ...", "int",
     "wide printf=1/2 chk=__fwprintf_chk:0/_/1/2"},
    {"swprintf", "W count=a1 nullable-if=a1, int, R str, ...", "int",
     "wide printf=2/3 chk=__swprintf_chk:0/1/_/size/2/3"},
    {"vwprintf", "R str, other", "int",
     "wide printf=0/1 chk=__vwprintf_chk:_/0/1"},
    {"vfwprintf", "RW, R str, other", "int",
     "wide printf=1/2 chk=__vfwprintf_chk:0/_/1/2"},
    {"vswprintf", "W count=a1 nullable-if=a1, int, R str, other", "int",
     "wide printf=2/3 chk=__vswprintf_chk:0/1/_/size/2/3"},
    {"wscanf", "R str, ...", "int",
     "wide scanf=0/1 chk=__isoc99_wscanf:0/1 chk=__isoc23_wscanf:0/1"},
    {"fwscanf", "RW, R str, ...", "int",
     "wide scanf=1/2 chk=__isoc99_fwscanf:0/1/2 chk=__isoc23_fwscanf:0/1/2"},
    {"swscanf", "R str, R str, ...", "int",
     "wide scanf=1/2 chk=__isoc99_swscanf:0/1/2 chk=__isoc23_swscanf:0/1/2"},
    {"vwscanf", "R str, other", "int",
     "wide scanf=0/1 chk=__isoc99_vwscanf:0/1 chk=__isoc23_vwscanf:0/1"},
    {"vfwscanf", "RW, R str, other", "int",
     "wide scanf=1/2 chk=__isoc99_vfwscanf:0/1/2 chk=__isoc23_vfwscanf:0/1/2"},
    {"vswscanf", "R str, R str, other", "int",
     "wide scanf=1/2 chk=__isoc99_vswscanf:0/1/2 chk=__isoc23_vswscanf:0/1/2"},
    {"wcsftime", "W count=a1 nullable-if=a1, int, R str, R", "int", "wide"},
    {"open_wmemstream", "W bytes=8 nullable, W nullable", "ptr", "wide"},
    {"wctype", "R str", "int", ""},
    {"wctrans", "R str", "int", ""},
    {"mbrtoc8",
     "W nullable, R bytes=min(a2,strlen(a1)+1) nullable, int, RW nullable",
     "int", ""},
    {"mbrtoc16",
     "W nullable, R bytes=min(a2,strlen(a1)+1) nullable, int, RW nullable",
     "int", ""},
    {"mbrtoc32",
     "W nullable, R bytes=min(a2,strlen(a1)+1) nullable, int, RW nullable",
     "int", ""},
    {"c8rtomb", "W nullable, int, RW nullable", "int", ""},
    {"c16rtomb", "W nullable, int, RW nullable", "int", ""},
    {"c32rtomb", "W nullable, int, RW nullable", "int", ""},
});

// <math.h>, <errno.h>, <ctype.h>.
static constexpr auto MathExpectations = std::to_array<Expectation>({
    {"frexp", "other, W", "int", ""},
    {"frexpf", "other, W", "int", ""},
    {"frexpl", "other, W", "int", ""},
    {"modf", "other, W", "int", ""},
    {"modff", "other, W", "int", ""},
    {"modfl", "other, W", "int", ""},
    {"remquo", "other, other, W", "int", ""},
    {"remquof", "other, other, W", "int", ""},
    {"remquol", "other, other, W", "int", ""},
    {"nan", "R str", "int", ""},
    {"nanf", "R str", "int", ""},
    {"nanl", "R str", "int", ""},
    {"sincos", "other, W, W", "void", ""},
    {"sincosf", "other, W, W", "void", ""},
    {"sincosl", "other, W, W", "void", ""},
    {"lgamma_r", "other, W", "int", ""},
});

// Compiler builtins (Clang's Builtins.td) and the assertion-failure entry
// points of glibc, musl and macOS. Each __sync builtin also has the sized
// forms Sema resolves it to.
static constexpr auto BuiltinExpectations = std::to_array<Expectation>({
    {"__assert_fail", "R str, R str, int, R str nullable", "void", ""},
    {"__assert_perror_fail", "int, R str, int, R str nullable", "void", ""},
    {"__assert_rtn", "R str nullable, R str, int, R str", "void", ""},
    {"__assert", "R str, R str, int", "void", ""},
});

TEST(LibrarySpecTest, EveryRowMatchesItsIndependentExpectation) {
  const std::vector<std::span<const Expectation>> tables = {
      HeapExpectations,    StringExpectations, StdioExpectations,
      StdlibExpectations,  UnistdExpectations, SystemExpectations,
      UsersExpectations,   ThreadExpectations, SignalExpectations,
      NetworkExpectations, WideExpectations,   MathExpectations,
      BuiltinExpectations,
  };
  std::map<std::string, const LibraryEntry *> rows;
  std::map<std::string, int> overloads;
  for (const LibraryEntry &entry : LibrarySpec::shipped().entries()) {
    const int n = ++overloads[entry.name];
    rows[n == 1 ? entry.name : entry.name + "#" + std::to_string(n)] = &entry;
  }
  std::map<std::string, int> expected;
  for (const std::span<const Expectation> table : tables) {
    for (const Expectation &expectation : table) {
      const std::string key(expectation.name);
      EXPECT_EQ(++expected[key], 1) << "two expectations for " << key;
      const auto it = rows.find(key);
      if (it == rows.end()) {
        ADD_FAILURE() << "no row for the expectation of " << key;
        continue;
      }
      const LibraryEntry &entry = *it->second;
      EXPECT_EQ(renderParams(entry), expectation.params) << key;
      EXPECT_EQ(renderResult(entry), expectation.result) << key;
      EXPECT_EQ(renderClauses(entry), expectation.clauses) << key;
    }
  }
  for (const auto &[key, entry] : rows)
    EXPECT_TRUE(expected.contains(key)) << "row without an expectation: " << key
                                        << " (line " << entry->line << ")";
}

} // namespace weavec::core
