//===- LibrarySpecTest.cpp - The declarative C library table --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §8.3: every row of LibrarySpec.txt is checked against an
// independent expectation table, written from the C standard, POSIX and the
// platform manuals in a notation of its own (`render` below), so that a
// wrong row fails. The other tests cover terms, parser errors, aliases and
// the fortified forms, the §8.3 model fixes and the §5.2 header list.
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

static std::string joined(const std::vector<std::uint8_t> &values) {
  std::string text;
  for (const std::uint8_t value : values)
    text += (text.empty() ? "" : "/") + std::to_string(value);
  return text;
}

static std::string renderCallback(const LibCallback &callback) {
  switch (callback.kind) {
  case LibCallback::Kind::Sync:
    return "sync=" +
           (callback.arguments.empty() ? "none" : joined(callback.arguments));
  case LibCallback::Kind::Entry:
    return "entry=" +
           (callback.arguments.empty() ? "none" : joined(callback.arguments));
  case LibCallback::Kind::AtExit:
    return "atexit";
  }
  return "?";
}

static std::string renderValue(const LibraryResult &value, bool noreturn) {
  using Kind = LibraryResult::Kind;
  std::string text;
  switch (value.kind) {
  case Kind::Void:
    return noreturn ? "noreturn" : "void";
  case Kind::Int:
    return value.value ? "int value=" + value.value->str() : "int";
  case Kind::Fresh:
    text = "fresh=" + value.family;
    break;
  case Kind::Static:
    text = "static=" + value.state;
    break;
  case Kind::Arg:
    text = "arg=" + std::to_string(value.arg);
    break;
  case Kind::Interior:
    text = "interior=" + std::to_string(value.arg);
    break;
  case Kind::InteriorState:
    text = "interior-state=" + value.state;
    break;
  case Kind::Unknown:
    text = "ptr";
    break;
  }
  switch (value.null) {
  case LibraryResult::Null::Never:
    text += " nonnull";
    break;
  case LibraryResult::Null::OnFailure:
    text += " null-on-failure";
    break;
  case LibraryResult::Null::May:
    text += " nullable";
    break;
  }
  if (value.extent)
    text += " extent=" + value.extent->str();
  if (value.offset)
    text += " offset=" + value.offset->str();
  if (value.kind == Kind::Arg && !value.family.empty())
    text += " else-fresh=" + value.family;
  if (value.kind == Kind::Arg && !value.state.empty())
    text += " else-static=" + value.state;
  if (value.zeroInit)
    text += " zeroed";
  else if (value.family == "free")
    text += " unzeroed";
  if (value.zeroFilled)
    text += " zero-filled";
  if (value.string)
    text += " str";
  if (value.replaces)
    text += " replaces";
  return text;
}

