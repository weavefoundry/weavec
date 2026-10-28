//===- LibrarySpec.cpp - The C library table of the guards ----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0036 §6. The parser reads the `LibrarySpec.txt` syntax documented at
// the top of that file: comments and continuations are removed first, giving
// logical lines that each hold one entry; each entry is parsed by recursive
// descent and validated on its own, and the aliases are indexed last.
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/LibrarySpec.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <set>
#include <utility>

namespace weavec::core {

//===----------------------------------------------------------------------===//
// LibTerm
//===----------------------------------------------------------------------===//

LibTerm LibTerm::constant(std::int64_t value) {
  return LibTerm{.kind = Kind::Constant, .value = value};
}

LibTerm LibTerm::argument(unsigned index) {
  return LibTerm{.kind = Kind::Argument,
                 .arg = static_cast<std::uint8_t>(index)};
}

LibTerm LibTerm::stringLength(unsigned index) {
  return LibTerm{.kind = Kind::StringLength,
                 .arg = static_cast<std::uint8_t>(index)};
}

static LibTerm binary(LibTerm::Kind kind, LibTerm left, LibTerm right) {
  LibTerm term{.kind = kind};
  term.operands.push_back(std::move(left));
  term.operands.push_back(std::move(right));
  return term;
}

LibTerm LibTerm::product(LibTerm left, LibTerm right) {
  return binary(Kind::Product, std::move(left), std::move(right));
}

LibTerm LibTerm::sum(LibTerm left, LibTerm right) {
  return binary(Kind::Sum, std::move(left), std::move(right));
}

LibTerm LibTerm::difference(LibTerm left, std::int64_t right) {
  LibTerm term{.kind = Kind::Difference, .value = right};
  term.operands.push_back(std::move(left));
  return term;
}

LibTerm LibTerm::min(LibTerm left, LibTerm right) {
  return binary(Kind::Min, std::move(left), std::move(right));
}

LibTerm LibTerm::quotient(LibTerm left, std::int64_t divisor) {
  LibTerm term{.kind = Kind::Quotient, .value = divisor};
  term.operands.push_back(std::move(left));
  return term;
}

static bool isAdditive(const LibTerm &term) {
  return term.kind == LibTerm::Kind::Sum ||
         term.kind == LibTerm::Kind::Difference;
}

static std::string parenthesised(const LibTerm &term, bool needed) {
  return needed ? "(" + term.str() + ")" : term.str();
}

std::string LibTerm::str() const {
  switch (kind) {
  case Kind::Constant:
    return std::to_string(value);
  case Kind::Argument:
    return "a" + std::to_string(arg);
  case Kind::StringLength:
    return "strlen(a" + std::to_string(arg) + ")";
  case Kind::Product:
    return parenthesised(operands[0], isAdditive(operands[0])) + "*" +
           parenthesised(operands[1], isAdditive(operands[1]) ||
                                          operands[1].kind == Kind::Product);
  case Kind::Sum:
    return operands[0].str() + "+" +
           parenthesised(operands[1], isAdditive(operands[1]));
  case Kind::Difference:
    return operands[0].str() + "-" + std::to_string(value);
  case Kind::Min:
    return "min(" + operands[0].str() + "," + operands[1].str() + ")";
  case Kind::Quotient:
    return parenthesised(operands[0], isAdditive(operands[0])) + "/" +
           std::to_string(value);
  }
  return {};
}

static std::optional<std::int64_t>
ask(const std::function<std::optional<std::int64_t>(unsigned)> &function,
    unsigned index) {
  return function ? function(index) : std::nullopt;
}

static std::optional<std::int64_t> checkedAdd(std::int64_t left,
                                              std::int64_t right) {
  constexpr auto Max = std::numeric_limits<std::int64_t>::max();
  constexpr auto Min = std::numeric_limits<std::int64_t>::min();
  if ((right > 0 && left > Max - right) || (right < 0 && left < Min - right))
    return std::nullopt;
  return left + right;
}

static std::optional<std::int64_t> checkedMultiply(std::int64_t left,
                                                   std::int64_t right) {
  if (left == 0 || right == 0)
    return 0;
  constexpr auto Max = std::numeric_limits<std::int64_t>::max();
  constexpr auto Min = std::numeric_limits<std::int64_t>::min();
  // Decide overflow before multiplying: signed overflow is undefined.
  bool overflows = false;
  if (left > 0)
    overflows = right > 0 ? left > Max / right : right < Min / left;
  else
    overflows = right > 0 ? left < Min / right : left < Max / right;
  if (overflows)
    return std::nullopt;
  return left * right;
}

std::optional<std::int64_t> LibTerm::evaluate(const Values &values) const {
  switch (kind) {
  case Kind::Constant:
    return value;
  case Kind::Argument:
    return ask(values.argument, arg);
  case Kind::StringLength:
    return ask(values.stringLength, arg);
  case Kind::Difference: {
    const auto left = operands[0].evaluate(values);
    if (!left || value == std::numeric_limits<std::int64_t>::min())
      return std::nullopt;
    return checkedAdd(*left, -value);
  }
  case Kind::Quotient: {
    const auto left = operands[0].evaluate(values);
    if (!left || value <= 0)
      return std::nullopt;
    // Rounded down, as a mathematical integer.
    return *left >= 0 ? *left / value : -((-*left + value - 1) / value);
  }
  case Kind::Product:
  case Kind::Sum:
  case Kind::Min: {
    const auto left = operands[0].evaluate(values);
    const auto right = operands[1].evaluate(values);
    if (!left || !right)
      return std::nullopt;
    if (kind == Kind::Min)
      return std::min(*left, *right);
    return kind == Kind::Sum ? checkedAdd(*left, *right)
                             : checkedMultiply(*left, *right);
  }
  }
  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// Entries and matches
//===----------------------------------------------------------------------===//

const LibraryParam *LibraryEntry::param(unsigned index) const noexcept {
  return index < params.size() ? &params[index] : nullptr;
}

int LibraryMatch::rowArgument(unsigned callArgument) const noexcept {
  if (alias == nullptr)
    return static_cast<int>(callArgument);
  const std::vector<std::int8_t> &map = alias->argumentOf;
  if (callArgument < map.size())
    return map[callArgument];
  if (entry == nullptr || !entry->variadic || map.empty() || map.back() < 0)
    return -1;
  return map.back() + static_cast<int>(callArgument - map.size()) + 1;
}

int LibraryMatch::callArgument(unsigned rowArg) const noexcept {
  if (alias == nullptr)
    return static_cast<int>(rowArg);
  const std::vector<std::int8_t> &map = alias->argumentOf;
  for (std::size_t i = 0; i < map.size(); ++i)
    if (std::cmp_equal(map[i], rowArg))
      return static_cast<int>(i);
  if (entry == nullptr || !entry->variadic || map.empty() || map.back() < 0 ||
      std::cmp_less_equal(rowArg, map.back()))
    return -1;
  return static_cast<int>(map.size()) - 1 +
         (static_cast<int>(rowArg) - map.back());
}

//===----------------------------------------------------------------------===//
// Lexical helpers
//===----------------------------------------------------------------------===//

namespace {
/// One logical line: an entry, with comments removed and continuations
/// joined, and the physical line of each character.
struct LogicalLine {
  std::string text;
  std::vector<unsigned> lines;
};
} // namespace

static std::vector<LogicalLine> logicalLines(std::string_view text) {
  std::vector<LogicalLine> result;
  LogicalLine current;
  unsigned number = 0;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos)
      end = text.size();
    ++number;
    std::string_view physical = text.substr(start, end - start);
    if (const auto hash = physical.find('#'); hash != std::string_view::npos)
      physical = physical.substr(0, hash);
    while (!physical.empty() &&
           (physical.back() == ' ' || physical.back() == '\t' ||
            physical.back() == '\r'))
      physical.remove_suffix(1);
    const bool continues = !physical.empty() && physical.back() == '\\';
    if (continues)
      physical.remove_suffix(1);
    for (const char c : physical) {
      current.text += c == '\t' ? ' ' : c;
      current.lines.push_back(number);
    }
    if (continues) {
      current.text += ' ';
      current.lines.push_back(number);
    } else {
      result.push_back(std::move(current));
      current = {};
    }
    if (end == text.size())
      break;
    start = end + 1;
  }
  if (!current.text.empty())
    result.push_back(std::move(current));
  return result;
}

