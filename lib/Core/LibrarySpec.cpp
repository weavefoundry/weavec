//===- LibrarySpec.cpp - The declarative C library table ------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §8. The parser reads the `LibrarySpec.txt` syntax documented at
// the top of that file: comments and continuations are removed first, giving
// logical lines that each hold one directive or one entry; each entry is then
// parsed by recursive descent and validated on its own, and the table as a
// whole is validated last (overloads, aliases).
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/LibrarySpec.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
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

LibTerm LibTerm::formatLength(unsigned index) {
  return LibTerm{.kind = Kind::FormatLength,
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
  case Kind::FormatLength:
    return "fmtlen(a" + std::to_string(arg) + ")";
  case Kind::Macro:
    return macro;
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
  }
  return {};
}

bool LibTerm::mentions(Kind wanted) const {
  return kind == wanted ||
         std::ranges::any_of(operands, [wanted](const LibTerm &operand) {
           return operand.mentions(wanted);
         });
}

static void collectArguments(const LibTerm &term,
                             std::vector<unsigned> &found) {
  const bool leaf = term.kind == LibTerm::Kind::Argument ||
                    term.kind == LibTerm::Kind::StringLength ||
                    term.kind == LibTerm::Kind::FormatLength;
  if (leaf && std::ranges::find(found, term.arg) == found.end())
    found.push_back(term.arg);
  for (const LibTerm &operand : term.operands)
    collectArguments(operand, found);
}