static std::string renderParam(const LibraryParam &param) {
  using Access = LibraryParam::Access;
  std::string text;
  switch (param.type) {
  case LibraryParam::Type::Int:
    return "int";
  case LibraryParam::Type::Other:
    return "other";
  case LibraryParam::Type::Function:
    text = param.null == LibraryParam::Null::Allowed ? "fn?" : "fn";
    if (param.callback)
      text += " " + renderCallback(*param.callback);
    return text;
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
  switch (param.effect) {
  case LibraryParam::Effect::Borrow:
    break;
  case LibraryParam::Effect::Release:
    text += " releases=" + param.family;
    break;
  case LibraryParam::Effect::Realloc:
    text += " reallocs=" + param.family;
    break;
  case LibraryParam::Effect::Retain:
    text += " retains=" + param.state;
    break;
  case LibraryParam::Effect::Escape:
    text += " escapes";
    break;
  case LibraryParam::Effect::Init:
    text += " inits=" + param.family;
    break;
  case LibraryParam::Effect::Fini:
    text += " finis=" + param.family;
    break;
  }
  if (param.callback)
    text += " " + renderCallback(*param.callback);
  if (param.out)
    text += " out=(" + renderValue(*param.out, false) + ")";
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

static std::string renderClauses(const LibraryEntry &entry) {
  std::vector<std::string> clauses;
  clauses.reserve(entry.disjoint.size() + entry.invalidates.size() +
                  entry.reads.size() + entry.chk.size() + 3);
  for (const LibDisjoint &d : entry.disjoint)
    clauses.push_back("disjoint=" + std::to_string(d.first) + "/" +
                      std::to_string(d.second) + "/" + d.length.str());
  for (const LibCopy &copy : entry.copies)
    clauses.push_back("copies=" + std::to_string(copy.dst) + "/" +
                      std::to_string(copy.src) + "/" + copy.length.str());
  for (const LibFill &fill : entry.fills)
    clauses.push_back("fills=" + std::to_string(fill.dst) + "/" +
                      fill.value.str() + "/" + fill.length.str());
  for (const LibStringWrite &write : entry.writesString)
    clauses.push_back("writes-str=" + std::to_string(write.dst) +
                      (write.length ? "/" + write.length->str() : ""));
  for (const std::string &state : entry.invalidates)
    clauses.push_back("invalidates=" + state);
  for (const std::string &state : entry.reads)
    clauses.push_back("reads=" + state);
  if (entry.exits)
    clauses.emplace_back("exits");
  if (entry.returnsTwice)
    clauses.emplace_back("returns-twice");
  if (entry.format)
    clauses.push_back(std::string(entry.format->kind == LibFormat::Kind::Printf
                                      ? "printf="
                                      : "scanf=") +
                      std::to_string(entry.format->format) + "/" +
                      std::to_string(entry.format->first));
  for (const LibraryChk &alias : entry.chk) {
    std::string text = "chk=" + alias.name + ":";
    for (std::size_t i = 0; i < alias.argumentOf.size(); ++i)
      text += (i != 0 ? "/" : "") + std::to_string(alias.argumentOf[i]);
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

static const LibraryEntry &row(std::string_view name) {
  const LibraryEntry *entry = LibrarySpec::shipped().find(name);
  EXPECT_NE(entry, nullptr) << name;
  static const LibraryEntry Missing{.name = "<missing>"};
  return entry != nullptr ? *entry : Missing;
}

//===----------------------------------------------------------------------===//
// The shipped table and the syntax
//===----------------------------------------------------------------------===//

TEST(LibrarySpecTest, ShippedTableParses) {
  EXPECT_EQ(LibrarySpec::shippedError(), "");
  EXPECT_TRUE(LibrarySpec::shippedText().starts_with("# LibrarySpec.txt"));
  EXPECT_GE(LibrarySpec::shipped().entries().size(), 600U);
  EXPECT_GE(LibrarySpec::shipped().headers().size(), 180U);
}

TEST(LibrarySpecTest, EveryRowRoundTripsThroughItsCanonicalText) {
  for (const LibraryEntry &entry : LibrarySpec::shipped().entries()) {
    const std::string text =
        (entry.header.empty() ? "builtins;\n"
                              : "header " + entry.header + ";\n") +
        entry.str();
    std::string error;
    const auto again = LibrarySpec::parse(text, error);
    ASSERT_TRUE(again.has_value()) << text << "\n" << error;
    ASSERT_EQ(again->entries().size(), 1U);
    LibraryEntry copy = again->entries().front();
    copy.line = entry.line;
    EXPECT_EQ(copy, entry) << text;
  }
}

TEST(LibrarySpecTest, DirectivesContinuationsAndComments) {
  const LibrarySpec spec = parseOk("# a comment\n"
                                   "header bits/*;\n"
                                   "header a.h;   # trailing comment\n"
                                   "f (r:str, \\\n"
                                   "   int) -> int;\n"
                                   "builtins;\n"
                                   "__builtin_g (int) -> int;\n"
                                   "header a.h;\n"
                                   "h () -> void;\n");
  ASSERT_EQ(spec.entries().size(), 3U);
  EXPECT_EQ(spec.entries()[0].header, "a.h");
  EXPECT_EQ(spec.entries()[0].line, 4U);
  EXPECT_EQ(spec.entries()[0].params.size(), 2U);
  EXPECT_TRUE(spec.entries()[1].isCompilerBuiltin());
  EXPECT_EQ(spec.entries()[2].header, "a.h");
  EXPECT_EQ(spec.headers(), (std::vector<std::string>{"bits/*", "a.h"}));
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
  EXPECT_EQ(roundTrip("fmtlen(a1)+1"), "fmtlen(a1)+1");
  EXPECT_EQ(roundTrip("a3-1"), "a3-1");
  EXPECT_EQ(roundTrip("L_tmpnam"), "L_tmpnam");
  std::string error;
  const auto sum = LibTerm::parse("a0+a1*a2", error);
  ASSERT_TRUE(sum.has_value());
  EXPECT_EQ(sum->kind, LibTerm::Kind::Sum);
  EXPECT_EQ(sum->operands[1].kind, LibTerm::Kind::Product);
  EXPECT_EQ(sum->arguments(), (std::vector<unsigned>{0, 1, 2}));
  for (const char *bad : {"", "a", "a0+", "min(a0)", "a0 a1", "a256",
                          "strlen(x)", "fmtlen(a1", "a1-b", "x0"})
    EXPECT_FALSE(LibTerm::parse(bad, error).has_value()) << bad;

  const LibTerm::Values values{
      .argument = [](unsigned i) -> std::optional<std::int64_t> {
        return i < 3 ? std::optional<std::int64_t>(10 * (i + 1)) : std::nullopt;
      },
      .stringLength = [](unsigned) -> std::optional<std::int64_t> { return 5; },
      .formatLength = {},
      .macro = [](std::string_view name) -> std::optional<std::int64_t> {
        return name == "BUFSIZ" ? std::optional<std::int64_t>(1024)
                                : std::nullopt;
      }};
  const auto eval = [&](std::string_view text) {
    std::string why;
    return LibTerm::parse(text, why)->evaluate(values);
  };
  EXPECT_EQ(eval("a0*a1+1"), 201);
  EXPECT_EQ(eval("min(a2,strlen(a1)+1)"), 6);
  EXPECT_EQ(eval("a2-31"), -1);
  EXPECT_EQ(eval("BUFSIZ"), 1024);
  EXPECT_EQ(eval("PATH_MAX"), std::nullopt);
  EXPECT_EQ(eval("a3"), std::nullopt);
  EXPECT_EQ(eval("fmtlen(a1)"), std::nullopt);
  EXPECT_EQ(eval("9223372036854775807+a0"), std::nullopt);
  EXPECT_EQ(eval("4611686018427387904*a1"), std::nullopt);
}

TEST(LibrarySpecTest, ParserErrorsNameTheLineAndTheProblem) {
  struct Case {
    const char *text;
    const char *error;
  };
  const std::vector<Case> cases = {
      {"f () -> int;",
       "LibrarySpec.txt:1: 'f': an entry must follow a 'header' or "
       "'builtins' line"},
      {"header bits/*;\nf () -> int;",
       "LibrarySpec.txt:2: 'f': an entry must follow a concrete 'header', "
       "not a pattern"},
      {"header /usr/include/stdio.h;",
       "LibrarySpec.txt:1: invalid header name '/usr/include/stdio.h'"},
      {"header a.h;\nf (int -> int;",
       "LibrarySpec.txt:2: 'f': expected ')' after the parameters"},
      {"header a.h;\nf (int, \\\n   bogus) -> int;",
       "LibrarySpec.txt:3: 'f': unknown parameter type 'bogus'"},
      {"header a.h;\nf (r:bytez(a0)) -> int;",
       "LibrarySpec.txt:2: 'f': unknown parameter flag 'bytez'"},
      {"header a.h;\nf (r:null-ok:null-if-zero(a1), int) -> int;",
       "conflicting null flags ('null-if-zero')"},
      {"header a.h;\nf (w:bytes(a3), int) -> int;",
       "'a3' in parameter 0 names a missing parameter"},
      {"header a.h;\nf (w:bytes(fmtlen(a1)), r:str, ...) -> int;",
       "'fmtlen(a1)' needs a 'printf(1, …)' clause"},
      {"header a.h;\nf (int) -> fresh(free);",
       "a 'fresh(free)' value needs 'zero-init' or 'no-zero-init' (§11)"},
      {"header a.h;\nf (r) -> arg(0):zero-init;",
       "'zero-init' and 'no-zero-init' apply only to fresh results"},
      {"header a.h;\nf () -> int exits;", "'exits' needs a 'noreturn' result"},
      {"header a.h;\nf () -> void returns-twice;",
       "'returns-twice' needs an 'int' result"},
      {"header a.h;\nf () -> int", "expected ';' at the end of the entry"},
      {"header a.h;\nf () -> int; g () -> int;",
       "unexpected text after ';' (one entry per line)"},
      {"header a.h;\nf (r, int) -> int chk(__f_chk: 0,0,-1);",
       "alias '__f_chk' must map argument 0 exactly once"},
      {"header a.h;\nf (r, int) -> int chk(__f_chk: 0,-1);",
       "alias '__f_chk' must map argument 1 exactly once"},
      {"header a.h;\nf (r, int) -> int chk(__f_chk: 0,5);",
       "alias '__f_chk' maps to missing argument 5"},
      {"header a.h;\nf (int) -> int chk(__f: -2);", "only -1 may be negative"},
      {"header a.h;\nf (int) -> int;\nf (int) -> int;",
       "LibrarySpec.txt:3: 'f': defined again with the same signature (first "
       "on line 2)"},
      {"header a.h;\nf (int) -> int chk(g: 0);\ng (int) -> int;",
       "LibrarySpec.txt:2: 'f': alias 'g' is also an entry name"},
      {"header a.h;\nf (r, ...) -> int printf(0,1);",
       "the format argument must be an 'r:str' parameter"},
      {"header a.h;\nf (r:str, ...) -> int printf(0,2);",
       "the first variadic argument is 1, not 2"},
      {"header a.h;\nf (r:str, int) -> int printf(0,1);",
       "a 'v…' function's va_list argument must be 'other'"},
      {"header a.h;\nf (w:str) -> int;",
       "parameter 0: 'str' needs read access ('r' or 'rw')"},
      {"header a.h;\nf (int) -> interior(0);",
       "the result names argument 0, which is not a pointer"},
      {"header a.h;\nf (w) -> arg(0):or-fresh(free):zero-init;",
       "'or-fresh'/'or-static' need argument 0 to be 'null-ok'"},
      {"header a.h;\nf (fn:entry(0,1)) -> int;",
       "'entry(…)' takes at most one argument"},
      {"header a.h;\nf (fn:bytes(1)) -> int;",
       "'bytes' is not a flag of 'fn' parameters"},
      {"header a.h;\nf (int:null-ok) -> int;",
       "'int' parameters take no flags"},
      {"header a.h;\nf (w:out(ptr:replaces)) -> int;",
       "'replaces' applies only to a fresh 'out' value"},
      {"header a.h;\nf (w:out(int)) -> int;",
       "an 'out' value must be a pointer, not 'int'"},
      {"header a.h;\nf (w, r) -> int disjoint(0,0,1);",
       "'disjoint' needs two different pointer parameters"},
      {"header a.h;\nf () -> int frobnicate;", "unknown clause 'frobnicate'"},
      {"header a.h;\nf (r:release(free):escape) -> int;",
       "conflicting effect flags ('escape')"},
      {"header a.h;\nf (r:str:str) -> int;", "duplicate flag 'str'"},
      {"header a.h;\nf (int) -> static(s):extent(a0):nonnull:null-ok;",
       "conflicting null flags ('null-ok')"},
      {"header a.h;\nf (int) -> arg(0);",
       "the result names argument 0, which is not a pointer"},
      {"header a.h;\nf (r, w) -> int copies(0,1,a2);",
       "'copies' needs a written and a different read pointer parameter"},
      {"header a.h;\nf (w) -> int fills(0,0);", "expected ',' in 'fills(…)'"},
      {"header a.h;\nf (r:str) -> int writes-str(0);",
       "'writes-str' needs a written pointer parameter"},
      {"header a.h;\nf (w, int) -> int writes-str(0,a3);",
       "'a3' in 'writes-str' names a missing parameter"},
      {"header a.h;\nf (r) -> int:extent(1);",
       "the only flag of an 'int' result is 'value(…)'"},
      {"header a.h;\nf (r) -> int:value(a3);",
       "'a3' in the result names a missing parameter"},
      {"header a.h;\nf (r) -> ptr:offset(1);",
       "'offset' applies only to 'interior(N)' results"},
      {"header a.h;\nf (int) -> fresh(x):zero-filled;",
       "'zero-filled' applies only to fresh results with an extent"},
  };
  for (const Case &c : cases)
    EXPECT_NE(parseError(c.text).find(c.error), std::string::npos)
        << c.text << "\n  gave: " << parseError(c.text);
}

//===----------------------------------------------------------------------===//
// Names, aliases and fortified forms (§8, "Which calls a row governs")
//===----------------------------------------------------------------------===//

/// The row arguments that call arguments 0..count-1 become.
static std::vector<int> remap(std::string_view callee, unsigned count) {
  const auto match = LibrarySpec::shipped().lookup(callee);
  EXPECT_TRUE(match.has_value()) << callee;
  std::vector<int> rows;
  for (unsigned i = 0; match && i < count; ++i)
    rows.push_back(match->rowArgument(i));
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
  EXPECT_EQ(resolvedName("__builtin_alloca"), "alloca");
  EXPECT_EQ(resolvedName("__builtin_fabs"), "fabs");
  EXPECT_EQ(resolvedName("__builtin_setjmp"), "setjmp");
  EXPECT_EQ(resolvedName("__builtin_expect"), "__builtin_expect");
  EXPECT_EQ(resolvedName("json_decref"), "<none>");
  EXPECT_EQ(resolvedName("__builtin_json_decref"), "<none>");
  EXPECT_EQ(spec.find("__builtin_memcpy"), nullptr);
  const auto plain = spec.lookup("__builtin_memcpy");
  ASSERT_TRUE(plain.has_value());
  EXPECT_EQ(plain->alias, nullptr);
  EXPECT_EQ(plain->rowArgument(2), 2);
  EXPECT_EQ(plain->callArgument(1), 1);
}

TEST(LibrarySpecTest, MacOSFortifiedSpellingsRemapArguments) {
  // secure/_string.h: __builtin___X_chk(dst, ..., __darwin_obsz(dst)).
  EXPECT_EQ(remap("__builtin___memcpy_chk", 4),
            (std::vector<int>{0, 1, 2, -1}));
  EXPECT_EQ(remap("__builtin___memmove_chk", 4),
            (std::vector<int>{0, 1, 2, -1}));
  EXPECT_EQ(remap("__builtin___memset_chk", 4),
            (std::vector<int>{0, 1, 2, -1}));
  EXPECT_EQ(remap("__builtin___strcpy_chk", 3), (std::vector<int>{0, 1, -1}));
  EXPECT_EQ(remap("__builtin___stpcpy_chk", 3), (std::vector<int>{0, 1, -1}));
  EXPECT_EQ(remap("__builtin___strcat_chk", 3), (std::vector<int>{0, 1, -1}));
  EXPECT_EQ(remap("__builtin___strncpy_chk", 4),
            (std::vector<int>{0, 1, 2, -1}));
  EXPECT_EQ(remap("__builtin___stpncpy_chk", 4),
            (std::vector<int>{0, 1, 2, -1}));
  EXPECT_EQ(remap("__builtin___strncat_chk", 4),
            (std::vector<int>{0, 1, 2, -1}));
  EXPECT_EQ(remap("__builtin___strlcpy_chk", 4),
            (std::vector<int>{0, 1, 2, -1}));
  EXPECT_EQ(remap("__builtin___strlcat_chk", 4),
            (std::vector<int>{0, 1, 2, -1}));
  EXPECT_EQ(remap("__builtin___memccpy_chk", 5),
            (std::vector<int>{0, 1, 2, 3, -1}));
  // secure/_stdio.h: the flag and the object size precede the format.
  EXPECT_EQ(remap("__builtin___sprintf_chk", 6),
            (std::vector<int>{0, -1, -1, 1, 2, 3}));
  EXPECT_EQ(remap("__builtin___snprintf_chk", 7),
            (std::vector<int>{0, 1, -1, -1, 2, 3, 4}));
  EXPECT_EQ(remap("__builtin___vsprintf_chk", 5),
            (std::vector<int>{0, -1, -1, 1, 2}));
  EXPECT_EQ(remap("__builtin___vsnprintf_chk", 6),
            (std::vector<int>{0, 1, -1, -1, 2, 3}));
  EXPECT_EQ(resolvedName("__builtin___sprintf_chk"), "sprintf");
  EXPECT_EQ(resolvedName("__builtin___vsnprintf_chk"), "vsnprintf");
  // macOS declares the functions the builtins lower to.
  EXPECT_EQ(remap("__snprintf_chk", 6), (std::vector<int>{0, 1, -1, -1, 2, 3}));
  EXPECT_EQ(remap("__sprintf_chk", 5), (std::vector<int>{0, -1, -1, 1, 2}));
  const auto sprintfChk =
      LibrarySpec::shipped().lookup("__builtin___sprintf_chk");
  ASSERT_TRUE(sprintfChk.has_value());
  EXPECT_EQ(sprintfChk->callArgument(1), 3);
  EXPECT_EQ(sprintfChk->callArgument(2), 4);
  ASSERT_NE(sprintfChk->param(3), nullptr);
  EXPECT_TRUE(sprintfChk->param(3)->string);
  EXPECT_EQ(sprintfChk->param(1), nullptr);
  EXPECT_EQ(sprintfChk->param(4), nullptr);
}

TEST(LibrarySpecTest, GlibcFortifiedSpellingsRemapArguments) {
  // bits/string_fortified.h and the functions the builtins call.
  EXPECT_EQ(remap("__memcpy_chk", 4), (std::vector<int>{0, 1, 2, -1}));
  EXPECT_EQ(remap("__mempcpy_chk", 4), (std::vector<int>{0, 1, 2, -1}));
  EXPECT_EQ(remap("__strcpy_chk", 3), (std::vector<int>{0, 1, -1}));
  EXPECT_EQ(remap("__explicit_bzero_chk", 3), (std::vector<int>{0, 1, -1}));
  // bits/stdio2.h: the flag precedes the format.
  EXPECT_EQ(remap("__printf_chk", 3), (std::vector<int>{-1, 0, 1}));
  EXPECT_EQ(remap("__fprintf_chk", 4), (std::vector<int>{0, -1, 1, 2}));
  EXPECT_EQ(remap("__vfprintf_chk", 4), (std::vector<int>{0, -1, 1, 2}));
  EXPECT_EQ(remap("__asprintf_chk", 4), (std::vector<int>{0, -1, 1, 2}));
  EXPECT_EQ(remap("__syslog_chk", 4), (std::vector<int>{0, -1, 1, 2}));
  EXPECT_EQ(remap("__fgets_chk", 4), (std::vector<int>{0, -1, 1, 2}));
  EXPECT_EQ(remap("__fread_chk", 5), (std::vector<int>{0, -1, 1, 2, 3}));
  // bits/unistd.h, bits/socket2.h, bits/wchar2.h.
  EXPECT_EQ(remap("__read_chk", 4), (std::vector<int>{0, 1, 2, -1}));
  EXPECT_EQ(remap("__pread64_chk", 5), (std::vector<int>{0, 1, 2, 3, -1}));
  EXPECT_EQ(remap("__readlink_chk", 4), (std::vector<int>{0, 1, 2, -1}));
  EXPECT_EQ(remap("__getcwd_chk", 3), (std::vector<int>{0, 1, -1}));
  EXPECT_EQ(remap("__recv_chk", 5), (std::vector<int>{0, 1, 2, -1, 3}));
  EXPECT_EQ(remap("__wmemcpy_chk", 4), (std::vector<int>{0, 1, 2, -1}));
  EXPECT_EQ(remap("__open_2", 2), (std::vector<int>{0, 1}));
  // Renames rather than fortified forms.
  EXPECT_EQ(resolvedName("__isoc99_sscanf"), "sscanf");
  EXPECT_EQ(resolvedName("__isoc23_strtol"), "strtol");
  EXPECT_EQ(resolvedName("__xpg_basename"), "basename");
  EXPECT_EQ(resolvedName("__sigsetjmp"), "sigsetjmp");
  EXPECT_EQ(resolvedName("__longjmp_chk"), "longjmp");
  EXPECT_EQ(resolvedName("__sync_fetch_and_add_4"), "__sync_fetch_and_add");
  EXPECT_EQ(remap("__isoc99_fscanf", 4), (std::vector<int>{0, 1, 2, 3}));
}

TEST(LibrarySpecTest, OverloadsFollowTheDeclaration) {
  using Type = LibraryParam::Type;
  const LibrarySpec &spec = LibrarySpec::shipped();
  EXPECT_EQ(spec.overloads("qsort_r").size(), 2U);
  const LibSignature glibc{.params = {Type::Pointer, Type::Int, Type::Int,
                                      Type::Function, Type::Pointer}};
  const LibSignature bsd{.params = {Type::Pointer, Type::Int, Type::Int,
                                    Type::Pointer, Type::Function}};
  const auto gnu = spec.lookup("qsort_r", glibc);
  ASSERT_TRUE(gnu.has_value());
  ASSERT_TRUE(gnu->entry->params[3].callback.has_value());
  EXPECT_EQ(gnu->entry->params[3].callback->arguments,
            (std::vector<std::uint8_t>{0, 0, 4}));
  const auto apple = spec.lookup("qsort_r", bsd);
  ASSERT_TRUE(apple.has_value());
  ASSERT_TRUE(apple->entry->params[4].callback.has_value());
  EXPECT_EQ(apple->entry->params[4].callback->arguments,
            (std::vector<std::uint8_t>{3, 0, 0}));
  const LibSignature xsi{.params = {Type::Int, Type::Pointer, Type::Int}};
  LibSignature gnuMessage = xsi;
  gnuMessage.pointerResult = true;
  EXPECT_EQ(spec.lookup("strerror_r", xsi)->entry->result.kind,
            LibraryResult::Kind::Int);
  EXPECT_EQ(spec.lookup("strerror_r", gnuMessage)->entry->result.kind,
            LibraryResult::Kind::Unknown);
  // A declaration that does not fit the row does not use it.
  const LibSignature twoArgs{.params = {Type::Pointer, Type::Pointer},
                             .pointerResult = true};
  EXPECT_FALSE(spec.lookup("memcpy", twoArgs).has_value());
  // `other` accepts a pointer: va_list is one on some targets.
  const LibSignature pointerList{.params = {Type::Pointer, Type::Pointer}};
  EXPECT_TRUE(spec.lookup("vprintf", pointerList).has_value());
}

//===----------------------------------------------------------------------===//
// The model fixes of §8.3
//===----------------------------------------------------------------------===//

TEST(LibrarySpecTest, ModelFixesOfSection83) {
  using Null = LibraryParam::Null;
  using Effect = LibraryParam::Effect;
  using Kind = LibraryResult::Kind;
  // system(NULL) is allowed.
  EXPECT_EQ(row("system").params[0].null, Null::Allowed);
  // Zero-length calls accept null, with the length as the zero term.
  const auto zeroLength = [](std::string_view name, unsigned param,
                             std::string_view length) {
    const LibraryParam &p = row(name).params.at(param);
    EXPECT_EQ(p.null, Null::AllowedIfZero) << name << " " << param;
    EXPECT_EQ(p.zeroTerm ? p.zeroTerm->str() : "", length) << name;
  };
  zeroLength("read", 1, "a2");
  zeroLength("write", 1, "a2");
  zeroLength("memcpy", 0, "a2");
  zeroLength("memcpy", 1, "a2");
  zeroLength("memmove", 0, "a2");
  zeroLength("memmove", 1, "a2");
  zeroLength("memset", 0, "a2");
  zeroLength("memcmp", 0, "a2");
  zeroLength("memcmp", 1, "a2");
  zeroLength("snprintf", 0, "a1");
  zeroLength("strncpy", 0, "a2");
  zeroLength("strncpy", 1, "a2");
  // Static storage with the right lifetime.
  EXPECT_EQ(row("getenv").result.kind, Kind::Static);
  EXPECT_EQ(row("getenv").result.state, "environ");
  EXPECT_EQ(row("localtime").result.kind, Kind::Static);
  EXPECT_EQ(row("strerror").result.kind, Kind::Static);
  EXPECT_EQ(row("getpwnam").result.kind, Kind::Static);
  EXPECT_EQ(row("strtok").result.kind, Kind::InteriorState);
  EXPECT_EQ(row("strtok").params[0].effect, Effect::Retain);
  EXPECT_EQ(row("strtok").reads, (std::vector<std::string>{"strtok"}));
  EXPECT_EQ(row("setenv").invalidates, (std::vector<std::string>{"environ"}));
  EXPECT_EQ(row("putenv").params[0].effect, Effect::Retain);
  // longjmp does not return, and does not end the process.
  EXPECT_TRUE(row("longjmp").noreturn);
  EXPECT_FALSE(row("longjmp").exits);
  EXPECT_TRUE(row("siglongjmp").noreturn);
  EXPECT_TRUE(row("exit").exits);
  EXPECT_TRUE(row("abort").exits);
  // realloc(p, 0) may release p on the null class: the size is the extent.
  EXPECT_EQ(row("realloc").params[0].effect, Effect::Realloc);
  EXPECT_EQ(row("realloc").result.extent->str(), "a1");
  // The releasers, and what they release.
  EXPECT_EQ(row("freeifaddrs").params[0].effect, Effect::Release);
  EXPECT_EQ(row("freeaddrinfo").params[0].effect, Effect::Release);
  EXPECT_EQ(row("getifaddrs").params[0].out->family, "freeifaddrs");
  EXPECT_EQ(row("getaddrinfo").params[3].out->family, "freeaddrinfo");
  EXPECT_EQ(row("globfree").params[0].effect, Effect::Fini);
  EXPECT_EQ(row("glob").params[3].effect, Effect::Init);
  EXPECT_EQ(row("regfree").params[0].effect, Effect::Fini);
  EXPECT_EQ(row("regcomp").params[0].effect, Effect::Init);
}

//===----------------------------------------------------------------------===//
// The header list (§5.2) and the accessors
//===----------------------------------------------------------------------===//

TEST(LibrarySpecTest, HeaderListNamesThePlatformHeadersOnly) {
  const LibrarySpec &spec = LibrarySpec::shipped();
  for (const char *header :
       {"stdio.h", "stdlib.h", "string.h", "pthread.h", "sys/ioctl.h",
        "sys/socket.h", "arpa/inet.h", "netdb.h", "dirent.h", "regex.h",
        "bits/stdio2.h", "bits/types/struct_FILE.h", "linux/if.h",
        "netinet/ip6.h", "./stdio.h", "sys\\socket.h", "execinfo.h"})
    EXPECT_TRUE(spec.isPlatformHeader(header, false)) << header;
  for (const char *header :
       {"jansson.h", "sqlite3.h", "zlib.h", "curl/curl.h", "openssl/ssl.h",
        "readline/readline.h", "ncurses.h", "sys/capability.h", "bits",
        "stdio.hpp", "my/stdio.h", ""})
    EXPECT_FALSE(spec.isPlatformHeader(header, false)) << header;
  // On Darwin every header of the SDK counts.
  EXPECT_TRUE(spec.isPlatformHeader("jansson.h", true));
  // Every entry's header is in the list.
  for (const LibraryEntry &entry : spec.entries())
    if (!entry.isCompilerBuiltin())
      EXPECT_TRUE(spec.isPlatformHeader(entry.header, false)) << entry.name;
}

TEST(LibrarySpecTest, AccessorsDescribeTheRow) {
  EXPECT_TRUE(row("malloc").allocates());
  EXPECT_FALSE(row("malloc").releases());
  EXPECT_TRUE(row("free").releases());
  EXPECT_TRUE(row("realloc").releases());
  EXPECT_TRUE(row("getline").allocates());
  EXPECT_TRUE(row("realpath").allocates());
  EXPECT_FALSE(row("strlen").allocates());
  EXPECT_TRUE(row("qsort").hasCallback());
  EXPECT_TRUE(row("sigaction").hasCallback());
  EXPECT_FALSE(row("memcpy").hasCallback());
  EXPECT_FALSE(row("exit").knownToReturn());
  EXPECT_FALSE(row("longjmp").knownToReturn());
  EXPECT_TRUE(row("strlen").knownToReturn());
  EXPECT_TRUE(row("getenv").trustsLibrarySpec());
  EXPECT_TRUE(row("strtok").trustsLibrarySpec());
  EXPECT_TRUE(row("tmpnam").trustsLibrarySpec());
  EXPECT_TRUE(row("pthread_create").trustsLibrarySpec());
  EXPECT_FALSE(row("memcpy").trustsLibrarySpec());
  EXPECT_TRUE(row("__builtin_object_size").isCompilerBuiltin());
  EXPECT_FALSE(row("memcpy").isCompilerBuiltin());
  EXPECT_EQ(row("memcpy").param(3), nullptr);
  EXPECT_EQ(row("printf").param(1), nullptr);
  const LibSignature allocator = row("malloc").signature();
  EXPECT_EQ(allocator.params,
            (std::vector<LibraryParam::Type>{LibraryParam::Type::Int}));
  EXPECT_TRUE(allocator.pointerResult);
  EXPECT_TRUE(row("malloc").accepts(allocator));
  EXPECT_EQ(row("memcpy").header, "string.h");
  EXPECT_GT(row("memcpy").line, 0U);
}

//===----------------------------------------------------------------------===//
// The independent expectation table
//===----------------------------------------------------------------------===//
//
// One expectation per row, written from the C standard, POSIX and the glibc,
// BSD and macOS manuals, in the notation of `renderParam`, `renderValue` and
// `renderClauses`: parameters are `int`, `other`, `fn[?]` or an access (`none`,
// `R`, `W`, `RW`) with `str`, `bytes=`, `count=`, `nullable[-if=]`, an effect
// (`releases=`, `reallocs=`, `retains=`, `escapes`, `inits=`, `finis=`), a
// callback (`sync=`, `entry=`, `atexit`) and `out=(…)`; results name their
// kind and always their nullability.

// Heap allocation: every heap result may fail, is released by free, and
// carries its §11 zero-initialisation decision.
static constexpr auto HeapExpectations = std::to_array<Expectation>({
    {"malloc", "int", "fresh=free null-on-failure extent=a0 zeroed", ""},
    {"calloc", "int, int",
     "fresh=free null-on-failure extent=a0*a1 zeroed zero-filled", ""},
    {"realloc", "RW nullable reallocs=free, int",
     "fresh=free null-on-failure extent=a1 zeroed", ""},
    {"reallocarray", "RW nullable reallocs=free, int, int",
     "fresh=free null-on-failure extent=a1*a2 zeroed", ""},
    {"reallocf", "RW nullable releases=free, int",
     "fresh=free null-on-failure extent=a1 unzeroed", ""},
    {"aligned_alloc", "int, int", "fresh=free null-on-failure extent=a1 zeroed",
     ""},
    {"posix_memalign",
     "W out=(fresh=free null-on-failure extent=a2 zeroed), int, int", "int",
     ""},
    {"valloc", "int", "fresh=free null-on-failure extent=a0 zeroed", ""},
    {"free", "none nullable releases=free", "void", ""},
    {"free_sized", "none nullable releases=free, int", "void", ""},
    {"free_aligned_sized", "none nullable releases=free, int, int", "void", ""},
    {"canonicalize_file_name", "R str", "fresh=free null-on-failure zeroed str",
     ""},
    {"realpath", "R str, W bytes=PATH_MAX nullable",
     "arg=1 null-on-failure else-fresh=free zeroed str",
     "chk=__realpath_chk:0/1/-1"},
    {"memalign", "int, int", "fresh=free null-on-failure extent=a1 zeroed", ""},
    {"pvalloc", "int", "fresh=free null-on-failure extent=a0 zeroed", ""},
    {"malloc_usable_size", "none nullable", "int", ""},
    {"malloc_size", "none nullable", "int", ""},
    {"malloc_good_size", "int", "int", ""},
    {"alloca", "int", "fresh=stack nonnull extent=a0 zeroed", ""},
    {"strdup", "R str",
     "fresh=free null-on-failure extent=strlen(a0)+1 zeroed str", ""},
    {"strndup", "R bytes=min(a1,strlen(a0)+1), int",
     "fresh=free null-on-failure extent=min(a1,strlen(a0))+1 zeroed str", ""},
    {"wcsdup", "R str", "fresh=free null-on-failure zeroed str", ""},
    {"asprintf",
     "W out=(fresh=free null-on-failure extent=fmtlen(a1)+1 zeroed str), "
     "R str, ...",
     "int", "printf=1/2 chk=__asprintf_chk:0/-1/1/2"},
    {"vasprintf",
     "W out=(fresh=free null-on-failure extent=fmtlen(a1)+1 zeroed str), "
     "R str, other",
     "int", "printf=1/2 chk=__vasprintf_chk:0/-1/1/2"},
    {"getline", "RW out=(fresh=free nullable zeroed str replaces), RW, RW",
     "int", ""},
    {"getdelim",
     "RW out=(fresh=free nullable zeroed str replaces), RW, int, RW", "int",
     ""},
    {"tempnam", "R str nullable, R str nullable",
     "fresh=free null-on-failure zeroed str", "reads=environ"},
    {"getcwd", "W bytes=a1 nullable, int",
     "arg=0 null-on-failure else-fresh=free zeroed str",
     "chk=__getcwd_chk:0/1/-1"},
    {"get_current_dir_name", "", "fresh=free null-on-failure zeroed str",
     "reads=environ"},
    {"backtrace", "W count=a1, int", "int", ""},
    {"backtrace_symbols", "R count=a1, int",
     "fresh=free null-on-failure unzeroed", ""},
    {"backtrace_symbols_fd", "R count=a1, int, int", "void", ""},
});

// <string.h>, <strings.h>: C17 7.24, POSIX, glibc and BSD extensions.
static constexpr auto StringExpectations = std::to_array<Expectation>({
    {"memcpy", "W bytes=a2 nullable-if=a2, R bytes=a2 nullable-if=a2, int",
     "arg=0 nonnull",
     "disjoint=0/1/a2 copies=0/1/a2 chk=__builtin___memcpy_chk:0/1/2/-1 "
     "chk=__memcpy_chk:0/1/2/-1"},
    {"memmove", "W bytes=a2 nullable-if=a2, R bytes=a2 nullable-if=a2, int",
     "arg=0 nonnull",
     "copies=0/1/a2 chk=__builtin___memmove_chk:0/1/2/-1 "
     "chk=__memmove_chk:0/1/2/-1"},
    {"memset", "W bytes=a2 nullable-if=a2, int, int", "arg=0 nonnull",
     "fills=0/a1/a2 chk=__builtin___memset_chk:0/1/2/-1 "
     "chk=__memset_chk:0/1/2/-1"},
    {"memset_explicit", "W bytes=a2 nullable-if=a2, int, int", "arg=0 nonnull",
     "fills=0/a1/a2"},
    {"memcmp", "R bytes=a2 nullable-if=a2, R bytes=a2 nullable-if=a2, int",
     "int", ""},
    {"memchr", "R bytes=a2, int, int", "interior=0 nullable", ""},
    {"memrchr", "R bytes=a2, int, int", "interior=0 nullable", ""},
    {"rawmemchr", "R bytes=__WEAVEC_UNBOUNDED, int", "interior=0 nonnull", ""},
    {"memmem", "R bytes=a1, int, R bytes=a3, int", "interior=0 nullable", ""},
    {"mempcpy", "W bytes=a2, R bytes=a2, int", "interior=0 nonnull offset=a2",
     "disjoint=0/1/a2 copies=0/1/a2 chk=__builtin___mempcpy_chk:0/1/2/-1 "
     "chk=__mempcpy_chk:0/1/2/-1"},
    {"memccpy",
     "W bytes=min(a3,strlen(a1)+1), R bytes=min(a3,strlen(a1)+1), "
     "int, int",
     "interior=0 nullable",
     "disjoint=0/1/min(a3,strlen(a1)+1) "
     "chk=__builtin___memccpy_chk:0/1/2/3/-1"},
    {"strcpy", "W bytes=strlen(a1)+1, R str", "arg=0 nonnull",
     "disjoint=0/1/strlen(a1)+1 writes-str=0/strlen(a1) "
     "chk=__builtin___strcpy_chk:0/1/-1 chk=__strcpy_chk:0/1/-1"},
    {"strncpy",
     "W bytes=a2 nullable-if=a2, R bytes=min(a2,strlen(a1)+1) "
     "nullable-if=a2, int",
     "arg=0 nonnull",
     "disjoint=0/1/min(a2,strlen(a1)+1) copies=0/1/min(a2,strlen(a1)+1) "
     "chk=__builtin___strncpy_chk:0/1/2/-1 chk=__strncpy_chk:0/1/2/-1"},
    {"stpcpy", "W bytes=strlen(a1)+1, R str",
     "interior=0 nonnull offset=strlen(a1)",
     "disjoint=0/1/strlen(a1)+1 writes-str=0/strlen(a1) "
     "chk=__builtin___stpcpy_chk:0/1/-1 chk=__stpcpy_chk:0/1/-1"},
    {"stpncpy", "W bytes=a2, R bytes=min(a2,strlen(a1)+1), int",
     "interior=0 nonnull offset=min(a2,strlen(a1))",
     "disjoint=0/1/min(a2,strlen(a1)+1) copies=0/1/min(a2,strlen(a1)+1) "
     "chk=__builtin___stpncpy_chk:0/1/2/-1 chk=__stpncpy_chk:0/1/2/-1"},
    {"strcat", "RW str bytes=strlen(a0)+strlen(a1)+1, R str", "arg=0 nonnull",
     "disjoint=0/1/strlen(a0)+strlen(a1)+1 "
     "writes-str=0/strlen(a0)+strlen(a1) "
     "chk=__builtin___strcat_chk:0/1/-1 chk=__strcat_chk:0/1/-1"},
    {"strncat",
     "RW str bytes=strlen(a0)+min(a2,strlen(a1))+1, "
     "R bytes=min(a2,strlen(a1)+1), int",
     "arg=0 nonnull",
     "disjoint=0/1/strlen(a0)+min(a2,strlen(a1))+1 "
     "writes-str=0/strlen(a0)+min(a2,strlen(a1)) "
     "chk=__builtin___strncat_chk:0/1/2/-1 chk=__strncat_chk:0/1/2/-1"},
    {"strlcpy", "W bytes=a2 nullable-if=a2, R str, int", "int value=strlen(a1)",
     "disjoint=0/1/min(a2,strlen(a1)+1) writes-str=0/min(a2-1,strlen(a1)) "
     "chk=__builtin___strlcpy_chk:0/1/2/-1 chk=__strlcpy_chk:0/1/2/-1"},
    {"strlcat", "RW bytes=a2 nullable-if=a2, R str, int", "int",
     "chk=__builtin___strlcat_chk:0/1/2/-1 chk=__strlcat_chk:0/1/2/-1"},
    {"strlen", "R str", "int value=strlen(a0)", ""},
    {"strnlen", "R bytes=min(a1,strlen(a0)+1), int",
     "int value=min(a1,strlen(a0))", ""},
    {"strcmp", "R str, R str", "int", ""},
    {"strncmp",
     "R bytes=min(a2,strlen(a0)+1), R bytes=min(a2,strlen(a1)+1), int", "int",
     ""},
    {"strcoll", "R str, R str", "int", ""},
    {"strcoll_l", "R str, R str, none", "int", ""},
    {"strxfrm", "W bytes=a2 nullable-if=a2, R str, int", "int", ""},
    {"strxfrm_l", "W bytes=a2 nullable-if=a2, R str, int, none", "int", ""},
    {"strverscmp", "R str, R str", "int", ""},
    {"strchr", "R str, int", "interior=0 nullable", ""},
    {"strrchr", "R str, int", "interior=0 nullable", ""},
    {"strchrnul", "R str, int", "interior=0 nonnull", ""},
    {"strstr", "R str, R str", "interior=0 nullable", ""},
    {"strcasestr", "R str, R str", "interior=0 nullable", ""},
    {"strpbrk", "R str, R str", "interior=0 nullable", ""},
    {"strspn", "R str, R str", "int", ""},
    {"strcspn", "R str, R str", "int", ""},
    {"strtok", "RW str nullable retains=strtok, R str",
     "interior-state=strtok nullable", "reads=strtok"},
    {"strtok_r", "RW str nullable, R str, RW out=(interior=0 nullable)",
     "interior=0 nullable", ""},
    {"strsep", "RW, R str", "ptr nullable str", ""},
    {"strerror", "int", "static=strerror nonnull str", ""},
    {"strerror_l", "int, none", "static=strerror nonnull str", ""},
    {"strerror_r", "int, W bytes=a2, int", "ptr nonnull str", ""},
    {"strerror_r#2", "int, W bytes=a2, int", "int", ""},
    {"strsignal", "int", "static=strsignal nullable str", ""},
    {"explicit_bzero", "W bytes=a1, int", "void",
     "fills=0/0/a1 chk=__explicit_bzero_chk:0/1/-1"},
    {"memset_s", "W bytes=a1, int, int, int", "int", "fills=0/a2/a3"},
    {"bzero", "W bytes=a1, int", "void", "fills=0/0/a1"},
    {"bcopy", "R bytes=a2, W bytes=a2, int", "void", "copies=1/0/a2"},
    {"bcmp", "R bytes=a2, R bytes=a2, int", "int", ""},
    {"index", "R str, int", "interior=0 nullable", ""},
    {"rindex", "R str, int", "interior=0 nullable", ""},
    {"strcasecmp", "R str, R str", "int", ""},
    {"strncasecmp",
     "R bytes=min(a2,strlen(a0)+1), R bytes=min(a2,strlen(a1)+1), int", "int",
     ""},
    {"strcasecmp_l", "R str, R str, none", "int", ""},
    {"strncasecmp_l",
     "R bytes=min(a2,strlen(a0)+1), R bytes=min(a2,strlen(a1)+1), int, none",
     "int", ""},
    {"ffs", "int", "int", ""},
    {"ffsl", "int", "int", ""},
    {"ffsll", "int", "int", ""},
});

// <stdio.h>: C17 7.21, POSIX and glibc/BSD extensions. Streams are read and
// written by every operation except the pure queries.
static constexpr auto StdioExpectations = std::to_array<Expectation>({
    {"fopen", "R str, R str", "fresh=fclose null-on-failure", ""},
    {"fdopen", "int, R str", "fresh=fclose null-on-failure", ""},
    {"freopen", "R str nullable, R str, RW", "arg=2 null-on-failure", ""},
    {"tmpfile", "", "fresh=fclose null-on-failure", ""},
    {"fmemopen", "W bytes=a1 nullable escapes, int, R str",
     "fresh=fclose null-on-failure", ""},
    {"open_memstream",
     "W escapes out=(fresh=free null-on-failure unzeroed), W escapes",
     "fresh=fclose null-on-failure", ""},
    {"fclose", "RW releases=fclose", "int", ""},
    {"popen", "R str, R str", "fresh=pclose null-on-failure", "reads=environ"},
    {"pclose", "RW releases=pclose", "int", ""},
    {"fflush", "RW nullable", "int", ""},
    {"fpurge", "RW", "int", ""},
    {"setvbuf", "RW, W bytes=a3 nullable escapes, int, int", "int", ""},
    {"setbuf", "RW, W bytes=BUFSIZ nullable escapes", "void", ""},
    {"setbuffer", "RW, W bytes=a2 nullable escapes, int", "void", ""},
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
    {"getchar", "", "int", ""},
    {"getchar_unlocked", "", "int", ""},
    {"ungetc", "int, RW", "int", ""},
    {"fputc", "int, RW", "int", ""},
    {"putc", "int, RW", "int", ""},
    {"putc_unlocked", "int, RW", "int", ""},
    {"putchar", "int", "int", ""},
    {"putchar_unlocked", "int", "int", ""},
    {"fgets", "W bytes=a1, int, RW", "arg=0 nullable",
     "writes-str=0 chk=__fgets_chk:0/-1/1/2"},
    {"gets", "W bytes=__WEAVEC_UNBOUNDED", "arg=0 nullable",
     "chk=__gets_chk:0/-1"},
    {"fgetln", "RW, W", "interior=0 nullable", ""},
    {"fread", "W bytes=a1*a2, int, int, RW", "int",
     "chk=__fread_chk:0/-1/1/2/3"},
    {"fread_unlocked", "W bytes=a1*a2, int, int, RW", "int",
     "chk=__fread_unlocked_chk:0/-1/1/2/3"},
    {"fwrite", "R bytes=a1*a2, int, int, RW", "int", ""},
    {"fwrite_unlocked", "R bytes=a1*a2, int, int, RW", "int", ""},
    {"puts", "R str", "int", ""},
    {"fputs", "R str, RW", "int", ""},
    {"perror", "R str nullable", "void", ""},
    {"printf", "R str, ...", "int",
     "printf=0/1 chk=__builtin___printf_chk:-1/0/1 chk=__printf_chk:-1/0/1"},
    {"fprintf", "RW, R str, ...", "int",
     "printf=1/2 chk=__builtin___fprintf_chk:0/-1/1/2 "
     "chk=__fprintf_chk:0/-1/1/2"},
    {"dprintf", "int, R str, ...", "int",
     "printf=1/2 chk=__dprintf_chk:0/-1/1/2"},
    {"sprintf", "W bytes=fmtlen(a1)+1, R str, ...", "int value=fmtlen(a1)",
     "writes-str=0/fmtlen(a1) printf=1/2 "
     "chk=__builtin___sprintf_chk:0/-1/-1/1/2 "
     "chk=__sprintf_chk:0/-1/-1/1/2"},
    {"snprintf", "W bytes=a1 nullable-if=a1, int, R str, ...",
     "int value=fmtlen(a2)",
     "writes-str=0/min(a1-1,fmtlen(a2)) printf=2/3 "
     "chk=__builtin___snprintf_chk:0/1/-1/-1/2/3 "
     "chk=__snprintf_chk:0/1/-1/-1/2/3"},
    {"vprintf", "R str, other", "int",
     "printf=0/1 chk=__builtin___vprintf_chk:-1/0/1 chk=__vprintf_chk:-1/0/1"},
    {"vfprintf", "RW, R str, other", "int",
     "printf=1/2 chk=__builtin___vfprintf_chk:0/-1/1/2 "
     "chk=__vfprintf_chk:0/-1/1/2"},
    {"vdprintf", "int, R str, other", "int",
     "printf=1/2 chk=__vdprintf_chk:0/-1/1/2"},
    {"vsprintf", "W bytes=fmtlen(a1)+1, R str, other", "int value=fmtlen(a1)",
     "writes-str=0/fmtlen(a1) printf=1/2 "
     "chk=__builtin___vsprintf_chk:0/-1/-1/1/2 "
     "chk=__vsprintf_chk:0/-1/-1/1/2"},
    {"vsnprintf", "W bytes=a1 nullable-if=a1, int, R str, other",
     "int value=fmtlen(a2)",
     "writes-str=0/min(a1-1,fmtlen(a2)) printf=2/3 "
     "chk=__builtin___vsnprintf_chk:0/1/-1/-1/2/3 "
     "chk=__vsnprintf_chk:0/1/-1/-1/2/3"},
    {"scanf", "R str, ...", "int",
     "scanf=0/1 chk=__isoc99_scanf:0/1 chk=__isoc23_scanf:0/1"},
    {"fscanf", "RW, R str, ...", "int",
     "scanf=1/2 chk=__isoc99_fscanf:0/1/2 chk=__isoc23_fscanf:0/1/2"},
    {"sscanf", "R str, R str, ...", "int",
     "scanf=1/2 chk=__isoc99_sscanf:0/1/2 chk=__isoc23_sscanf:0/1/2"},
    {"vscanf", "R str, other", "int",
     "scanf=0/1 chk=__isoc99_vscanf:0/1 chk=__isoc23_vscanf:0/1"},
    {"vfscanf", "RW, R str, other", "int",
     "scanf=1/2 chk=__isoc99_vfscanf:0/1/2 chk=__isoc23_vfscanf:0/1/2"},
    {"vsscanf", "R str, R str, other", "int",
     "scanf=1/2 chk=__isoc99_vsscanf:0/1/2 chk=__isoc23_vsscanf:0/1/2"},
    {"remove", "R str", "int", ""},
    {"rename", "R str, R str", "int", ""},
    {"renameat", "int, R str, int, R str", "int", ""},
    {"tmpnam", "W bytes=L_tmpnam nullable",
     "arg=0 null-on-failure else-static=tmpnam str", ""},
    {"ctermid", "W bytes=L_ctermid nullable",
     "arg=0 nonnull else-static=ctermid str", ""},
});

// <stdlib.h>, <inttypes.h>: conversions, environment, sorting and exit.
static constexpr auto StdlibExpectations = std::to_array<Expectation>({
    {"atoi", "R str", "int", ""},
    {"atol", "R str", "int", ""},
    {"atoll", "R str", "int", ""},
    {"atof", "R str", "int", ""},
    {"strtol", "R str, W nullable out=(interior=0 nonnull), int", "int",
     "chk=__isoc23_strtol:0/1/2"},
    {"strtoll", "R str, W nullable out=(interior=0 nonnull), int", "int",
     "chk=__isoc23_strtoll:0/1/2"},
    {"strtoul", "R str, W nullable out=(interior=0 nonnull), int", "int",
     "chk=__isoc23_strtoul:0/1/2"},
    {"strtoull", "R str, W nullable out=(interior=0 nonnull), int", "int",
     "chk=__isoc23_strtoull:0/1/2"},
    {"strtod", "R str, W nullable out=(interior=0 nonnull)", "int", ""},
    {"strtof", "R str, W nullable out=(interior=0 nonnull)", "int", ""},
    {"strtold", "R str, W nullable out=(interior=0 nonnull)", "int", ""},
    {"strtoimax", "R str, W nullable out=(interior=0 nonnull), int", "int",
     "chk=__isoc23_strtoimax:0/1/2"},
    {"strtoumax", "R str, W nullable out=(interior=0 nonnull), int", "int",
     "chk=__isoc23_strtoumax:0/1/2"},
    {"getenv", "R str", "static=environ nullable str", "reads=environ"},
    {"secure_getenv", "R str", "static=environ nullable str", "reads=environ"},
    {"setenv", "R str, R str, int", "int", "invalidates=environ reads=environ"},
    {"unsetenv", "R str", "int", "invalidates=environ reads=environ"},
    {"putenv", "R str retains=environ", "int",
     "invalidates=environ reads=environ"},
    {"clearenv", "", "int", "invalidates=environ"},
    {"system", "R str nullable", "int", "reads=environ"},
    {"qsort", "RW bytes=a1*a2, int, int, fn sync=0/0", "void", ""},
    {"qsort_r", "RW bytes=a1*a2, int, int, fn sync=0/0/4, none nullable",
     "void", ""},
    {"qsort_r#2", "RW bytes=a1*a2, int, int, none nullable, fn sync=3/0/0",
     "void", ""},
    {"bsearch", "none, R bytes=a2*a3, int, int, fn sync=0/1",
     "interior=1 nullable", ""},
    {"exit", "int", "noreturn", "exits"},
    {"_Exit", "int", "noreturn", "exits"},
    {"quick_exit", "int", "noreturn", "exits"},
    {"abort", "", "noreturn", "exits"},
    {"atexit", "fn atexit", "int", ""},
    {"at_quick_exit", "fn atexit", "int", ""},
    {"on_exit", "fn atexit, none nullable escapes", "int", ""},
    {"mblen", "R bytes=min(a1,strlen(a0)+1) nullable, int", "int", ""},
    {"mbtowc", "W nullable, R bytes=min(a2,strlen(a1)+1) nullable, int", "int",
     ""},
    {"wctomb", "W bytes=MB_CUR_MAX nullable, int", "int",
     "chk=__wctomb_chk:0/1/-1"},
    {"mbstowcs", "W count=a2 nullable, R str, int", "int",
     "chk=__mbstowcs_chk:0/1/2/-1"},
    {"wcstombs", "W bytes=a2 nullable, R str, int", "int",
     "chk=__wcstombs_chk:0/1/2/-1"},
    {"mkstemp", "RW str", "int", ""},
    {"mkostemp", "RW str, int", "int", ""},
    {"mkstemps", "RW str, int", "int", ""},
    {"mkdtemp", "RW str", "arg=0 null-on-failure", ""},
    {"mktemp", "RW str", "arg=0 nonnull", ""},
    {"rand", "", "int", ""},
    {"srand", "int", "void", ""},
    {"rand_r", "RW", "int", ""},
    {"random", "", "int", ""},
    {"srandom", "int", "void", ""},
    {"initstate", "int, W bytes=a2 escapes, int", "ptr nullable", ""},
    {"setstate", "RW escapes", "ptr nullable", ""},
    {"drand48", "", "int", ""},
    {"erand48", "RW count=3", "int", ""},
    {"lrand48", "", "int", ""},
    {"nrand48", "RW count=3", "int", ""},
    {"mrand48", "", "int", ""},
    {"jrand48", "RW count=3", "int", ""},
    {"srand48", "int", "void", ""},
    {"seed48", "R count=3", "static=seed48 nonnull", ""},
    {"lcong48", "R count=7", "void", ""},
    {"arc4random", "", "int", ""},
    {"arc4random_uniform", "int", "int", ""},
    {"arc4random_buf", "W bytes=a1, int", "void", ""},
    {"getsubopt", "RW, R, W out=(ptr nullable)", "int", ""},
    {"a64l", "R str", "int", ""},
    {"l64a", "int", "static=l64a nonnull str", ""},
    {"getloadavg", "W count=a1, int", "int", ""},
    {"getprogname", "", "interior-state=progname nonnull str",
     "reads=progname"},
    {"setprogname", "R str retains=progname", "void", ""},
    {"ptsname", "int", "static=ptsname null-on-failure str", ""},
    {"ptsname_r", "int, W bytes=a2, int", "int", ""},
    {"posix_openpt", "int", "int", ""},
    {"grantpt", "int", "int", ""},
    {"unlockpt", "int", "int", ""},
});

// <unistd.h>, <fcntl.h>, <sys/stat.h> and friends: POSIX.1-2024.
static constexpr auto UnistdExpectations = std::to_array<Expectation>({
    {"read", "int, W bytes=a2 nullable-if=a2, int", "int",
     "chk=__read_chk:0/1/2/-1"},
    {"write", "int, R bytes=a2 nullable-if=a2, int", "int", ""},
    {"pread", "int, W bytes=a2 nullable-if=a2, int, int", "int",
     "chk=__pread_chk:0/1/2/3/-1 chk=__pread64_chk:0/1/2/3/-1"},
    {"pwrite", "int, R bytes=a2 nullable-if=a2, int, int", "int", ""},
    {"close", "int", "int", ""},
    {"pipe", "W count=2", "int", ""},
    {"pipe2", "W count=2, int", "int", ""},
    {"dup", "int", "int", ""},
    {"dup2", "int, int", "int", ""},
    {"dup3", "int, int, int", "int", ""},
    {"lseek", "int, int, int", "int", ""},
    {"access", "R str, int", "int", ""},
    {"faccessat", "int, R str, int, int", "int", ""},
    {"unlink", "R str", "int", ""},
    {"unlinkat", "int, R str, int", "int", ""},
    {"rmdir", "R str", "int", ""},
    {"chdir", "R str", "int", ""},
    {"fchdir", "int", "int", ""},
    {"getwd", "W bytes=PATH_MAX", "arg=0 null-on-failure", ""},
    {"readlink", "R str, W bytes=a2, int", "int",
     "chk=__readlink_chk:0/1/2/-1"},
    {"readlinkat", "int, R str, W bytes=a3, int", "int",
     "chk=__readlinkat_chk:0/1/2/3/-1"},
    {"symlink", "R str, R str", "int", ""},
    {"symlinkat", "R str, int, R str", "int", ""},
    {"link", "R str, R str", "int", ""},
    {"linkat", "int, R str, int, R str, int", "int", ""},
    {"chown", "R str, int, int", "int", ""},
    {"fchown", "int, int, int", "int", ""},
    {"lchown", "R str, int, int", "int", ""},
    {"fchownat", "int, R str, int, int, int", "int", ""},
    {"truncate", "R str, int", "int", ""},
    {"ftruncate", "int, int", "int", ""},
    {"fsync", "int", "int", ""},
    {"fdatasync", "int", "int", ""},
    {"sync", "", "void", ""},
    {"isatty", "int", "int", ""},
    {"ttyname", "int", "static=ttyname null-on-failure str", ""},
    {"ttyname_r", "int, W bytes=a2, int", "int",
     "chk=__ttyname_r_chk:0/1/2/-1"},
    {"gethostname", "W bytes=a1, int", "int", "chk=__gethostname_chk:0/1/-1"},
    {"sethostname", "R bytes=a1, int", "int", ""},
    {"getdomainname", "W bytes=a1, int", "int",
     "chk=__getdomainname_chk:0/1/-1"},
    {"getlogin", "", "static=getlogin null-on-failure str", ""},
    {"getlogin_r", "W bytes=a1, int", "int", "chk=__getlogin_r_chk:0/1/-1"},
    {"getopt", "int, RW count=a0, R str", "int", ""},
    {"execv", "R str, R", "int", "reads=environ"},
    {"execve", "R str, R, R nullable", "int", ""},
    {"execvp", "R str, R", "int", "reads=environ"},
    {"execvpe", "R str, R, R", "int", "reads=environ"},
    {"execl", "R str, R str nullable, ...", "int", "reads=environ"},
    {"execlp", "R str, R str nullable, ...", "int", "reads=environ"},
    {"execle", "R str, R str nullable, ...", "int", ""},
    {"fexecve", "int, R, R", "int", ""},
    {"fork", "", "int", ""},
    {"vfork", "", "int", "returns-twice"},
    {"_exit", "int", "noreturn", "exits"},
    {"alarm", "int", "int", ""},
    {"pause", "", "int", ""},
    {"sleep", "int", "int", ""},
    {"usleep", "int", "int", ""},
    {"getpid", "", "int", ""},
    {"getppid", "", "int", ""},
    {"sysconf", "int", "int", ""},
    {"pathconf", "R str, int", "int", ""},
    {"fpathconf", "int, int", "int", ""},
    {"confstr", "int, W bytes=a2 nullable-if=a2, int", "int",
     "chk=__confstr_chk:0/1/2/-1"},
    {"chroot", "R str", "int", ""},
    {"nice", "int", "int", ""},
    {"getentropy", "W bytes=a1, int", "int", ""},
    {"getgroups", "int, W count=a0 nullable-if=a0", "int",
     "chk=__getgroups_chk:0/1/-1"},
    {"swab", "R bytes=a2, W bytes=a2, int", "void", "disjoint=0/1/a2"},
    {"getopt_long", "int, RW count=a0, R str, R nullable, W nullable", "int",
     ""},
    {"getopt_long_only", "int, RW count=a0, R str, R nullable, W nullable",
     "int", ""},
    {"open", "R str, int, ...", "int", "chk=__open_2:0/1"},
    {"openat", "int, R str, int, ...", "int", "chk=__openat_2:0/1/2"},
    {"creat", "R str, int", "int", ""},
    {"fcntl", "int, int, ...", "int", ""},
    {"posix_fadvise", "int, int, int, int", "int", ""},
    {"posix_fallocate", "int, int, int", "int", ""},
    {"stat", "R str, W", "int", ""},
    {"fstat", "int, W", "int", ""},
    {"lstat", "R str, W", "int", ""},
    {"fstatat", "int, R str, W, int", "int", ""},
    {"chmod", "R str, int", "int", ""},
    {"fchmod", "int, int", "int", ""},
    {"fchmodat", "int, R str, int, int", "int", ""},
    {"mkdir", "R str, int", "int", ""},
    {"mkdirat", "int, R str, int", "int", ""},
    {"mkfifo", "R str, int", "int", ""},
    {"mkfifoat", "int, R str, int", "int", ""},
    {"mknod", "R str, int, int", "int", ""},
    {"mknodat", "int, R str, int, int", "int", ""},
    {"umask", "int", "int", ""},
    {"utimensat", "int, R str, R count=2 nullable, int", "int", ""},
    {"futimens", "int, R count=2 nullable", "int", ""},
    {"statvfs", "R str, W", "int", ""},
    {"fstatvfs", "int, W", "int", ""},
    {"statfs", "R str, W", "int", ""},
    {"fstatfs", "int, W", "int", ""},
    {"utime", "R str, R nullable", "int", ""},
    {"utimes", "R str, R count=2 nullable", "int", ""},
    {"futimes", "int, R count=2 nullable", "int", ""},
    {"lutimes", "R str, R count=2 nullable", "int", ""},
    {"gettimeofday", "W, W nullable", "int", ""},
    {"settimeofday", "R, R nullable", "int", ""},
    {"getitimer", "int, W", "int", ""},
    {"setitimer", "int, R, W nullable", "int", ""},
});

// <dirent.h>, <time.h>, <sys/mman.h>, <dlfcn.h>.
static constexpr auto SystemExpectations = std::to_array<Expectation>({
    {"opendir", "R str", "fresh=closedir null-on-failure", ""},
    {"fdopendir", "int", "fresh=closedir null-on-failure", ""},
    {"closedir", "RW releases=closedir", "int", ""},
    {"readdir", "RW", "interior=0 nullable", ""},
    {"readdir_r", "RW, W, W out=(arg=1 nullable)", "int", ""},
    {"rewinddir", "RW", "void", ""},
    {"seekdir", "RW, int", "void", ""},
    {"telldir", "R", "int", ""},
    {"dirfd", "R", "int", ""},
    {"scandir",
     "R str, W out=(fresh=free null-on-failure unzeroed), fn? sync=none, "
     "fn? sync=none",
     "int", ""},
    {"alphasort", "R, R", "int", ""},
    {"versionsort", "R, R", "int", ""},
    {"time", "W nullable", "int", ""},
    {"clock", "", "int", ""},
    {"difftime", "int, int", "int", ""},
    {"mktime", "RW", "int", "reads=environ"},
    {"timegm", "RW", "int", ""},
    {"asctime", "R", "static=asctime null-on-failure str", ""},
    {"ctime", "R", "static=asctime null-on-failure str", "reads=environ"},
    {"gmtime", "R", "static=tm null-on-failure", ""},
    {"localtime", "R", "static=tm null-on-failure", "reads=environ"},
    {"asctime_r", "R, W bytes=26", "arg=1 null-on-failure str", ""},
    {"ctime_r", "R, W bytes=26", "arg=1 null-on-failure str", "reads=environ"},
    {"gmtime_r", "R, W", "arg=1 null-on-failure", ""},
    {"localtime_r", "R, W", "arg=1 null-on-failure", "reads=environ"},
    {"strftime", "W bytes=a1, int, R str, R", "int", "reads=environ"},
    {"strftime_l", "W bytes=a1, int, R str, R, none", "int", ""},
    {"strptime", "R str, R str, W", "interior=0 nullable", ""},
    {"tzset", "", "void", "reads=environ"},
    {"timespec_get", "W, int", "int", ""},
    {"timespec_getres", "W nullable, int", "int", ""},
    {"clock_gettime", "int, W", "int", ""},
    {"clock_getres", "int, W nullable", "int", ""},
    {"clock_settime", "int, R", "int", ""},
    {"clock_nanosleep", "int, int, R, W nullable", "int", ""},
    {"nanosleep", "R, W nullable", "int", ""},
    {"timer_create", "int, R nullable entry=none, W", "int", ""},
    {"timer_settime", "none nullable, int, R, W nullable", "int", ""},
    {"timer_gettime", "none nullable, W", "int", ""},
    {"timer_getoverrun", "none nullable", "int", ""},
    {"timer_delete", "none nullable", "int", ""},
    {"mmap", "none nullable, int, int, int, int, int",
     "fresh=munmap nonnull extent=a1", ""},
    {"munmap", "none releases=munmap, int", "int", ""},
    {"mprotect", "none, int, int", "int", ""},
    {"msync", "none, int, int", "int", ""},
    {"madvise", "none, int, int", "int", ""},
    {"posix_madvise", "none, int, int", "int", ""},
    {"mlock", "none, int", "int", ""},
    {"munlock", "none, int", "int", ""},
    {"mincore", "none, int, W", "int", ""},
    {"shm_open", "R str, int, int", "int", ""},
    {"shm_unlink", "R str", "int", ""},
    {"memfd_create", "R str, int", "int", ""},
    {"dlopen", "R str nullable, int", "fresh=dlclose null-on-failure", ""},
    {"dlclose", "none releases=dlclose", "int", ""},
    {"dlsym", "none nullable, R str", "ptr nullable", ""},
    {"dlerror", "", "static=dlerror nullable str", ""},
    {"dladdr", "none, W", "int", ""},
});

// Users and groups, locales, iconv, paths, patterns and messages.
static constexpr auto UsersExpectations = std::to_array<Expectation>({
    {"getpwnam", "R str", "static=passwd nullable", "invalidates=passwd"},
    {"getpwuid", "int", "static=passwd nullable", "invalidates=passwd"},
    {"getpwent", "", "static=passwd nullable", "invalidates=passwd"},
    {"setpwent", "", "void", ""},
    {"endpwent", "", "void", ""},
    {"getpwnam_r", "R str, W, W bytes=a3, int, W out=(arg=1 nullable)", "int",
     ""},
    {"getpwuid_r", "int, W, W bytes=a3, int, W out=(arg=1 nullable)", "int",
     ""},
    {"getgrnam", "R str", "static=group nullable", "invalidates=group"},
    {"getgrgid", "int", "static=group nullable", "invalidates=group"},
    {"getgrent", "", "static=group nullable", "invalidates=group"},
    {"setgrent", "", "void", ""},
    {"endgrent", "", "void", ""},
    {"getgrnam_r", "R str, W, W bytes=a3, int, W out=(arg=1 nullable)", "int",
     ""},
    {"getgrgid_r", "int, W, W bytes=a3, int, W out=(arg=1 nullable)", "int",
     ""},
    {"setgroups", "int, R count=a0", "int", ""},
    {"initgroups", "R str, int", "int", ""},
    {"setlocale", "int, R str nullable", "static=setlocale null-on-failure str",
     "invalidates=setlocale invalidates=localeconv invalidates=nl_langinfo"},
    {"localeconv", "", "static=localeconv nonnull", ""},
    {"newlocale", "int, R str, none nullable reallocs=freelocale",
     "fresh=freelocale null-on-failure", ""},
    {"duplocale", "none", "fresh=freelocale null-on-failure", ""},
    {"freelocale", "none releases=freelocale", "void", ""},
    {"uselocale", "none nullable", "ptr nullable", ""},
    {"nl_langinfo", "int", "static=nl_langinfo nonnull str", ""},
    {"nl_langinfo_l", "int, none", "static=nl_langinfo nonnull str", ""},
    {"iconv_open", "R str, R str", "fresh=iconv_close nonnull", ""},
    {"iconv", "none, RW nullable, RW nullable, RW nullable, RW nullable", "int",
     ""},
    {"iconv_close", "none releases=iconv_close", "int", ""},
    {"basename", "RW str nullable", "interior=0 nonnull str",
     "chk=__xpg_basename:0"},
    {"dirname", "RW str nullable", "interior=0 nonnull str", ""},
    {"regcomp", "W inits=regfree, R str, int", "int", ""},
    {"regexec", "R, R str, int, W count=a2 nullable, int", "int", ""},
    {"regerror", "int, R, W bytes=a3 nullable-if=a3, int", "int", ""},
    {"regfree", "RW finis=regfree", "void", ""},
    {"fnmatch", "R str, R str, int", "int", ""},
    {"glob", "R str, int, fn? sync=none, RW inits=globfree", "int", ""},
    {"globfree", "RW finis=globfree", "void", ""},
    {"wordexp", "R str, RW inits=wordfree, int", "int", "reads=environ"},
    {"wordfree", "RW finis=wordfree", "void", ""},
    {"err", "int, R str nullable, ...", "noreturn", "exits printf=1/2"},
    {"errx", "int, R str nullable, ...", "noreturn", "exits printf=1/2"},
    {"verr", "int, R str nullable, other", "noreturn", "exits printf=1/2"},
    {"verrx", "int, R str nullable, other", "noreturn", "exits printf=1/2"},
    {"warn", "R str nullable, ...", "void", "printf=0/1"},
    {"warnx", "R str nullable, ...", "void", "printf=0/1"},
    {"vwarn", "R str nullable, other", "void", "printf=0/1"},
    {"vwarnx", "R str nullable, other", "void", "printf=0/1"},
    {"error", "int, int, R str, ...", "void", "printf=2/3"},
    {"error_at_line", "int, int, R str nullable, int, R str, ...", "void",
     "printf=4/5"},
    {"openlog", "R str nullable retains=syslog, int, int", "void", ""},
    {"syslog", "int, R str, ...", "void",
     "reads=syslog printf=1/2 chk=__syslog_chk:0/-1/1/2"},
    {"vsyslog", "int, R str, other", "void",
     "reads=syslog printf=1/2 chk=__vsyslog_chk:0/-1/1/2"},
    {"closelog", "", "void", ""},
    {"setlogmask", "int", "int", ""},
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
    {"pthread_create", "W, R nullable, fn entry=3, none nullable escapes",
     "int", ""},
    {"pthread_join", "other, W nullable", "int", ""},
    {"pthread_detach", "other", "int", ""},
    {"pthread_exit", "none nullable escapes", "noreturn", ""},
    {"pthread_self", "", "int", ""},
    {"pthread_self#2", "", "ptr nonnull", ""},
    {"pthread_equal", "other, other", "int", ""},
    {"pthread_cancel", "other", "int", ""},
    {"pthread_kill", "other, int", "int", ""},
    {"pthread_setcancelstate", "int, W nullable", "int", ""},
    {"pthread_setcanceltype", "int, W nullable", "int", ""},
    {"pthread_testcancel", "", "void", ""},
    {"pthread_attr_init", "W", "int", ""},
    {"pthread_attr_destroy", "RW", "int", ""},
    {"pthread_attr_setdetachstate", "RW, int", "int", ""},
    {"pthread_attr_getdetachstate", "R, W", "int", ""},
    {"pthread_attr_setstacksize", "RW, int", "int", ""},
    {"pthread_attr_getstacksize", "R, W", "int", ""},
    {"pthread_attr_setstack", "RW, none escapes, int", "int", ""},
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
    {"pthread_cond_timedwait", "RW, RW, R", "int", ""},
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
    {"pthread_once", "RW, fn sync=none", "int", ""},
    {"pthread_key_create", "W, fn? entry=none", "int", ""},
    {"pthread_key_delete", "int", "int", ""},
    {"pthread_setspecific", "int, none nullable escapes", "int", ""},
    {"pthread_getspecific", "int", "ptr nullable", ""},
    {"pthread_sigmask", "int, R nullable, W nullable", "int", ""},
    {"pthread_atfork", "fn? entry=none, fn? entry=none, fn? entry=none", "int",
     ""},
    {"pthread_setname_np", "other, R str", "int", ""},
    {"pthread_setname_np#2", "R str", "int", ""},
    {"pthread_getname_np", "other, W bytes=a2, int", "int", ""},
    {"thrd_create", "W, fn entry=2, none nullable escapes", "int", ""},
    {"thrd_join", "other, W nullable", "int", ""},
    {"thrd_detach", "other", "int", ""},
    {"thrd_exit", "int", "noreturn", ""},
    {"thrd_sleep", "R, W nullable", "int", ""},
    {"thrd_yield", "", "void", ""},
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
    {"tss_create", "W, fn? entry=none", "int", ""},
    {"tss_delete", "int", "void", ""},
    {"tss_get", "int", "ptr nullable", ""},
    {"tss_set", "int, none nullable escapes", "int", ""},
    {"call_once", "RW, fn sync=none", "void", ""},
});

// Signals (handlers are entry points), non-local jumps (§5.4), scheduling,
// semaphores, trees and searches (synchronous comparators), walks, spawning
// and polling.
static constexpr auto SignalExpectations = std::to_array<Expectation>({
    {"signal", "int, fn? entry=none", "ptr nullable", ""},
    {"sigaction", "int, R nullable entry=none, W nullable", "int", ""},
    {"raise", "int", "int", ""},
    {"kill", "int, int", "int", ""},
    {"killpg", "int, int", "int", ""},
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
    {"siginterrupt", "int, int", "int", ""},
    {"sigqueue", "int, int, other", "int", ""},
    {"psignal", "int, R str nullable", "void", ""},
    {"psiginfo", "R, R str nullable", "void", ""},
    {"wait", "W nullable", "int", ""},
    {"waitpid", "int, W nullable, int", "int", ""},
    {"waitid", "int, int, W, int", "int", ""},
    {"wait4", "int, W nullable, int, W nullable", "int", ""},
    {"setjmp", "other", "int", "returns-twice"},
    {"_setjmp", "other", "int", "returns-twice"},
    {"sigsetjmp", "other, int", "int", "returns-twice chk=__sigsetjmp:0/1"},
    {"longjmp", "other, int", "noreturn", "chk=__longjmp_chk:0/1"},
    {"_longjmp", "other, int", "noreturn", ""},
    {"siglongjmp", "other, int", "noreturn", ""},
    {"sched_yield", "", "int", ""},
    {"sched_getaffinity", "int, int, W bytes=a1", "int", ""},
    {"sched_setaffinity", "int, int, R bytes=a1", "int", ""},
    {"sched_getparam", "int, W", "int", ""},
    {"sched_setparam", "int, R", "int", ""},
    {"sched_setscheduler", "int, int, R", "int", ""},
    {"sem_open", "R str, int, ...", "fresh=sem_close nullable", ""},
    {"sem_close", "RW releases=sem_close", "int", ""},
    {"sem_unlink", "R str", "int", ""},
    {"sem_init", "W, int, int", "int", ""},
    {"sem_destroy", "RW", "int", ""},
    {"sem_wait", "RW", "int", ""},
    {"sem_trywait", "RW", "int", ""},
    {"sem_timedwait", "RW, R", "int", ""},
    {"sem_post", "RW", "int", ""},
    {"sem_getvalue", "RW, W", "int", ""},
    {"tsearch", "none escapes, RW, fn sync=0/1", "ptr nullable", ""},
    {"tfind", "none, R, fn sync=0/1", "ptr nullable", ""},
    {"tdelete", "none, RW, fn sync=0/1", "ptr nullable", ""},
    {"twalk", "none nullable, fn sync=0", "void", ""},
    {"tdestroy", "none nullable, fn sync=none", "void", ""},
    {"lfind", "none, R bytes=__WEAVEC_UNBOUNDED, RW, int, fn sync=0/1",
     "interior=1 nullable", ""},
    {"lsearch", "none, RW bytes=__WEAVEC_UNBOUNDED, RW, int, fn sync=0/1",
     "interior=1 nonnull", ""},
    {"ftw", "R str, fn sync=none, int", "int", ""},
    {"nftw", "R str, fn sync=none, int, int", "int", ""},
    {"posix_spawn", "W nullable, R str, R nullable, R nullable, R, R", "int",
     ""},
    {"posix_spawnp", "W nullable, R str, R nullable, R nullable, R, R", "int",
     "reads=environ"},
    {"poll", "RW count=a1 nullable-if=a1, int, int", "int",
     "chk=__poll_chk:0/1/2/-1"},
    {"ppoll", "RW count=a1 nullable-if=a1, int, R nullable, R nullable", "int",
     "chk=__ppoll_chk:0/1/2/3/-1"},
    {"select", "int, RW nullable, RW nullable, RW nullable, RW nullable", "int",
     ""},
    {"pselect",
     "int, RW nullable, RW nullable, RW nullable, R nullable, R nullable",
     "int", ""},
    {"epoll_create", "int", "int", ""},
    {"epoll_create1", "int", "int", ""},
    {"epoll_ctl", "int, int, int, R nullable", "int", ""},
    {"epoll_wait", "int, W count=a2, int, int", "int", ""},
    {"epoll_pwait", "int, W count=a2, int, int, R nullable", "int", ""},
    {"kqueue", "", "int", ""},
    {"kevent",
     "int, R count=a2 nullable-if=a2, int, W count=a4 nullable-if=a4, int, "
     "R nullable",
     "int", ""},
});

// Sockets, name resolution, interfaces and the remaining system interfaces.
static constexpr auto NetworkExpectations = std::to_array<Expectation>({
    {"socket", "int, int, int", "int", ""},
    {"socketpair", "int, int, int, W count=2", "int", ""},
    {"bind", "int, R bytes=a2, int", "int", ""},
    {"connect", "int, R bytes=a2, int", "int", ""},
    {"listen", "int, int", "int", ""},
    {"accept", "int, W nullable, RW nullable", "int", ""},
    {"accept4", "int, W nullable, RW nullable, int", "int", ""},
    {"shutdown", "int, int", "int", ""},
    {"send", "int, R bytes=a2 nullable-if=a2, int, int", "int", ""},
    {"sendto",
     "int, R bytes=a2 nullable-if=a2, int, int, R bytes=a5 nullable, int",
     "int", ""},
    {"sendmsg", "int, R, int", "int", ""},
    {"recv", "int, W bytes=a2 nullable-if=a2, int, int", "int",
     "chk=__recv_chk:0/1/2/-1/3"},
    {"recvfrom",
     "int, W bytes=a2 nullable-if=a2, int, int, W nullable, RW nullable", "int",
     "chk=__recvfrom_chk:0/1/2/-1/3/4/5"},
    {"recvmsg", "int, RW, int", "int", ""},
    {"getsockopt", "int, int, int, W nullable, RW", "int", ""},
    {"setsockopt", "int, int, int, R bytes=a4 nullable-if=a4, int", "int", ""},
    {"getsockname", "int, W, RW", "int", ""},
    {"getpeername", "int, W, RW", "int", ""},
    {"getaddrinfo",
     "R str nullable, R str nullable, R nullable, "
     "W out=(fresh=freeaddrinfo null-on-failure)",
     "int", ""},
    {"freeaddrinfo", "none releases=freeaddrinfo", "void", ""},
    {"gai_strerror", "int", "static=gai_strerror nonnull str", ""},
    {"getnameinfo",
     "R bytes=a1, int, W bytes=a3 nullable, int, W bytes=a5 nullable, int, "
     "int",
     "int", ""},
    {"gethostbyname", "R str", "static=hostent nullable",
     "invalidates=hostent"},
    {"gethostbyname2", "R str, int", "static=hostent nullable",
     "invalidates=hostent"},
    {"gethostbyaddr", "R bytes=a1, int, int", "static=hostent nullable",
     "invalidates=hostent"},
    {"getservbyname", "R str, R str nullable", "static=servent nullable",
     "invalidates=servent"},
    {"getservbyport", "int, R str nullable", "static=servent nullable",
     "invalidates=servent"},
    {"getprotobyname", "R str", "static=protoent nullable",
     "invalidates=protoent"},
    {"getprotobynumber", "int", "static=protoent nullable",
     "invalidates=protoent"},
    {"herror", "R str nullable", "void", ""},
    {"hstrerror", "int", "static=hstrerror nonnull str", ""},
    {"inet_ntop", "int, R, W bytes=a3, int", "arg=2 null-on-failure str", ""},
    {"inet_pton", "int, R str, W", "int", ""},
    {"inet_addr", "R str", "int", ""},
    {"inet_aton", "R str, W", "int", ""},
    {"inet_network", "R str", "int", ""},
    {"inet_ntoa", "other", "static=inet_ntoa nonnull str", ""},
    {"htons", "int", "int", ""},
    {"htonl", "int", "int", ""},
    {"ntohs", "int", "int", ""},
    {"ntohl", "int", "int", ""},
    {"getifaddrs", "W out=(fresh=freeifaddrs null-on-failure)", "int", ""},
    {"freeifaddrs", "none releases=freeifaddrs", "void", ""},
    {"if_nametoindex", "R str", "int", ""},
    {"if_indextoname", "int, W bytes=IF_NAMESIZE", "arg=1 null-on-failure str",
     ""},
    {"if_nameindex", "", "fresh=if_freenameindex null-on-failure", ""},
    {"if_freenameindex", "none releases=if_freenameindex", "void", ""},
    {"readv", "int, R count=a2, int", "int", ""},
    {"writev", "int, R count=a2, int", "int", ""},
    {"preadv", "int, R count=a2, int, int", "int", ""},
    {"pwritev", "int, R count=a2, int, int", "int", ""},
    {"sendfile", "int, int, RW nullable, int", "int", ""},
    {"getrandom", "W bytes=a1, int, int", "int", ""},
    {"eventfd", "int, int", "int", ""},
    {"inotify_init", "", "int", ""},
    {"inotify_init1", "int", "int", ""},
    {"inotify_add_watch", "int, R str, int", "int", ""},
    {"inotify_rm_watch", "int, int", "int", ""},
    {"timerfd_create", "int, int", "int", ""},
    {"timerfd_settime", "int, int, R, W nullable", "int", ""},
    {"timerfd_gettime", "int, W", "int", ""},
    {"signalfd", "int, R, int", "int", ""},
    {"prctl", "int, ...", "int", ""},
    {"sysinfo", "W", "int", ""},
    {"sysctlbyname", "R str, W nullable, RW nullable, R nullable, int", "int",
     ""},
    {"flock", "int, int", "int", ""},
    {"shmget", "int, int, int", "int", ""},
    {"shmat", "int, none nullable, int", "fresh=shmdt nonnull", ""},
    {"shmdt", "none releases=shmdt", "int", ""},
    {"shmctl", "int, int, RW nullable", "int", ""},
    {"mq_open", "R str, int, ...", "int", ""},
    {"mq_close", "int", "int", ""},
    {"mq_unlink", "R str", "int", ""},
    {"mq_send", "int, R bytes=a2, int, int", "int", ""},
    {"mq_receive", "int, W bytes=a2, int, W nullable", "int", ""},
    {"aio_read", "RW escapes", "int", ""},
    {"aio_write", "RW escapes", "int", ""},
    {"aio_error", "R", "int", ""},
    {"aio_return", "RW", "int", ""},
    {"aio_suspend", "R count=a1, int, R nullable", "int", ""},
    {"aio_cancel", "int, RW nullable", "int", ""},
    {"setmntent", "R str, R str", "fresh=endmntent null-on-failure", ""},
    {"getmntent", "RW", "static=mntent nullable", "invalidates=mntent"},
    {"endmntent", "RW releases=endmntent", "int", ""},
    {"fts_open", "R, int, fn? sync=none", "fresh=fts_close null-on-failure",
     ""},
    {"fts_read", "RW", "interior=0 nullable", ""},
    {"fts_children", "RW, int", "interior=0 nullable", ""},
    {"fts_set", "RW, RW, int", "int", ""},
    {"fts_close", "RW releases=fts_close", "int", ""},
});

// <wchar.h>, <wctype.h>, <uchar.h>: wide lengths count wchar_t elements;
// multibyte buffers count bytes.
static constexpr auto WideExpectations = std::to_array<Expectation>({
    {"wcslen", "R str", "int value=strlen(a0)", ""},
    {"wcsnlen", "R count=min(a1,strlen(a0)+1), int",
     "int value=min(a1,strlen(a0))", ""},
    {"wcscpy", "W count=strlen(a1)+1, R str", "arg=0 nonnull",
     "chk=__wcscpy_chk:0/1/-1"},
    {"wcsncpy", "W count=a2, R count=min(a2,strlen(a1)+1), int",
     "arg=0 nonnull", "chk=__wcsncpy_chk:0/1/2/-1"},
    {"wcpcpy", "W count=strlen(a1)+1, R str", "interior=0 nonnull",
     "chk=__wcpcpy_chk:0/1/-1"},
    {"wcpncpy", "W count=a2, R count=min(a2,strlen(a1)+1), int",
     "interior=0 nonnull", "chk=__wcpncpy_chk:0/1/2/-1"},
    {"wcscat", "RW str count=strlen(a0)+strlen(a1)+1, R str", "arg=0 nonnull",
     "chk=__wcscat_chk:0/1/-1"},
    {"wcsncat",
     "RW str count=strlen(a0)+min(a2,strlen(a1))+1, "
     "R count=min(a2,strlen(a1)+1), int",
     "arg=0 nonnull", "chk=__wcsncat_chk:0/1/2/-1"},
    {"wcscmp", "R str, R str", "int", ""},
    {"wcsncmp",
     "R count=min(a2,strlen(a0)+1), R count=min(a2,strlen(a1)+1), int", "int",
     ""},
    {"wcscasecmp", "R str, R str", "int", ""},
    {"wcsncasecmp",
     "R count=min(a2,strlen(a0)+1), R count=min(a2,strlen(a1)+1), int", "int",
     ""},
    {"wcscoll", "R str, R str", "int", ""},
    {"wcsxfrm", "W count=a2 nullable-if=a2, R str, int", "int", ""},
    {"wcschr", "R str, int", "interior=0 nullable", ""},
    {"wcsrchr", "R str, int", "interior=0 nullable", ""},
    {"wcsstr", "R str, R str", "interior=0 nullable", ""},
    {"wcspbrk", "R str, R str", "interior=0 nullable", ""},
    {"wcsspn", "R str, R str", "int", ""},
    {"wcscspn", "R str, R str", "int", ""},
    {"wcstok", "RW str nullable, R str, RW out=(interior=0 nullable)",
     "interior=0 nullable", ""},
    {"wcswidth", "R count=min(a1,strlen(a0)+1), int", "int", ""},
    {"wcwidth", "int", "int", ""},
    {"wmemcpy", "W count=a2 nullable-if=a2, R count=a2 nullable-if=a2, int",
     "arg=0 nonnull", "chk=__wmemcpy_chk:0/1/2/-1"},
    {"wmemmove", "W count=a2 nullable-if=a2, R count=a2 nullable-if=a2, int",
     "arg=0 nonnull", "chk=__wmemmove_chk:0/1/2/-1"},
    {"wmemset", "W count=a2 nullable-if=a2, int, int", "arg=0 nonnull",
     "chk=__wmemset_chk:0/1/2/-1"},
    {"wmemcmp", "R count=a2 nullable-if=a2, R count=a2 nullable-if=a2, int",
     "int", ""},
    {"wmemchr", "R count=a2, int, int", "interior=0 nullable", ""},
    {"wmempcpy", "W count=a2, R count=a2, int", "interior=0 nonnull",
     "chk=__wmempcpy_chk:0/1/2/-1"},
    {"wcstol", "R str, W nullable out=(interior=0 nonnull), int", "int",
     "chk=__isoc23_wcstol:0/1/2"},
    {"wcstoll", "R str, W nullable out=(interior=0 nonnull), int", "int",
     "chk=__isoc23_wcstoll:0/1/2"},
    {"wcstoul", "R str, W nullable out=(interior=0 nonnull), int", "int",
     "chk=__isoc23_wcstoul:0/1/2"},
    {"wcstoull", "R str, W nullable out=(interior=0 nonnull), int", "int",
     "chk=__isoc23_wcstoull:0/1/2"},
    {"wcstod", "R str, W nullable out=(interior=0 nonnull)", "int", ""},
    {"wcstof", "R str, W nullable out=(interior=0 nonnull)", "int", ""},
    {"wcstold", "R str, W nullable out=(interior=0 nonnull)", "int", ""},
    {"btowc", "int", "int", ""},
    {"wctob", "int", "int", ""},
    {"mbsinit", "R nullable", "int", ""},
    {"mbrlen", "R bytes=min(a1,strlen(a0)+1) nullable, int, RW nullable", "int",
     ""},
    {"mbrtowc",
     "W nullable, R bytes=min(a2,strlen(a1)+1) nullable, int, RW nullable",
     "int", ""},
    {"wcrtomb", "W bytes=MB_CUR_MAX nullable, int, RW nullable", "int",
     "chk=__wcrtomb_chk:0/1/2/-1"},
    {"mbsrtowcs", "W count=a2 nullable, RW, int, RW nullable", "int",
     "chk=__mbsrtowcs_chk:0/1/2/3/-1"},
    {"wcsrtombs", "W bytes=a2 nullable, RW, int, RW nullable", "int",
     "chk=__wcsrtombs_chk:0/1/2/3/-1"},
    {"mbsnrtowcs", "W count=a3 nullable, RW, int, int, RW nullable", "int",
     "chk=__mbsnrtowcs_chk:0/1/2/3/4/-1"},
    {"wcsnrtombs", "W bytes=a3 nullable, RW, int, int, RW nullable", "int",
     "chk=__wcsnrtombs_chk:0/1/2/3/4/-1"},
    {"fgetwc", "RW", "int", ""},
    {"getwc", "RW", "int", ""},
    {"getwchar", "", "int", ""},
    {"fputwc", "int, RW", "int", ""},
    {"putwc", "int, RW", "int", ""},
    {"putwchar", "int", "int", ""},
    {"ungetwc", "int, RW", "int", ""},
    {"fgetws", "W count=a1, int, RW", "arg=0 nullable",
     "chk=__fgetws_chk:0/-1/1/2"},
    {"fputws", "R str, RW", "int", ""},
    {"fwide", "RW, int", "int", ""},
    {"wprintf", "R str, ...", "int", "printf=0/1 chk=__wprintf_chk:-1/0/1"},
    {"fwprintf", "RW, R str, ...", "int",
     "printf=1/2 chk=__fwprintf_chk:0/-1/1/2"},
    {"swprintf", "W count=a1 nullable-if=a1, int, R str, ...", "int",
     "printf=2/3 chk=__swprintf_chk:0/1/-1/-1/2/3"},
    {"vwprintf", "R str, other", "int", "printf=0/1 chk=__vwprintf_chk:-1/0/1"},
    {"vfwprintf", "RW, R str, other", "int",
     "printf=1/2 chk=__vfwprintf_chk:0/-1/1/2"},
    {"vswprintf", "W count=a1 nullable-if=a1, int, R str, other", "int",
     "printf=2/3 chk=__vswprintf_chk:0/1/-1/-1/2/3"},
    {"wscanf", "R str, ...", "int",
     "scanf=0/1 chk=__isoc99_wscanf:0/1 chk=__isoc23_wscanf:0/1"},
    {"fwscanf", "RW, R str, ...", "int",
     "scanf=1/2 chk=__isoc99_fwscanf:0/1/2 chk=__isoc23_fwscanf:0/1/2"},
    {"swscanf", "R str, R str, ...", "int",
     "scanf=1/2 chk=__isoc99_swscanf:0/1/2 chk=__isoc23_swscanf:0/1/2"},
    {"vwscanf", "R str, other", "int",
     "scanf=0/1 chk=__isoc99_vwscanf:0/1 chk=__isoc23_vwscanf:0/1"},
    {"vfwscanf", "RW, R str, other", "int",
     "scanf=1/2 chk=__isoc99_vfwscanf:0/1/2 chk=__isoc23_vfwscanf:0/1/2"},
    {"vswscanf", "R str, R str, other", "int",
     "scanf=1/2 chk=__isoc99_vswscanf:0/1/2 chk=__isoc23_vswscanf:0/1/2"},
    {"wcsftime", "W count=a1, int, R str, R", "int", "reads=environ"},
    {"open_wmemstream",
     "W escapes out=(fresh=free null-on-failure unzeroed), W escapes",
     "fresh=fclose null-on-failure", ""},
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
    {"c8rtomb", "W bytes=MB_CUR_MAX nullable, int, RW nullable", "int", ""},
    {"c16rtomb", "W bytes=MB_CUR_MAX nullable, int, RW nullable", "int", ""},
    {"c32rtomb", "W bytes=MB_CUR_MAX nullable, int, RW nullable", "int", ""},
});

// <math.h>, <errno.h>, <ctype.h>.
static constexpr auto MathExpectations = std::to_array<Expectation>({
    {"fabs", "other", "int", ""},
    {"fabsf", "other", "int", ""},
    {"fabsl", "other", "int", ""},
    {"floor", "other", "int", ""},
    {"floorf", "other", "int", ""},
    {"floorl", "other", "int", ""},
    {"ceil", "other", "int", ""},
    {"ceilf", "other", "int", ""},
    {"ceill", "other", "int", ""},
    {"trunc", "other", "int", ""},
    {"truncf", "other", "int", ""},
    {"truncl", "other", "int", ""},
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
    {"__errno_location", "", "static=errno nonnull", ""},
    {"__error", "", "static=errno nonnull", ""},
    {"__ctype_b_loc", "", "static=ctype nonnull", ""},
    {"__ctype_tolower_loc", "", "static=ctype nonnull", ""},
    {"__ctype_toupper_loc", "", "static=ctype nonnull", ""},
});

// Compiler builtins (Clang's Builtins.td) and the assertion-failure entry
// points of glibc, musl and macOS. Each __sync builtin also has the sized
// forms Sema resolves it to.
static constexpr auto BuiltinExpectations = std::to_array<Expectation>({
    {"__builtin_object_size", "none nullable, int", "int", ""},
    {"__builtin_dynamic_object_size", "none nullable, int", "int", ""},
    {"__builtin_expect", "int, int", "int", ""},
    {"__builtin_expect_with_probability", "int, int, other", "int", ""},
    {"__builtin_unpredictable", "int", "int", ""},
    {"__builtin_assume", "int", "void", ""},
    {"__builtin_assume_aligned", "none nullable, int, ...", "arg=0 nonnull",
     ""},
    {"__builtin_launder", "none nullable", "arg=0 nonnull", ""},
    {"__builtin_prefetch", "none nullable, ...", "void", ""},
    {"__builtin_unreachable", "", "noreturn", ""},
    {"__builtin_trap", "", "noreturn", "exits"},
    {"__builtin_verbose_trap", "R str, R str", "noreturn", "exits"},
    {"__builtin_debugtrap", "", "void", ""},
    {"__builtin_va_start", "other, ...", "void", ""},
    {"__builtin_c23_va_start", "other, ...", "void", ""},
    {"__builtin_va_end", "other", "void", ""},
    {"__builtin_va_copy", "other, other", "void", ""},
    {"__builtin_frame_address", "int", "ptr nullable", ""},
    {"__builtin_return_address", "int", "ptr nullable", ""},
    {"__builtin_extract_return_addr", "none nullable", "arg=0 nonnull", ""},
    {"__builtin___clear_cache", "none, none", "void", ""},
    {"__builtin_memcpy_inline", "W bytes=a2, R bytes=a2, int", "void",
     "disjoint=0/1/a2 copies=0/1/a2"},
    {"__builtin_memset_inline", "W bytes=a2, int, int", "void",
     "fills=0/a1/a2"},
    {"__builtin_alloca_with_align", "int, int",
     "fresh=stack nonnull extent=a0 zeroed", ""},
    {"__builtin_alloca_uninitialized", "int", "fresh=stack nonnull extent=a0",
     ""},
    {"__builtin_alloca_with_align_uninitialized", "int, int",
     "fresh=stack nonnull extent=a0", ""},
    {"__builtin_add_overflow", "other, other, W", "int", ""},
    {"__builtin_sub_overflow", "other, other, W", "int", ""},
    {"__builtin_mul_overflow", "other, other, W", "int", ""},
    {"__builtin_sadd_overflow", "int, int, W", "int", ""},
    {"__builtin_saddl_overflow", "int, int, W", "int", ""},
    {"__builtin_saddll_overflow", "int, int, W", "int", ""},
    {"__builtin_uadd_overflow", "int, int, W", "int", ""},
    {"__builtin_uaddl_overflow", "int, int, W", "int", ""},
    {"__builtin_uaddll_overflow", "int, int, W", "int", ""},
    {"__builtin_ssub_overflow", "int, int, W", "int", ""},
    {"__builtin_ssubl_overflow", "int, int, W", "int", ""},
    {"__builtin_ssubll_overflow", "int, int, W", "int", ""},
    {"__builtin_usub_overflow", "int, int, W", "int", ""},
    {"__builtin_usubl_overflow", "int, int, W", "int", ""},
    {"__builtin_usubll_overflow", "int, int, W", "int", ""},
    {"__builtin_smul_overflow", "int, int, W", "int", ""},
    {"__builtin_smull_overflow", "int, int, W", "int", ""},
    {"__builtin_smulll_overflow", "int, int, W", "int", ""},
    {"__builtin_umul_overflow", "int, int, W", "int", ""},
    {"__builtin_umull_overflow", "int, int, W", "int", ""},
    {"__builtin_umulll_overflow", "int, int, W", "int", ""},
    {"__builtin_bswap16", "int", "int", ""},
    {"__builtin_bswap32", "int", "int", ""},
    {"__builtin_bswap64", "int", "int", ""},
    {"__atomic_thread_fence", "int", "void", ""},
    {"__atomic_signal_fence", "int", "void", ""},
    {"__atomic_is_lock_free", "int, none nullable", "int", ""},
    {"__atomic_always_lock_free", "int, none nullable", "int", ""},
    {"__c11_atomic_thread_fence", "int", "void", ""},
    {"__c11_atomic_signal_fence", "int", "void", ""},
    {"__c11_atomic_is_lock_free", "int", "int", ""},
    {"__sync_fetch_and_add", "RW, int, ...", "int",
     "chk=__sync_fetch_and_add_1:0/1 chk=__sync_fetch_and_add_2:0/1 "
     "chk=__sync_fetch_and_add_4:0/1 chk=__sync_fetch_and_add_8:0/1 "
     "chk=__sync_fetch_and_add_16:0/1"},
    {"__sync_fetch_and_sub", "RW, int, ...", "int",
     "chk=__sync_fetch_and_sub_1:0/1 chk=__sync_fetch_and_sub_2:0/1 "
     "chk=__sync_fetch_and_sub_4:0/1 chk=__sync_fetch_and_sub_8:0/1 "
     "chk=__sync_fetch_and_sub_16:0/1"},
    {"__sync_fetch_and_or", "RW, int, ...", "int",
     "chk=__sync_fetch_and_or_1:0/1 chk=__sync_fetch_and_or_2:0/1 "
     "chk=__sync_fetch_and_or_4:0/1 chk=__sync_fetch_and_or_8:0/1 "
     "chk=__sync_fetch_and_or_16:0/1"},
    {"__sync_fetch_and_and", "RW, int, ...", "int",
     "chk=__sync_fetch_and_and_1:0/1 chk=__sync_fetch_and_and_2:0/1 "
     "chk=__sync_fetch_and_and_4:0/1 chk=__sync_fetch_and_and_8:0/1 "
     "chk=__sync_fetch_and_and_16:0/1"},
    {"__sync_fetch_and_xor", "RW, int, ...", "int",
     "chk=__sync_fetch_and_xor_1:0/1 chk=__sync_fetch_and_xor_2:0/1 "
     "chk=__sync_fetch_and_xor_4:0/1 chk=__sync_fetch_and_xor_8:0/1 "
     "chk=__sync_fetch_and_xor_16:0/1"},
    {"__sync_add_and_fetch", "RW, int, ...", "int",
     "chk=__sync_add_and_fetch_1:0/1 chk=__sync_add_and_fetch_2:0/1 "
     "chk=__sync_add_and_fetch_4:0/1 chk=__sync_add_and_fetch_8:0/1 "
     "chk=__sync_add_and_fetch_16:0/1"},
    {"__sync_sub_and_fetch", "RW, int, ...", "int",
     "chk=__sync_sub_and_fetch_1:0/1 chk=__sync_sub_and_fetch_2:0/1 "
     "chk=__sync_sub_and_fetch_4:0/1 chk=__sync_sub_and_fetch_8:0/1 "
     "chk=__sync_sub_and_fetch_16:0/1"},
    {"__sync_or_and_fetch", "RW, int, ...", "int",
     "chk=__sync_or_and_fetch_1:0/1 chk=__sync_or_and_fetch_2:0/1 "
     "chk=__sync_or_and_fetch_4:0/1 chk=__sync_or_and_fetch_8:0/1 "
     "chk=__sync_or_and_fetch_16:0/1"},
    {"__sync_and_and_fetch", "RW, int, ...", "int",
     "chk=__sync_and_and_fetch_1:0/1 chk=__sync_and_and_fetch_2:0/1 "
     "chk=__sync_and_and_fetch_4:0/1 chk=__sync_and_and_fetch_8:0/1 "
     "chk=__sync_and_and_fetch_16:0/1"},
    {"__sync_xor_and_fetch", "RW, int, ...", "int",
     "chk=__sync_xor_and_fetch_1:0/1 chk=__sync_xor_and_fetch_2:0/1 "
     "chk=__sync_xor_and_fetch_4:0/1 chk=__sync_xor_and_fetch_8:0/1 "
     "chk=__sync_xor_and_fetch_16:0/1"},
    {"__sync_bool_compare_and_swap", "RW, int, int, ...", "int",
     "chk=__sync_bool_compare_and_swap_1:0/1/2 "
     "chk=__sync_bool_compare_and_swap_2:0/1/2 "
     "chk=__sync_bool_compare_and_swap_4:0/1/2 "
     "chk=__sync_bool_compare_and_swap_8:0/1/2 "
     "chk=__sync_bool_compare_and_swap_16:0/1/2"},
    {"__sync_val_compare_and_swap", "RW, int, int, ...", "int",
     "chk=__sync_val_compare_and_swap_1:0/1/2 "
     "chk=__sync_val_compare_and_swap_2:0/1/2 "
     "chk=__sync_val_compare_and_swap_4:0/1/2 "
     "chk=__sync_val_compare_and_swap_8:0/1/2 "
     "chk=__sync_val_compare_and_swap_16:0/1/2"},
    {"__sync_lock_test_and_set", "RW, int, ...", "int",
     "chk=__sync_lock_test_and_set_1:0/1 chk=__sync_lock_test_and_set_2:0/1 "
     "chk=__sync_lock_test_and_set_4:0/1 chk=__sync_lock_test_and_set_8:0/1 "
     "chk=__sync_lock_test_and_set_16:0/1"},
    {"__sync_lock_release", "W, ...", "void",
     "chk=__sync_lock_release_1:0 chk=__sync_lock_release_2:0 "
     "chk=__sync_lock_release_4:0 chk=__sync_lock_release_8:0 "
     "chk=__sync_lock_release_16:0"},
    {"__sync_synchronize", "", "void", ""},
    {"__assert_fail", "R str, R str, int, R str nullable", "noreturn", "exits"},
    {"__assert_perror_fail", "int, R str, int, R str nullable", "noreturn",
     "exits"},
    {"__assert_rtn", "R str nullable, R str, int, R str", "noreturn", "exits"},
    {"__assert", "R str, R str, int", "noreturn", "exits"},
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
      EXPECT_EQ(renderValue(entry.result, entry.noreturn), expectation.result)
          << key;
      EXPECT_EQ(renderClauses(entry), expectation.clauses) << key;
    }
  }
  for (const auto &[key, entry] : rows)
    EXPECT_TRUE(expected.contains(key)) << "row without an expectation: " << key
                                        << " (line " << entry->line << ")";
}

} // namespace weavec::core