static bool isDigit(char c) {
  return c >= '0' && c <= '9';
}
static bool isIdentifierStart(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static bool isIdentifierChar(char c) {
  return isIdentifierStart(c) || isDigit(c);
}

static bool isIdentifier(std::string_view text) {
  return !text.empty() && isIdentifierStart(text.front()) &&
         std::ranges::all_of(text, isIdentifierChar);
}

namespace {
/// Reads terms from `text` starting at `pos`, advancing `pos`.
class TermReader {
public:
  TermReader(std::string_view text, std::size_t &pos) : text(text), pos(pos) {}

  std::optional<LibTerm> read() { return sum(); }
  [[nodiscard]] const std::string &error() const { return message; }

private:
  std::string_view text;
  std::size_t &pos;
  std::string message;

  void skipSpace() {
    while (pos < text.size() && text[pos] == ' ')
      ++pos;
  }
  bool accept(std::string_view token) {
    skipSpace();
    if (text.substr(pos).starts_with(token)) {
      pos += token.size();
      return true;
    }
    return false;
  }
  std::optional<LibTerm> fail(std::string why) {
    if (message.empty())
      message = std::move(why);
    return std::nullopt;
  }
  std::optional<std::int64_t> number() {
    skipSpace();
    if (pos >= text.size() || !isDigit(text[pos]))
      return std::nullopt;
    std::int64_t value = 0;
    while (pos < text.size() && isDigit(text[pos])) {
      const auto digit = static_cast<std::int64_t>(text[pos++] - '0');
      if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10)
        return std::nullopt;
      value = (value * 10) + digit;
    }
    return value;
  }
  std::optional<unsigned> argumentIndex() {
    if (!accept("a"))
      return std::nullopt;
    const auto index = number();
    if (!index || *index > 255)
      return std::nullopt;
    return static_cast<unsigned>(*index);
  }
  std::optional<LibTerm> sum() {
    auto left = product();
    while (left) {
      if (accept("+")) {
        auto right = product();
        if (!right)
          return std::nullopt;
        left = LibTerm::sum(std::move(*left), std::move(*right));
      } else if (accept("-")) {
        const auto right = number();
        if (!right)
          return fail("expected an integer after '-' in a term");
        left = LibTerm::difference(std::move(*left), *right);
      } else {
        break;
      }
    }
    return left;
  }
  std::optional<LibTerm> product() {
    auto left = atom();
    while (left) {
      if (accept("*")) {
        auto right = atom();
        if (!right)
          return std::nullopt;
        left = LibTerm::product(std::move(*left), std::move(*right));
      } else if (accept("/")) {
        const auto right = number();
        if (!right || *right <= 0)
          return fail("expected a positive integer after '/' in a term");
        left = LibTerm::quotient(std::move(*left), *right);
      } else {
        break;
      }
    }
    return left;
  }
  std::optional<LibTerm> atom() {
    skipSpace();
    if (pos >= text.size())
      return fail("expected a term");
    if (isDigit(text[pos])) {
      const auto value = number();
      if (!value)
        return fail("integer out of range in a term");
      return LibTerm::constant(*value);
    }
    if (accept("(")) {
      auto inner = sum();
      if (inner && !accept(")"))
        return fail("expected ')' in a term");
      return inner;
    }
    if (accept("strlen(")) {
      const auto index = argumentIndex();
      if (!index || !accept(")"))
        return fail("expected 'strlen(aN)'");
      return LibTerm::stringLength(*index);
    }
    if (accept("min(")) {
      auto left = sum();
      if (!left)
        return std::nullopt;
      if (!accept(","))
        return fail("expected ',' in 'min(…)'");
      auto right = sum();
      if (!right)
        return std::nullopt;
      if (!accept(")"))
        return fail("expected ')' after 'min(…'");
      return LibTerm::min(std::move(*left), std::move(*right));
    }
    if (text[pos] == 'a' && pos + 1 < text.size() && isDigit(text[pos + 1])) {
      const auto index = argumentIndex();
      if (!index)
        return fail("argument index out of range in a term");
      return LibTerm::argument(*index);
    }
    return fail("expected a term");
  }
};
} // namespace