std::vector<unsigned> LibTerm::arguments() const {
  std::vector<unsigned> found;
  collectArguments(*this, found);
  return found;
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
  case Kind::FormatLength:
    return ask(values.formatLength, arg);
  case Kind::Macro:
    return values.macro ? values.macro(macro) : std::nullopt;
  case Kind::Difference: {
    const auto left = operands[0].evaluate(values);
    if (!left || value == std::numeric_limits<std::int64_t>::min())
      return std::nullopt;
    return checkedAdd(*left, -value);
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
// Results, entries and matches
//===----------------------------------------------------------------------===//

bool LibraryResult::isPointer() const noexcept {
  return kind != Kind::Void && kind != Kind::Int;
}

const LibraryParam *LibraryEntry::param(unsigned index) const noexcept {
  return index < params.size() ? &params[index] : nullptr;
}

bool LibraryEntry::allocates() const noexcept {
  if (result.kind == LibraryResult::Kind::Fresh ||
      (result.kind == LibraryResult::Kind::Arg && !result.family.empty()))
    return true;
  return std::ranges::any_of(params, [](const LibraryParam &p) {
    return p.out && p.out->kind == LibraryResult::Kind::Fresh;
  });
}

bool LibraryEntry::releases() const noexcept {
  return std::ranges::any_of(params, [](const LibraryParam &p) {
    return p.effect == LibraryParam::Effect::Release ||
           p.effect == LibraryParam::Effect::Realloc;
  });
}

bool LibraryEntry::hasCallback() const noexcept {
  return std::ranges::any_of(
      params, [](const LibraryParam &p) { return p.callback.has_value(); });
}

bool LibraryEntry::knownToReturn() const noexcept {
  return !noreturn && !exits;
}

bool LibraryEntry::isCompilerBuiltin() const noexcept {
  return header.empty();
}

bool LibraryEntry::trustsLibrarySpec() const noexcept {
  const auto hidden = [](const LibraryResult &value) {
    return value.kind == LibraryResult::Kind::Static ||
           value.kind == LibraryResult::Kind::InteriorState ||
           !value.state.empty();
  };
  if (hidden(result) || !invalidates.empty() || !reads.empty())
    return true;
  return std::ranges::any_of(params, [&hidden](const LibraryParam &p) {
    return p.effect == LibraryParam::Effect::Retain || p.callback ||
           (p.out && hidden(*p.out));
  });
}

LibSignature LibraryEntry::signature() const {
  LibSignature signature{.variadic = variadic,
                         .pointerResult = result.isPointer()};
  for (const LibraryParam &p : params)
    signature.params.push_back(p.type);
  return signature;
}

bool LibraryEntry::accepts(const LibSignature &declared) const {
  if (declared.params.size() != params.size() ||
      declared.variadic != variadic ||
      declared.pointerResult != result.isPointer())
    return false;
  for (std::size_t i = 0; i < params.size(); ++i) {
    const LibraryParam::Type row = params[i].type;
    if (row != LibraryParam::Type::Other && row != declared.params[i])
      return false;
  }
  return true;
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

const LibraryParam *LibraryMatch::param(unsigned callArgument) const noexcept {
  const int row = rowArgument(callArgument);
  return entry == nullptr || row < 0 ? nullptr
                                     : entry->param(static_cast<unsigned>(row));
}

//===----------------------------------------------------------------------===//
// Canonical spelling
//===----------------------------------------------------------------------===//

static std::string callbackText(const LibCallback &callback) {
  std::string text;
  switch (callback.kind) {
  case LibCallback::Kind::Sync:
    text = "sync(";
    break;
  case LibCallback::Kind::Entry:
    text = "entry(";
    break;
  case LibCallback::Kind::AtExit:
    return "at-exit";
  }
  for (std::size_t i = 0; i < callback.arguments.size(); ++i)
    text += (i != 0 ? "," : "") + std::to_string(callback.arguments[i]);
  return text + ")";
}

/// The nullability a result kind has when the row does not say.
static LibraryResult::Null defaultNull(LibraryResult::Kind kind) {
  switch (kind) {
  case LibraryResult::Kind::Fresh:
    return LibraryResult::Null::OnFailure;
  case LibraryResult::Kind::Interior:
  case LibraryResult::Kind::InteriorState:
  case LibraryResult::Kind::Unknown:
    return LibraryResult::Null::May;
  case LibraryResult::Kind::Void:
  case LibraryResult::Kind::Int:
  case LibraryResult::Kind::Static:
  case LibraryResult::Kind::Arg:
    return LibraryResult::Null::Never;
  }
  return LibraryResult::Null::Never;
}

static std::string resultText(const LibraryResult &result, bool noreturn) {
  using Kind = LibraryResult::Kind;
  std::string text;
  switch (result.kind) {
  case Kind::Void:
    return noreturn ? "noreturn" : "void";
  case Kind::Int:
    return result.value ? "int:value(" + result.value->str() + ")" : "int";
  case Kind::Fresh:
    text = "fresh(" + result.family + ")";
    break;
  case Kind::Static:
    text = "static(" + result.state + ")";
    break;
  case Kind::Arg:
    text = "arg(" + std::to_string(result.arg) + ")";
    break;
  case Kind::Interior:
    text = "interior(" + std::to_string(result.arg) + ")";
    break;
  case Kind::InteriorState:
    text = "interior-state(" + result.state + ")";
    break;
  case Kind::Unknown:
    text = "ptr";
    break;
  }
  if (result.extent)
    text += ":extent(" + result.extent->str() + ")";
  if (result.offset)
    text += ":offset(" + result.offset->str() + ")";
  if (result.null != defaultNull(result.kind)) {
    switch (result.null) {
    case LibraryResult::Null::Never:
      text += ":nonnull";
      break;
    case LibraryResult::Null::OnFailure:
      text += ":null-on-failure";
      break;
    case LibraryResult::Null::May:
      text += ":null-ok";
      break;
    }
  }
  if (result.kind == Kind::Arg && !result.family.empty())
    text += ":or-fresh(" + result.family + ")";
  if (result.kind == Kind::Arg && !result.state.empty())
    text += ":or-static(" + result.state + ")";
  if (result.zeroInit)
    text += ":zero-init";
  else if (result.family == HeapFamily)
    text += ":no-zero-init";
  if (result.zeroFilled)
    text += ":zero-filled";
  if (result.string)
    text += ":str";
  if (result.replaces)
    text += ":replaces";
  return text;
}

static std::string_view accessText(LibraryParam::Access access) {
  switch (access) {
  case LibraryParam::Access::None:
    return "none";
  case LibraryParam::Access::Read:
    return "r";
  case LibraryParam::Access::Write:
    return "w";
  case LibraryParam::Access::ReadWrite:
    return "rw";
  }
  return "none";
}

static std::string effectText(const LibraryParam &param) {
  switch (param.effect) {
  case LibraryParam::Effect::Borrow:
    return {};
  case LibraryParam::Effect::Release:
    return ":release(" + param.family + ")";
  case LibraryParam::Effect::Realloc:
    return ":realloc(" + param.family + ")";
  case LibraryParam::Effect::Retain:
    return ":retain(" + param.state + ")";
  case LibraryParam::Effect::Escape:
    return ":escape";
  case LibraryParam::Effect::Init:
    return ":init(" + param.family + ")";
  case LibraryParam::Effect::Fini:
    return ":fini(" + param.family + ")";
  }
  return {};
}

static std::string paramText(const LibraryParam &param) {
  std::string text;
  switch (param.type) {
  case LibraryParam::Type::Int:
    return "int";
  case LibraryParam::Type::Other:
    return "other";
  case LibraryParam::Type::Function:
    text = "fn";
    break;
  case LibraryParam::Type::Pointer:
    text = accessText(param.access);
    break;
  }
  if (param.bytes)
    text += ":bytes(" + param.bytes->str() + ")";
  if (param.count)
    text += ":count(" + param.count->str() + ")";
  if (param.string)
    text += ":str";
  if (param.null == LibraryParam::Null::Allowed)
    text += ":null-ok";
  else if (param.null == LibraryParam::Null::AllowedIfZero && param.zeroTerm)
    text += ":null-if-zero(" + param.zeroTerm->str() + ")";
  text += effectText(param);
  if (param.callback)
    text += ":" + callbackText(*param.callback);
  if (param.out)
    text += ":out(" + resultText(*param.out, false) + ")";
  return text;
}

std::string LibraryEntry::str() const {
  std::string text = name + " (";
  for (std::size_t i = 0; i < params.size(); ++i)
    text += (i != 0 ? ", " : "") + paramText(params[i]);
  if (variadic)
    text += params.empty() ? "..." : ", ...";
  text += ") -> " + resultText(result, noreturn);
  for (const LibDisjoint &d : disjoint)
    text += " disjoint(" + std::to_string(d.first) + "," +
            std::to_string(d.second) + "," + d.length.str() + ")";
  for (const LibCopy &copy : copies)
    text += " copies(" + std::to_string(copy.dst) + "," +
            std::to_string(copy.src) + "," + copy.length.str() + ")";
  for (const LibFill &fill : fills)
    text += " fills(" + std::to_string(fill.dst) + "," + fill.value.str() +
            "," + fill.length.str() + ")";
  for (const LibStringWrite &write : writesString)
    text += " writes-str(" + std::to_string(write.dst) +
            (write.length ? "," + write.length->str() : "") + ")";
  for (const std::string &state : invalidates)
    text += " invalidates(" + state + ")";
  for (const std::string &state : reads)
    text += " reads(" + state + ")";
  if (exits)
    text += " exits";
  if (returnsTwice)
    text += " returns-twice";
  if (format) {
    text += format->kind == LibFormat::Kind::Printf ? " printf(" : " scanf(";
    text += std::to_string(format->format) + "," +
            std::to_string(format->first) + ")";
  }
  for (const LibraryChk &alias : chk) {
    text += " chk(" + alias.name + ":";
    for (std::size_t i = 0; i < alias.argumentOf.size(); ++i)
      text += (i != 0 ? "," : "") + std::to_string(alias.argumentOf[i]);
    text += ')';
  }
  return text + ";";
}

//===----------------------------------------------------------------------===//
// Lexical helpers
//===----------------------------------------------------------------------===//

namespace {
/// One logical line: a directive or an entry, with comments removed and
/// continuations joined, and the physical line of each character.
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
static bool isUpper(char c) {
  return c >= 'A' && c <= 'Z';
}
static bool isLower(char c) {
  return c >= 'a' && c <= 'z';
}
static bool isIdentifierStart(char c) {
  return isUpper(c) || isLower(c) || c == '_';
}
static bool isIdentifierChar(char c) {
  return isIdentifierStart(c) || isDigit(c);
}

static bool isIdentifier(std::string_view text) {
  return !text.empty() && isIdentifierStart(text.front()) &&
         std::ranges::all_of(text, isIdentifierChar);
}

static std::optional<LibraryParam::Access> accessNamed(std::string_view word) {
  using Access = LibraryParam::Access;
  if (word == "r")
    return Access::Read;
  if (word == "w")
    return Access::Write;
  if (word == "rw")
    return Access::ReadWrite;
  if (word == "none")
    return Access::None;
  return std::nullopt;
}

/// The effect a flag names; `escape` for the one without a family.
static LibraryParam::Effect effectNamed(std::string_view flag) {
  using Effect = LibraryParam::Effect;
  if (flag == "release")
    return Effect::Release;
  if (flag == "realloc")
    return Effect::Realloc;
  if (flag == "retain")
    return Effect::Retain;
  if (flag == "init")
    return Effect::Init;
  if (flag == "fini")
    return Effect::Fini;
  return Effect::Escape;
}

static LibraryResult::Null nullNamed(std::string_view flag) {
  if (flag == "nonnull")
    return LibraryResult::Null::Never;
  if (flag == "null-on-failure")
    return LibraryResult::Null::OnFailure;
  return LibraryResult::Null::May;
}

/// The key a flag is recorded under: flags that exclude one another share
/// the key of their group.
static std::string
exclusionKey(const std::string &flag,
             std::initializer_list<std::pair<bool, std::string_view>> groups) {
  for (const auto &[member, group] : groups)
    if (member)
      return std::string(group);
  return flag;
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
    while (left && accept("*")) {
      auto right = atom();
      if (!right)
        return std::nullopt;
      left = LibTerm::product(std::move(*left), std::move(*right));
    }
    return left;
  }
  std::optional<LibTerm> lengthOf(bool format) {
    const auto index = argumentIndex();
    if (!index || !accept(")"))
      return fail(format ? "expected 'fmtlen(aN)'" : "expected 'strlen(aN)'");
    return format ? LibTerm::formatLength(*index)
                  : LibTerm::stringLength(*index);
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
    if (accept("strlen("))
      return lengthOf(false);
    if (accept("fmtlen("))
      return lengthOf(true);
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
    if (isUpper(text[pos]) || text[pos] == '_') {
      const std::size_t begin = pos;
      while (pos < text.size() && isIdentifierChar(text[pos]))
        ++pos;
      return LibTerm{.kind = LibTerm::Kind::Macro,
                     .macro = std::string(text.substr(begin, pos - begin))};
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
  /// The header entries are declared in; empty under `builtins`.
  std::string header;
  bool haveSection = false;
  bool underPattern = false;
  /// The entry being parsed, for messages.
  std::string entryName;

  bool fail(const std::string &why, std::optional<unsigned> at = {}) {
    if (!message.empty())
      return false;
    unsigned number = at.value_or(0);
    if (!at && line != nullptr && !line->lines.empty())
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
  bool peek(char c) {
    skipSpace();
    return pos < line->text.size() && line->text[pos] == c;
  }
  bool accept(char c) {
    if (!peek(c))
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
  std::optional<std::string> parenthesisedName(std::string_view flag,
                                               std::string_view what) {
    const std::string context = "after '" + std::string(flag) + "'";
    if (!expect('(', context))
      return std::nullopt;
    auto text = name(what);
    if (!text || !expect(')', "after " + std::string(what)))
      return std::nullopt;
    return text;
  }
  std::optional<std::int64_t> integer(bool allowMinusOne) {
    skipSpace();
    const bool negative = allowMinusOne && accept('-');
    const std::size_t begin = pos;
    std::int64_t value = 0;
    while (pos < line->text.size() && isDigit(line->text[pos]) &&
           value < 1000000)
      value = (value * 10) + (line->text[pos++] - '0');
    if (pos == begin || (negative && value != 1)) {
      fail(negative ? "only -1 may be negative" : "expected an integer");
      return std::nullopt;
    }
    return negative ? -value : value;
  }
  std::optional<std::uint8_t> index(std::string_view what) {
    const auto value = integer(false);
    if (!value)
      return std::nullopt;
    if (*value > 255) {
      fail(std::string(what) + " out of range");
      return std::nullopt;
    }
    return static_cast<std::uint8_t>(*value);
  }
  /// A term inside a clause's argument list, after its '(' or a ','.
  std::optional<LibTerm> clauseTerm(std::string_view clause) {
    TermReader reader(line->text, pos);
    auto term = reader.read();
    if (!term)
      fail(reader.error() + " in '" + std::string(clause) + "(…)'");
    return term;
  }
  std::optional<LibTerm> parenthesisedTerm(std::string_view flag) {
    if (!expect('(', "after '" + std::string(flag) + "'"))
      return std::nullopt;
    TermReader reader(line->text, pos);
    auto term = reader.read();
    if (!term) {
      fail(reader.error() + " in '" + std::string(flag) + "(…)'");
      return std::nullopt;
    }
    if (!expect(')', "after the term of '" + std::string(flag) + "'"))
      return std::nullopt;
    return term;
  }

  bool parseLine();
  bool parseHeader();
  bool parseEntry(std::string rowName);
  std::optional<LibraryParam> parseParam();
  bool parseParamFlag(LibraryParam &param, const std::string &flag,
                      std::set<std::string> &seen);
  std::optional<LibCallback> parseCallback(const std::string &flag);
  std::optional<LibraryResult> parseResult(bool out, bool &noreturn);
  bool parseResultFlag(LibraryResult &result, const std::string &flag, bool out,
                       std::set<std::string> &seen);
  bool parseClause(LibraryEntry &entry, const std::string &clause);
  bool validateTerm(const LibraryEntry &entry, const LibTerm &term,
                    std::string_view where);
  bool validateResult(const LibraryEntry &entry, const LibraryResult &result,
                      std::string_view where);
  bool validateEntry(LibraryEntry &entry);
  bool validateTable();
};

std::optional<LibraryParam> LibrarySpecParser::parseParam() {
  using Type = LibraryParam::Type;
  const std::string kind = word();
  LibraryParam param;
  if (kind == "int" || kind == "other") {
    param.type = kind == "int" ? Type::Int : Type::Other;
    if (peek(':')) {
      fail("'" + kind + "' parameters take no flags");
      return std::nullopt;
    }
    return param;
  }
  if (kind == "fn") {
    param.type = Type::Function;
  } else if (const auto access = accessNamed(kind)) {
    param.type = Type::Pointer;
    param.access = *access;
  } else {
    fail(kind.empty() ? "expected a parameter"
                      : "unknown parameter type '" + kind + "'");
    return std::nullopt;
  }
  std::set<std::string> seen;
  while (accept(':')) {
    const std::string flag = word();
    if (!parseParamFlag(param, flag, seen))
      return std::nullopt;
  }
  return param;
}

std::optional<LibCallback>
LibrarySpecParser::parseCallback(const std::string &flag) {
  LibCallback callback;
  if (flag == "at-exit") {
    callback.kind = LibCallback::Kind::AtExit;
    return callback;
  }
  callback.kind =
      flag == "sync" ? LibCallback::Kind::Sync : LibCallback::Kind::Entry;
  if (!expect('(', "after '" + flag + "'"))
    return std::nullopt;
  if (!accept(')')) {
    do {
      const auto argument = index("callback argument");
      if (!argument)
        return std::nullopt;
      callback.arguments.push_back(*argument);
    } while (accept(','));
    if (!expect(')', "after the arguments of '" + flag + "(…)'"))
      return std::nullopt;
  }
  if (callback.kind == LibCallback::Kind::Entry &&
      callback.arguments.size() > 1) {
    fail("'entry(…)' takes at most one argument");
    return std::nullopt;
  }
  return callback;
}

bool LibrarySpecParser::parseParamFlag(LibraryParam &param,
                                       const std::string &flag,
                                       std::set<std::string> &seen) {
  using Effect = LibraryParam::Effect;
  const bool function = param.type == LibraryParam::Type::Function;
  const bool callback = flag == "sync" || flag == "entry" || flag == "at-exit";
  const bool effect = flag == "release" || flag == "realloc" ||
                      flag == "retain" || flag == "escape" || flag == "init" ||
                      flag == "fini";
  const bool null = flag == "null-ok" || flag == "null-if-zero";
  const std::string key = exclusionKey(
      flag, {{callback, "callback"}, {effect, "effect"}, {null, "null"}});
  if (!seen.insert(key).second)
    return fail(key == flag ? "duplicate flag '" + flag + "'"
                            : "conflicting " + key + " flags ('" + flag + "')");
  if (function && !callback && flag != "null-ok")
    return fail("'" + flag + "' is not a flag of 'fn' parameters");
  if (flag == "bytes" || flag == "count") {
    auto term = parenthesisedTerm(flag);
    if (!term)
      return false;
    if ((flag == "bytes" && param.count) || (flag == "count" && param.bytes))
      return fail("'bytes' and 'count' exclude one another");
    (flag == "bytes" ? param.bytes : param.count) = std::move(*term);
  } else if (flag == "str") {
    param.string = true;
  } else if (flag == "null-ok") {
    param.null = LibraryParam::Null::Allowed;
  } else if (flag == "null-if-zero") {
    auto term = parenthesisedTerm(flag);
    if (!term)
      return false;
    param.null = LibraryParam::Null::AllowedIfZero;
    param.zeroTerm = std::move(*term);
  } else if (flag == "escape") {
    param.effect = Effect::Escape;
  } else if (effect) {
    const bool state = flag == "retain";
    auto text = parenthesisedName(flag, state ? "a state slot" : "a family");
    if (!text)
      return false;
    param.effect = effectNamed(flag);
    (state ? param.state : param.family) = std::move(*text);
  } else if (callback) {
    auto parsed = parseCallback(flag);
    if (!parsed)
      return false;
    param.callback = std::move(*parsed);
  } else if (flag == "out") {
    if (!expect('(', "after 'out'"))
      return false;
    bool noreturn = false;
    auto value = parseResult(true, noreturn);
    if (!value || !expect(')', "after the value of 'out(…)'"))
      return false;
    param.out = std::move(*value);
  } else {
    return fail(flag.empty() ? "expected a parameter flag after ':'"
                             : "unknown parameter flag '" + flag + "'");
  }
  return true;
}

std::optional<LibraryResult> LibrarySpecParser::parseResult(bool out,
                                                            bool &noreturn) {
  using Kind = LibraryResult::Kind;
  const std::string kind = word();
  LibraryResult result;
  if (kind == "void" || kind == "int" || kind == "noreturn") {
    if (out) {
      fail("an 'out' value must be a pointer, not '" + kind + "'");
      return std::nullopt;
    }
    result.kind = kind == "int" ? Kind::Int : Kind::Void;
    noreturn = kind == "noreturn";
    if (kind == "int" && accept(':')) {
      if (word() != "value") {
        fail("the only flag of an 'int' result is 'value(…)'");
        return std::nullopt;
      }
      result.value = parenthesisedTerm("value");
      if (!result.value)
        return std::nullopt;
    }
    if (peek(':')) {
      fail("'" + kind + "' results take no " +
           (kind == "int" ? "other flags" : "flags"));
      return std::nullopt;
    }
    return result;
  }
  if (kind == "fresh" || kind == "static" || kind == "interior-state") {
    const bool family = kind == "fresh";
    auto text = parenthesisedName(kind, family ? "a family" : "a state slot");
    if (!text)
      return std::nullopt;
    result.kind = Kind::InteriorState;
    if (family)
      result.kind = Kind::Fresh;
    else if (kind == "static")
      result.kind = Kind::Static;
    (family ? result.family : result.state) = std::move(*text);
  } else if (kind == "arg" || kind == "interior") {
    if (!expect('(', "after '" + kind + "'"))
      return std::nullopt;
    const auto argument = index("argument index");
    if (!argument || !expect(')', "after the argument index"))
      return std::nullopt;
    result.kind = kind == "arg" ? Kind::Arg : Kind::Interior;
    result.arg = *argument;
  } else if (kind == "ptr") {
    result.kind = Kind::Unknown;
  } else {
    fail(kind.empty() ? "expected a result"
                      : "unknown result kind '" + kind + "'");
    return std::nullopt;
  }
  result.null = defaultNull(result.kind);
  std::set<std::string> seen;
  while (accept(':')) {
    const std::string flag = word();
    if (!parseResultFlag(result, flag, out, seen))
      return std::nullopt;
  }
  const bool zeroFlag = seen.contains("zero");
  const bool fresh = result.kind == Kind::Fresh ||
                     (result.kind == Kind::Arg && !result.family.empty());
  if (zeroFlag && !fresh) {
    fail("'zero-init' and 'no-zero-init' apply only to fresh results");
    return std::nullopt;
  }
  if (fresh && result.family == HeapFamily && !zeroFlag) {
    fail("a 'fresh(free)' value needs 'zero-init' or 'no-zero-init' (§11)");
    return std::nullopt;
  }
  if (result.extent && result.kind != Kind::Fresh &&
      result.kind != Kind::Static) {
    fail("'extent' applies only to fresh and static results");
    return std::nullopt;
  }
  if (result.offset && result.kind != Kind::Interior) {
    fail("'offset' applies only to 'interior(N)' results");
    return std::nullopt;
  }
  if (result.zeroFilled && (result.kind != Kind::Fresh || !result.extent)) {
    fail("'zero-filled' applies only to fresh results with an extent");
    return std::nullopt;
  }
  return result;
}

bool LibrarySpecParser::parseResultFlag(LibraryResult &result,
                                        const std::string &flag, bool out,
                                        std::set<std::string> &seen) {
  const bool null =
      flag == "null-on-failure" || flag == "nonnull" || flag == "null-ok";
  const bool zero = flag == "zero-init" || flag == "no-zero-init";
  const bool alternative = flag == "or-fresh" || flag == "or-static";
  const std::string key = exclusionKey(
      flag, {{null, "null"}, {zero, "zero"}, {alternative, "alternative"}});
  if (!seen.insert(key).second)
    return fail(key == flag ? "duplicate flag '" + flag + "'"
                            : "conflicting " + key + " flags ('" + flag + "')");
  if (flag == "extent" || flag == "offset") {
    auto term = parenthesisedTerm(flag);
    if (!term)
      return false;
    (flag == "extent" ? result.extent : result.offset) = std::move(*term);
  } else if (null) {
    result.null = nullNamed(flag);
  } else if (zero) {
    result.zeroInit = flag == "zero-init";
  } else if (flag == "zero-filled") {
    result.zeroFilled = true;
  } else if (flag == "str") {
    result.string = true;
  } else if (flag == "replaces") {
    if (!out || result.kind != LibraryResult::Kind::Fresh)
      return fail("'replaces' applies only to a fresh 'out' value");
    result.replaces = true;
  } else if (alternative) {
    if (result.kind != LibraryResult::Kind::Arg)
      return fail("'" + flag + "' applies only to 'arg(N)' results");
    const bool fresh = flag == "or-fresh";
    auto text = parenthesisedName(flag, fresh ? "a family" : "a state slot");
    if (!text)
      return false;
    (fresh ? result.family : result.state) = std::move(*text);
  } else {
    return fail(flag.empty() ? "expected a result flag after ':'"
                             : "unknown result flag '" + flag + "'");
  }
  return true;
}

bool LibrarySpecParser::parseClause(LibraryEntry &entry,
                                    const std::string &clause) {
  if (clause == "exits" || clause == "returns-twice") {
    bool &flag = clause == "exits" ? entry.exits : entry.returnsTwice;
    if (flag)
      return fail("duplicate clause '" + clause + "'");
    flag = true;
  } else if (clause == "invalidates" || clause == "reads") {
    auto state = parenthesisedName(clause, "a state slot");
    if (!state)
      return false;
    auto &list = clause == "reads" ? entry.reads : entry.invalidates;
    if (std::ranges::find(list, *state) != list.end())
      return fail("duplicate clause '" + clause + "(" + *state + ")'");
    list.push_back(std::move(*state));
  } else if (clause == "disjoint") {
    if (!expect('(', "after 'disjoint'"))
      return false;
    LibDisjoint disjoint;
    const auto first = index("argument index");
    if (!first || !expect(',', "in 'disjoint(…)'"))
      return false;
    const auto second = index("argument index");
    if (!second || !expect(',', "in 'disjoint(…)'"))
      return false;
    TermReader reader(line->text, pos);
    auto length = reader.read();
    if (!length)
      return fail(reader.error() + " in 'disjoint(…)'");
    if (!expect(')', "after 'disjoint(…'"))
      return false;
    disjoint.first = *first;
    disjoint.second = *second;
    disjoint.length = std::move(*length);
    entry.disjoint.push_back(std::move(disjoint));
  } else if (clause == "copies" || clause == "fills") {
    const std::string context = "in '" + clause + "(…)'";
    if (!expect('(', "after '" + clause + "'"))
      return false;
    const auto dst = index("argument index");
    if (!dst || !expect(',', context))
      return false;
    std::optional<std::uint8_t> src;
    std::optional<LibTerm> value;
    if (clause == "copies")
      src = index("argument index");
    else
      value = clauseTerm(clause);
    if ((!src && !value) || !expect(',', context))
      return false;
    auto length = clauseTerm(clause);
    if (!length || !expect(')', "after '" + clause + "(…'"))
      return false;
    if (src)
      entry.copies.push_back(
          LibCopy{.dst = *dst, .src = *src, .length = std::move(*length)});
    else
      entry.fills.push_back(LibFill{.dst = *dst,
                                    .value = std::move(*value),
                                    .length = std::move(*length)});
  } else if (clause == "writes-str") {
    if (!expect('(', "after 'writes-str'"))
      return false;
    const auto dst = index("argument index");
    if (!dst)
      return false;
    LibStringWrite write{.dst = *dst, .length = std::nullopt};
    if (accept(',')) {
      write.length = clauseTerm(clause);
      if (!write.length)
        return false;
    }
    if (!expect(')', "after 'writes-str(…'"))
      return false;
    entry.writesString.push_back(std::move(write));
  } else if (clause == "printf" || clause == "scanf") {
    if (entry.format)
      return fail("more than one format clause");
    if (!expect('(', "after '" + clause + "'"))
      return false;
    const auto format = index("format argument");
    if (!format || !expect(',', "in '" + clause + "(…)'"))
      return false;
    const auto first = index("first variadic argument");
    if (!first || !expect(')', "after '" + clause + "(…'"))
      return false;
    entry.format =
        LibFormat{.kind = clause == "printf" ? LibFormat::Kind::Printf
                                             : LibFormat::Kind::Scanf,
                  .format = *format,
                  .first = *first};
  } else if (clause == "chk") {
    if (!expect('(', "after 'chk'"))
      return false;
    auto alias = name("an alias name");
    if (!alias || !expect(':', "after the alias name"))
      return false;
    LibraryChk chk{.name = std::move(*alias)};
    do {
      const auto argument = integer(true);
      if (!argument)
        return false;
      if (*argument > 127)
        return fail("alias argument index out of range");
      chk.argumentOf.push_back(static_cast<std::int8_t>(*argument));
    } while (accept(','));
    if (!expect(')', "after the arguments of 'chk(…)'"))
      return false;
    entry.chk.push_back(std::move(chk));
  } else {
    return fail(clause.empty() ? "expected a clause or ';'"
                               : "unknown clause '" + clause + "'");
  }
  return true;
}

bool LibrarySpecParser::parseLine() {
  pos = 0;
  if (atEnd())
    return true;
  const std::string first = word();
  if (first == "header")
    return parseHeader();
  if (first == "builtins") {
    if (!expect(';', "after 'builtins'"))
      return false;
    if (!atEnd())
      return fail("unexpected text after ';'");
    header.clear();
    haveSection = true;
    underPattern = false;
    return true;
  }
  if (!isIdentifier(first))
    return fail(first.empty() ? "expected an entry or a directive"
                              : "'" + first + "' is not a function name");
  return parseEntry(first);
}

static bool isHeaderName(std::string_view stem) {
  const auto allowed = [](char c) {
    return isIdentifierChar(c) || c == '.' || c == '/' || c == '-' || c == '+';
  };
  return !stem.empty() && stem.front() != '/' && stem.back() != '/' &&
         stem.find("..") == std::string_view::npos &&
         stem.find("//") == std::string_view::npos &&
         std::ranges::all_of(stem, allowed);
}

bool LibrarySpecParser::parseHeader() {
  skipSpace();
  const std::size_t begin = pos;
  while (pos < line->text.size() && line->text[pos] != ';' &&
         line->text[pos] != ' ')
    ++pos;
  const std::string path = line->text.substr(begin, pos - begin);
  if (!expect(';', "after the header name"))
    return false;
  if (!atEnd())
    return fail("unexpected text after ';'");
  const bool pattern = path.ends_with("/*");
  const std::string_view stem =
      std::string_view(path).substr(0, path.size() - (pattern ? 2 : 0));
  if (!isHeaderName(stem))
    return fail("invalid header name '" + path + "'");
  if (std::ranges::find(spec.headerList, path) == spec.headerList.end()) {
    spec.headerList.push_back(path);
    if (pattern)
      spec.headerPrefixes.push_back(std::string(stem) + "/");
    else
      spec.headerNames.insert(path);
  }
  header = pattern ? std::string() : path;
  haveSection = true;
  underPattern = pattern;
  return true;
}

bool LibrarySpecParser::parseEntry(std::string rowName) {
  entryName = rowName;
  if (!haveSection)
    return fail("an entry must follow a 'header' or 'builtins' line");
  if (underPattern)
    return fail("an entry must follow a concrete 'header', not a pattern");
  LibraryEntry entry;
  entry.name = std::move(rowName);
  entry.header = header;
  entry.line = line->lines.front();
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
    if (!expect(')', entry.variadic ? "after '...'" : "after the parameters"))
      return false;
  }
  if (!accept("->"))
    return fail("expected '->' after the parameters");
  bool noreturn = false;
  auto result = parseResult(false, noreturn);
  if (!result)
    return false;
  entry.result = std::move(*result);
  entry.noreturn = noreturn;
  while (!accept(';')) {
    if (atEnd())
      return fail("expected ';' at the end of the entry");
    if (!parseClause(entry, word()))
      return false;
  }
  if (!atEnd())
    return fail("unexpected text after ';' (one entry per line)");
  if (!validateEntry(entry))
    return false;
  spec.rows.push_back(std::move(entry));
  entryName.clear();
  return true;
}

static bool isPointerParam(const LibraryEntry &entry, unsigned index) {
  return index < entry.params.size() &&
         entry.params[index].type == LibraryParam::Type::Pointer;
}

static void forEachLeaf(const LibTerm &term,
                        const std::function<void(const LibTerm &)> &visit) {
  if (term.operands.empty())
    visit(term);
  for (const LibTerm &operand : term.operands)
    forEachLeaf(operand, visit);
}

bool LibrarySpecParser::validateTerm(const LibraryEntry &entry,
                                     const LibTerm &term,
                                     std::string_view where) {
  std::string problem;
  forEachLeaf(term, [&](const LibTerm &leaf) {
    if (!problem.empty() || leaf.kind == LibTerm::Kind::Constant ||
        leaf.kind == LibTerm::Kind::Macro)
      return;
    const std::string spelled = leaf.str();
    if (leaf.arg >= entry.params.size())
      problem = "'" + spelled + "' in " + std::string(where) +
                " names a missing parameter";
    else if (leaf.kind == LibTerm::Kind::StringLength &&
             entry.params[leaf.arg].type != LibraryParam::Type::Pointer)
      problem = "'" + spelled + "' needs a pointer parameter";
    else if (leaf.kind == LibTerm::Kind::FormatLength &&
             (!entry.format || entry.format->kind != LibFormat::Kind::Printf ||
              entry.format->format != leaf.arg))
      problem = "'" + spelled + "' needs a 'printf(" +
                std::to_string(leaf.arg) + ", …)' clause";
  });
  return problem.empty() || fail(problem);
}

bool LibrarySpecParser::validateResult(const LibraryEntry &entry,
                                       const LibraryResult &result,
                                       std::string_view where) {
  using Kind = LibraryResult::Kind;
  if (result.kind == Kind::Arg || result.kind == Kind::Interior) {
    const LibraryParam *param = entry.param(result.arg);
    if (param == nullptr || param->type != LibraryParam::Type::Pointer)
      return fail(std::string(where) + " names argument " +
                  std::to_string(result.arg) + ", which is not a pointer");
    const bool alternative = !result.family.empty() || !result.state.empty();
    if (alternative && param->null != LibraryParam::Null::Allowed)
      return fail(std::string(where) +
                  ": 'or-fresh'/'or-static' need argument " +
                  std::to_string(result.arg) + " to be 'null-ok'");
  }
  return std::ranges::all_of(
      std::initializer_list<const std::optional<LibTerm> *>{
          &result.extent, &result.value, &result.offset},
      [&](const std::optional<LibTerm> *term) {
        return !*term || validateTerm(entry, **term, where);
      });
}

bool LibrarySpecParser::validateEntry(LibraryEntry &entry) {
  const auto count = entry.params.size();
  const auto isPointer = [&entry](unsigned index) {
    return index < entry.params.size() &&
           entry.params[index].type == LibraryParam::Type::Pointer;
  };
  for (std::size_t i = 0; i < count; ++i) {
    const LibraryParam &param = entry.params[i];
    const std::string where = "parameter " + std::to_string(i);
    for (const auto *term : {&param.bytes, &param.count, &param.zeroTerm})
      if (*term && !validateTerm(entry, **term, where))
        return false;
    if (param.string && param.access != LibraryParam::Access::Read &&
        param.access != LibraryParam::Access::ReadWrite)
      return fail(where + ": 'str' needs read access ('r' or 'rw')");
    if (param.callback)
      for (const std::uint8_t argument : param.callback->arguments)
        if (argument >= count)
          return fail(where + ": the callback names missing argument " +
                      std::to_string(argument));
    if (param.out &&
        !validateResult(entry, *param.out, "the out value of " + where))
      return false;
  }
  if (!validateResult(entry, entry.result, "the result"))
    return false;
  if (entry.exits && !entry.noreturn)
    return fail("'exits' needs a 'noreturn' result");
  if (entry.returnsTwice && entry.result.kind != LibraryResult::Kind::Int)
    return fail("'returns-twice' needs an 'int' result");
  for (const LibDisjoint &disjoint : entry.disjoint) {
    if (!isPointer(disjoint.first) || !isPointer(disjoint.second) ||
        disjoint.first == disjoint.second)
      return fail("'disjoint' needs two different pointer parameters");
    if (!validateTerm(entry, disjoint.length, "'disjoint'"))
      return false;
  }
  const auto accesses = [&entry](unsigned index, LibraryParam::Access access) {
    if (!isPointerParam(entry, index))
      return false;
    const LibraryParam::Access actual = entry.params[index].access;
    return actual == access || actual == LibraryParam::Access::ReadWrite;
  };
  for (const LibCopy &copy : entry.copies) {
    if (!accesses(copy.dst, LibraryParam::Access::Write) ||
        !accesses(copy.src, LibraryParam::Access::Read) || copy.dst == copy.src)
      return fail("'copies' needs a written and a different read pointer "
                  "parameter");
    if (!validateTerm(entry, copy.length, "'copies'"))
      return false;
  }
  for (const LibFill &fill : entry.fills) {
    if (!accesses(fill.dst, LibraryParam::Access::Write))
      return fail("'fills' needs a written pointer parameter");
    if (!validateTerm(entry, fill.value, "'fills'") ||
        !validateTerm(entry, fill.length, "'fills'"))
      return false;
  }
  for (const LibStringWrite &write : entry.writesString) {
    if (!accesses(write.dst, LibraryParam::Access::Write))
      return fail("'writes-str' needs a written pointer parameter");
    if (write.length && !validateTerm(entry, *write.length, "'writes-str'"))
      return false;
  }
  if (entry.format) {
    LibFormat &format = *entry.format;
    if (!isPointer(format.format) || !entry.params[format.format].string)
      return fail("the format argument must be an 'r:str' parameter");
    format.vaList = !entry.variadic;
    if (entry.variadic && format.first != count)
      return fail("the first variadic argument is " + std::to_string(count) +
                  ", not " + std::to_string(format.first));
    if (!entry.variadic &&
        (format.first >= count ||
         entry.params[format.first].type != LibraryParam::Type::Other))
      return fail("a 'v…' function's va_list argument must be 'other'");
  }
  for (const LibraryChk &alias : entry.chk) {
    if (alias.name == entry.name)
      return fail("an alias cannot repeat the entry's name");
    std::vector<int> uses(count, 0);
    for (std::size_t i = 0; i < alias.argumentOf.size(); ++i) {
      // NOLINTNEXTLINE(bugprone-signed-char-misuse,cert-str34-c): an index
      const int target = alias.argumentOf[i];
      const bool last = i + 1 == alias.argumentOf.size();
      if (target == -1)
        continue;
      if (std::cmp_equal(target, count) && entry.variadic && last)
        continue;
      if (target < 0 || std::cmp_greater_equal(target, count))
        return fail("alias '" + alias.name + "' maps to missing argument " +
                    std::to_string(target));
      ++uses[static_cast<std::size_t>(target)];
    }
    for (std::size_t i = 0; i < count; ++i)
      if (uses[i] != 1)
        return fail("alias '" + alias.name + "' must map argument " +
                    std::to_string(i) + " exactly once");
  }
  return true;
}

bool LibrarySpecParser::validateTable() {
  for (std::size_t i = 0; i < spec.rows.size(); ++i) {
    const LibraryEntry &row = spec.rows[i];
    const auto [first, last] = spec.byName.equal_range(row.name);
    for (auto it = first; it != last; ++it) {
      if (spec.rows[it->second].signature() != row.signature())
        continue;
      entryName = row.name;
      return fail("defined again with the same signature (first on line " +
                      std::to_string(spec.rows[it->second].line) + ")",
                  row.line);
    }
    spec.byName.emplace(row.name, i);
  }
  for (std::size_t i = 0; i < spec.rows.size(); ++i) {
    const LibraryEntry &row = spec.rows[i];
    for (std::size_t k = 0; k < row.chk.size(); ++k) {
      const std::string &alias = row.chk[k].name;
      entryName = row.name;
      if (spec.byName.contains(alias))
        return fail("alias '" + alias + "' is also an entry name", row.line);
      if (!spec.byAlias.emplace(alias, std::make_pair(i, k)).second)
        return fail("alias '" + alias + "' is declared twice", row.line);
    }
  }
  entryName.clear();
  return true;
}

bool LibrarySpecParser::parse(std::string_view text) {
  const std::vector<LogicalLine> lines = logicalLines(text);
  for (const LogicalLine &logical : lines) {
    line = &logical;
    entryName.clear();
    if (!parseLine())
      return false;
  }
  line = nullptr;
  entryName.clear();
  return validateTable();
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
  error.clear();
  return spec;
}

const LibraryEntry *LibrarySpec::find(std::string_view name) const {
  const auto it = byName.lower_bound(name);
  return it == byName.end() || it->first != name ? nullptr : &rows[it->second];
}

std::vector<const LibraryEntry *>
LibrarySpec::overloads(std::string_view name) const {
  std::vector<const LibraryEntry *> found;
  const auto [first, last] = byName.equal_range(name);
  for (auto it = first; it != last; ++it)
    found.push_back(&rows[it->second]);
  return found;
}

std::optional<LibraryMatch>
LibrarySpec::resolve(std::string_view callee,
                     const LibSignature *declared) const {
  const auto direct = [&](std::string_view name) -> const LibraryEntry * {
    const auto [first, last] = byName.equal_range(name);
    for (auto it = first; it != last; ++it)
      if (declared == nullptr || rows[it->second].accepts(*declared))
        return &rows[it->second];
    return nullptr;
  };
  const auto aliased =
      [&](std::string_view name) -> std::optional<LibraryMatch> {
    const auto it = byAlias.find(name);
    if (it == byAlias.end())
      return std::nullopt;
    const LibraryEntry &row = rows[it->second.first];
    return LibraryMatch{.entry = &row, .alias = &row.chk[it->second.second]};
  };
  if (const LibraryEntry *entry = direct(callee))
    return LibraryMatch{.entry = entry};
  if (auto match = aliased(callee))
    return match;
  constexpr std::string_view Builtin = "__builtin_";
  if (!callee.starts_with(Builtin))
    return std::nullopt;
  callee.remove_prefix(Builtin.size());
  if (const LibraryEntry *entry = direct(callee))
    return LibraryMatch{.entry = entry};
  return aliased(callee);
}

std::optional<LibraryMatch> LibrarySpec::lookup(std::string_view callee) const {
  return resolve(callee, nullptr);
}

std::optional<LibraryMatch>
LibrarySpec::lookup(std::string_view callee,
                    const LibSignature &declared) const {
  return resolve(callee, &declared);
}

bool LibrarySpec::isPlatformHeader(std::string_view relativePath,
                                   bool darwinSdk) const {
  if (darwinSdk)
    return true;
  std::string path(relativePath);
  std::ranges::replace(path, '\\', '/');
  while (path.starts_with("./"))
    path.erase(0, 2);
  if (headerNames.contains(path))
    return true;
  return std::ranges::any_of(headerPrefixes, [&path](const std::string &p) {
    return path.starts_with(p);
  });
}

// The table text, as `LibrarySpecTextData` / `LibrarySpecTextSize`; generated
// from LibrarySpec.txt by lib/Core/CMakeLists.txt.
#include "LibrarySpecText.inc"

std::string_view LibrarySpec::shippedText() {
  return {LibrarySpecTextData, LibrarySpecTextSize};
}

namespace {
/// The shipped table, or the error that kept it empty.
struct ShippedLibrarySpec {
  LibrarySpec spec;
  std::string error;
};
} // namespace

static const ShippedLibrarySpec &shippedLibrarySpec() {
  static const ShippedLibrarySpec Shipped = [] {
    ShippedLibrarySpec result;
    if (auto parsed =
            LibrarySpec::parse(LibrarySpec::shippedText(), result.error))
      result.spec = std::move(*parsed);
    return result;
  }();
  return Shipped;
}

const LibrarySpec &LibrarySpec::shipped() {
  return shippedLibrarySpec().spec;
}

const std::string &LibrarySpec::shippedError() {
  return shippedLibrarySpec().error;
}

std::optional<unsigned> formatArgumentCount(std::string_view format,
                                            LibFormat::Kind kind) {
  const auto digit = [](char c) { return c >= '0' && c <= '9'; };
  unsigned count = 0;
  for (std::size_t i = 0; i < format.size(); ++i) {
    if (format[i] != '%')
      continue;
    if (++i >= format.size())
      return std::nullopt;
    if (format[i] == '%')
      continue;
    // `%1$d`: positional arguments are matched by index, not counted.
    std::size_t j = i;
    while (j < format.size() && digit(format[j]))
      ++j;
    if (j > i && j < format.size() && format[j] == '$')
      return std::nullopt;
    if (kind == LibFormat::Kind::Scanf) {
      const bool suppressed = format[i] == '*';
      if (suppressed)
        ++i;
      while (i < format.size() && digit(format[i]))
        ++i;
      while (i < format.size() &&
             std::string_view("hljztLqm").find(format[i]) !=
                 std::string_view::npos)
        ++i;
      if (i >= format.size())
        return std::nullopt;
      if (format[i] == '[') {
        // A scan set: `]` right after `[` or `[^` is a member.
        ++i;
        if (i < format.size() && format[i] == '^')
          ++i;
        if (i < format.size() && format[i] == ']')
          ++i;
        while (i < format.size() && format[i] != ']')
          ++i;
        if (i >= format.size())
          return std::nullopt;
      }
      count += suppressed ? 0 : 1;
      continue;
    }
    while (i < format.size() &&
           std::string_view("-+ #0'").find(format[i]) != std::string_view::npos)
      ++i;
    for (int part = 0; part < 2 && i < format.size(); ++part) {
      if (part == 1) {
        if (format[i] != '.')
          break;
        ++i;
      }
      if (i < format.size() && format[i] == '*') {
        ++count;
        ++i;
      } else {
        while (i < format.size() && digit(format[i]))
          ++i;
      }
    }
    while (i < format.size() && std::string_view("hljztLq").find(format[i]) !=
                                    std::string_view::npos)
      ++i;
    if (i >= format.size())
      return std::nullopt;
    // `%m` prints `strerror(errno)` and reads no argument.
    if (format[i] != 'm')
      ++count;
  }
  return count;
}

std::string boundedWriterName(std::string_view writer) {
  // The name knowledge stays in the table's own file (§19, gate H2).
  constexpr std::string_view Suffix = "printf";
  if (writer.size() <= Suffix.size() || !writer.ends_with(Suffix))
    return {};
  const std::string_view stem = writer.substr(0, writer.size() - Suffix.size());
  if (stem != "s" && stem != "vs")
    return {};
  return std::string(stem) + "nprintf";
}

} // namespace weavec::core