std::optional<LibTerm> LibTerm::parse(std::string_view text,
                                      std::string &error) {
  std::size_t pos = 0;
  TermReader reader(text, pos);
  auto term = reader.read();
  while (pos < text.size() && text[pos] == ' ')
    ++pos;
  if (term && pos != text.size()) {
    error = "unexpected '" + std::string(text.substr(pos)) + "' after a term";
    return std::nullopt;
  }
  if (!term)
    error = reader.error().empty() ? "expected a term" : reader.error();
  return term;
}

//===----------------------------------------------------------------------===//
// The parser
//===----------------------------------------------------------------------===//

class LibrarySpecParser {
public:
  explicit LibrarySpecParser(LibrarySpec &spec) : spec(spec) {}

  bool parse(std::string_view text);
  [[nodiscard]] const std::string &error() const { return message; }

private:
  LibrarySpec &spec;
  std::string message;
  const LogicalLine *line = nullptr;
  std::size_t pos = 0;
  /// The entry being parsed, for messages.
  std::string entryName;

  bool fail(const std::string &why) {
    if (!message.empty())
      return false;
    unsigned number = 0;
    if (line != nullptr && !line->lines.empty())
      number = line->lines[std::min(pos, line->lines.size() - 1)];
    message = "LibrarySpec.txt:" + std::to_string(number) + ": ";
    if (!entryName.empty())
      message += "'" + entryName + "': ";
    message += why;
    return false;
  }
  void skipSpace() {
    while (pos < line->text.size() && line->text[pos] == ' ')
      ++pos;
  }
  bool atEnd() {
    skipSpace();
    return pos >= line->text.size();
  }
  bool accept(char c) {
    skipSpace();
    if (pos >= line->text.size() || line->text[pos] != c)
      return false;
    ++pos;
    return true;
  }
  bool accept(std::string_view token) {
    skipSpace();
    if (!std::string_view(line->text).substr(pos).starts_with(token))
      return false;
    pos += token.size();
    return true;
  }
  bool expect(char c, std::string_view context) {
    if (accept(c))
      return true;
    return fail("expected '" + std::string(1, c) + "' " + std::string(context));
  }
  /// A keyword or flag: `[A-Za-z0-9_-]+`.
  std::string word() {
    skipSpace();
    const std::size_t begin = pos;
    while (pos < line->text.size() &&
           (isIdentifierChar(line->text[pos]) || line->text[pos] == '-'))
      ++pos;
    return line->text.substr(begin, pos - begin);
  }
  std::optional<std::string> name(std::string_view what) {
    std::string text = word();
    if (!isIdentifier(text)) {
      fail("expected " + std::string(what) +
           (text.empty() ? "" : ", found '" + text + "'"));
      return std::nullopt;
    }
    return text;
  }
  std::optional<std::uint8_t> index(std::string_view what) {
    skipSpace();
    const std::size_t begin = pos;
    unsigned value = 0;
    while (pos < line->text.size() && isDigit(line->text[pos]) && value < 1000)
      value = (value * 10) + static_cast<unsigned>(line->text[pos++] - '0');
    if (pos == begin) {
      fail("expected " + std::string(what));
      return std::nullopt;
    }
    if (value > 255) {
      fail(std::string(what) + " out of range");
      return std::nullopt;
    }
    return static_cast<std::uint8_t>(value);
  }
  std::optional<LibTerm> term(std::string_view where) {
    TermReader reader(line->text, pos);
    auto result = reader.read();
    if (!result)
      fail(reader.error() + " in '" + std::string(where) + "(…)'");
    return result;
  }
  std::optional<LibTerm> parenthesisedTerm(std::string_view flag) {
    if (!expect('(', "after '" + std::string(flag) + "'"))
      return std::nullopt;
    auto result = term(flag);
    if (!result ||
        !expect(')', "after the term of '" + std::string(flag) + "'"))
      return std::nullopt;
    return result;
  }

  bool parseEntry();
  std::optional<LibraryParam> parseParam();
  bool parseResult(LibraryEntry &entry);
  bool parseClause(LibraryEntry &entry, const std::string &clause);
  bool validateTerm(const LibraryEntry &entry, const LibTerm &value,
                    std::string_view where);
  bool validateEntry(const LibraryEntry &entry);
};

std::optional<LibraryParam> LibrarySpecParser::parseParam() {
  LibraryParam param;
  const std::string head = word();
  if (head == "int") {
    param.type = LibraryParam::Type::Int;
    return param;
  }
  if (head == "other") {
    param.type = LibraryParam::Type::Other;
    return param;
  }
  if (head == "fn") {
    param.type = LibraryParam::Type::Function;
    return param;
  }
  param.type = LibraryParam::Type::Pointer;
  if (head == "r") {
    param.access = LibraryParam::Access::Read;
  } else if (head == "w") {
    param.access = LibraryParam::Access::Write;
  } else if (head == "rw") {
    param.access = LibraryParam::Access::ReadWrite;
  } else if (head == "none") {
    param.access = LibraryParam::Access::None;
  } else {
    fail("expected a parameter, found '" + head + "'");
    return std::nullopt;
  }
  std::set<std::string> seen;
  while (accept(':')) {
    const std::string flag = word();
    if (!seen.insert(flag).second) {
      fail("duplicate flag '" + flag + "'");
      return std::nullopt;
    }
    if (flag == "bytes" || flag == "count") {
      auto value = parenthesisedTerm(flag);
      if (!value)
        return std::nullopt;
      (flag == "bytes" ? param.bytes : param.count) = std::move(*value);
    } else if (flag == "str") {
      param.string = true;
    } else if (flag == "null-ok") {
      param.null = LibraryParam::Null::Allowed;
    } else if (flag == "null-if-zero") {
      auto value = parenthesisedTerm(flag);
      if (!value)
        return std::nullopt;
      param.null = LibraryParam::Null::AllowedIfZero;
      param.zeroTerm = std::move(*value);
    } else if (flag == "release") {
      param.effect = LibraryParam::Effect::Release;
    } else if (flag == "realloc") {
      param.effect = LibraryParam::Effect::Realloc;
    } else {
      fail("unknown parameter flag '" + flag + "'");
      return std::nullopt;
    }
  }
  if (seen.contains("null-ok") && seen.contains("null-if-zero")) {
    fail("'null-ok' and 'null-if-zero' exclude each other");
    return std::nullopt;
  }
  if (param.bytes && param.count) {
    fail("'bytes' and 'count' exclude each other");
    return std::nullopt;
  }
  if (param.string && param.access != LibraryParam::Access::Read &&
      param.access != LibraryParam::Access::ReadWrite) {
    fail("'str' needs 'r' or 'rw'");
    return std::nullopt;
  }
  return param;
}

bool LibrarySpecParser::parseResult(LibraryEntry &entry) {
  const std::string kind = word();
  if (kind == "void") {
    entry.result = LibraryEntry::Result::Void;
  } else if (kind == "ptr") {
    entry.result = LibraryEntry::Result::Pointer;
  } else if (kind == "int") {
    entry.result = LibraryEntry::Result::Int;
    if (accept(':')) {
      if (word() != "value")
        return fail("expected 'value(…)' after 'int:'");
      entry.value = parenthesisedTerm("value");
      if (!entry.value)
        return false;
    }
  } else {
    return fail("expected 'void', 'int' or 'ptr', found '" + kind + "'");
  }
  return true;
}

bool LibrarySpecParser::parseClause(LibraryEntry &entry,
                                    const std::string &clause) {
  if (clause == "wide") {
    if (entry.wide)
      return fail("duplicate clause 'wide'");
    entry.wide = true;
    return true;
  }
  if (clause == "disjoint") {
    if (!expect('(', "after 'disjoint'"))
      return false;
    auto first = index("an argument index");
    if (!first || !expect(',', "in 'disjoint'"))
      return false;
    auto second = index("an argument index");
    if (!second || !expect(',', "in 'disjoint'"))
      return false;
    auto length = term("disjoint");
    if (!length || !expect(')', "after 'disjoint(…'"))
      return false;
    entry.disjoint.push_back(LibDisjoint{
        .first = *first, .second = *second, .length = std::move(*length)});
    return true;
  }
  if (clause == "printf" || clause == "scanf") {
    if (entry.format)
      return fail("a row has at most one format clause");
    if (!expect('(', "after '" + clause + "'"))
      return false;
    auto format = index("the format's index");
    if (!format || !expect(',', "in '" + clause + "'"))
      return false;
    auto first = index("the first argument's index");
    if (!first || !expect(')', "after '" + clause + "(…'"))
      return false;
    entry.format =
        LibFormat{.kind = clause == "printf" ? LibFormat::Kind::Printf
                                             : LibFormat::Kind::Scanf,
                  .format = *format,
                  .first = *first,
                  .vaList = !entry.variadic};
    return true;
  }
  if (clause == "wrapper") {
    if (entry.wrapper)
      return fail("duplicate clause 'wrapper'");
    if (!expect('(', "after 'wrapper'"))
      return false;
    auto wrapped = name("a wrapper name");
    if (!wrapped || !expect(')', "after the wrapper name"))
      return false;
    entry.wrapper = LibWrapper{.name = std::move(*wrapped)};
    return true;
  }
  if (clause == "chk") {
    if (!expect('(', "after 'chk'"))
      return false;
    auto alias = name("an alias name");
    if (!alias || !expect(':', "after the alias name"))
      return false;
    LibraryChk chk{.name = std::move(*alias)};
    do {
      if (accept("size")) {
        if (chk.sizeArgument >= 0)
          return fail("an alias has at most one 'size' argument");
        chk.sizeArgument = static_cast<int>(chk.argumentOf.size());
        chk.argumentOf.push_back(-1);
      } else if (accept('_')) {
        chk.argumentOf.push_back(-1);
      } else {
        auto row = index("a row argument index, '_' or 'size'");
        if (!row)
          return false;
        if (*row > 127)
          return fail("alias argument index out of range");
        chk.argumentOf.push_back(static_cast<std::int8_t>(*row));
      }
    } while (accept(','));
    if (!expect(')', "after the alias's arguments"))
      return false;
    entry.chk.push_back(std::move(chk));
    return true;
  }
  return fail("unknown clause '" + clause + "'");
}

bool LibrarySpecParser::validateTerm(const LibraryEntry &entry,
                                     const LibTerm &value,
                                     std::string_view where) {
  if ((value.kind == LibTerm::Kind::Argument ||
       value.kind == LibTerm::Kind::StringLength) &&
      value.arg >= entry.params.size())
    return fail("argument a" + std::to_string(value.arg) + " of " +
                std::string(where) + " is past the row's parameters");
  if (value.kind == LibTerm::Kind::StringLength &&
      entry.params[value.arg].type != LibraryParam::Type::Pointer)
    return fail("strlen(a" + std::to_string(value.arg) + ") of " +
                std::string(where) + " names a non-pointer");
  return std::ranges::all_of(value.operands, [&](const LibTerm &operand) {
    return validateTerm(entry, operand, where);
  });
}

bool LibrarySpecParser::validateEntry(const LibraryEntry &entry) {
  for (std::size_t i = 0; i < entry.params.size(); ++i) {
    const LibraryParam &param = entry.params[i];
    const std::string where = "parameter " + std::to_string(i);
    for (const std::optional<LibTerm> *value :
         {&param.bytes, &param.count, &param.zeroTerm})
      if (*value && !validateTerm(entry, **value, where))
        return false;
    if (param.count && !entry.wide)
      return fail("'count' needs a wide row");
  }
  if (entry.value && !validateTerm(entry, *entry.value, "the result"))
    return false;
  for (const LibDisjoint &disjoint : entry.disjoint)
    if (disjoint.first >= entry.params.size() ||
        disjoint.second >= entry.params.size() ||
        !validateTerm(entry, disjoint.length, "'disjoint'"))
      return fail("'disjoint' names an argument past the row's parameters");
  if (entry.format &&
      (entry.format->format >= entry.params.size() ||
       (!entry.variadic && entry.format->first >= entry.params.size())))
    return fail("the format clause names an argument past the parameters");
  // The runtime's wrapper is the row's own: a row that redirects to
  // another's would check that row's arguments.
  if (entry.wrapper && entry.wrapper->name != entry.name)
    return fail("'wrapper(" + entry.wrapper->name + ")' must name the row");
  for (const LibraryChk &chk : entry.chk)
    for (const std::int8_t row : chk.argumentOf)
      if (row >= 0 && !entry.variadic &&
          std::cmp_greater_equal(row, entry.params.size()))
        return fail("alias '" + chk.name +
                    "' names an argument past the row's parameters");
  return true;
}

bool LibrarySpecParser::parseEntry() {
  LibraryEntry entry;
  entry.line = line->lines.empty() ? 0 : line->lines.front();
  auto rowName = name("a function name");
  if (!rowName)
    return false;
  entry.name = *rowName;
  entryName = entry.name;
  if (!expect('(', "after the function name"))
    return false;
  if (!accept(')')) {
    do {
      if (accept("...")) {
        entry.variadic = true;
        break;
      }
      auto param = parseParam();
      if (!param)
        return false;
      entry.params.push_back(std::move(*param));
    } while (accept(','));
    if (!expect(')', "after the parameters"))
      return false;
  }
  if (!accept("->"))
    return fail("expected '->' after the parameters");
  if (!parseResult(entry))
    return false;
  while (!accept(';')) {
    if (atEnd())
      return fail("expected ';' at the end of the entry");
    const std::string clause = word();
    if (clause.empty())
      return fail("expected a clause or ';'");
    if (!parseClause(entry, clause))
      return false;
  }
  if (!atEnd())
    return fail("unexpected text after ';'");
  if (!validateEntry(entry))
    return false;
  spec.byName.emplace(entry.name, spec.rows.size());
  spec.rows.push_back(std::move(entry));
  entryName.clear();
  return true;
}

bool LibrarySpecParser::parse(std::string_view text) {
  const std::vector<LogicalLine> lines = logicalLines(text);
  for (const LogicalLine &logical : lines) {
    line = &logical;
    pos = 0;
    if (atEnd())
      continue;
    if (!parseEntry())
      return false;
  }
  line = nullptr;
  for (std::size_t row = 0; row < spec.rows.size(); ++row)
    for (std::size_t alias = 0; alias < spec.rows[row].chk.size(); ++alias) {
      const std::string &name = spec.rows[row].chk[alias].name;
      if (spec.byName.contains(name) || spec.byAlias.contains(name)) {
        entryName = spec.rows[row].name;
        return fail("alias '" + name + "' is already a row or an alias");
      }
      spec.byAlias.emplace(name, std::make_pair(row, alias));
    }
  return true;
}

//===----------------------------------------------------------------------===//
// LibrarySpec
//===----------------------------------------------------------------------===//

std::optional<LibrarySpec> LibrarySpec::parse(std::string_view text,
                                              std::string &error) {
  LibrarySpec spec;
  LibrarySpecParser parser(spec);
  if (!parser.parse(text)) {
    error = parser.error();
    return std::nullopt;
  }
  return spec;
}

const LibraryEntry *LibrarySpec::find(std::string_view name) const {
  const auto found = byName.find(name);
  return found == byName.end() ? nullptr : &rows[found->second];
}

/// Whether a call of `arguments` arguments fits `entry`.
static bool fits(const LibraryEntry &entry, unsigned arguments) {
  return entry.variadic ? arguments >= entry.params.size()
                        : arguments == entry.params.size();
}

std::optional<LibraryMatch>
LibrarySpec::lookup(std::string_view callee,
                    std::optional<unsigned> arguments) const {
  for (const bool builtin : {false, true}) {
    std::string_view name = callee;
    if (builtin) {
      if (!name.starts_with("__builtin_"))
        break;
      name.remove_prefix(sizeof("__builtin_") - 1);
    }
    if (const auto found = byAlias.find(name); found != byAlias.end()) {
      const LibraryEntry &entry = rows[found->second.first];
      return LibraryMatch{.entry = &entry,
                          .alias = &entry.chk[found->second.second]};
    }
    const auto [first, last] = byName.equal_range(name);
    if (first == last)
      continue;
    if (arguments)
      for (auto it = first; it != last; ++it)
        if (fits(rows[it->second], *arguments))
          return LibraryMatch{.entry = &rows[it->second]};
    return LibraryMatch{.entry = &rows[first->second]};
  }
  return std::nullopt;
}

// The table text, as `LibrarySpecTextData` / `LibrarySpecTextSize`; generated
// from LibrarySpec.txt by lib/Core/CMakeLists.txt.
#include "LibrarySpecText.inc"

std::string_view LibrarySpec::shippedText() {
  return {LibrarySpecTextData, LibrarySpecTextSize};
}

const LibrarySpec &LibrarySpec::shipped() {
  static const LibrarySpec Shipped = [] {
    std::string error;
    return parse(shippedText(), error).value_or(LibrarySpec{});
  }();
  return Shipped;
}

} // namespace weavec::core
