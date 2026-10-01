//===- EffectsIO.cpp - Summary format 30 text, joins, renumbering ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/EffectsIO.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace weavec::core {

//===----------------------------------------------------------------------===//
// Tokens
//===----------------------------------------------------------------------===//

static bool plainChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '#' || c == '-';
}

static std::string encode(std::string_view text) {
  static constexpr std::string_view Hex = "0123456789ABCDEF";
  std::string out;
  for (char c : text) {
    if (plainChar(c)) {
      out += c;
      continue;
    }
    unsigned byte = static_cast<unsigned char>(c);
    out += '%';
    out += Hex[byte >> 4U];
    out += Hex[byte & 0xFU];
  }
  return out;
}

static std::optional<std::string> decode(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') {
      out += text[i];
      continue;
    }
    if (i + 2 >= text.size())
      return std::nullopt;
    unsigned value = 0;
    auto [end, error] =
        std::from_chars(text.data() + i + 1, text.data() + i + 3, value, 16);
    if (error != std::errc() || end != text.data() + i + 3)
      return std::nullopt;
    out += static_cast<char>(value);
    i += 2;
  }
  return out;
}

template <typename T>
static std::optional<T> number(std::string_view text) {
  T value{};
  auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size())
    return std::nullopt;
  return value;
}

static std::vector<std::string_view> split(std::string_view text, char by) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  while (true) {
    std::size_t at = text.find(by, start);
    out.push_back(text.substr(start, at - start));
    if (at == std::string_view::npos)
      return out;
    start = at + 1;
  }
}

/// `key=value` → value, when `token` starts with `key=`.
static std::optional<std::string_view> valueOf(std::string_view token,
                                               std::string_view key) {
  if (token.size() > key.size() && token.starts_with(key) &&
      token[key.size()] == '=')
    return token.substr(key.size() + 1);
  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// Paths, terms, cases, values
//===----------------------------------------------------------------------===//

std::string printPath(const SummaryPath &path) {
  std::string out;
  switch (path.root) {
  case SummaryRoot::Param:
    out = "p" + std::to_string(path.index);
    break;
  case SummaryRoot::Global:
    out = "g" + std::to_string(path.index);
    break;
  case SummaryRoot::Result:
    out = "r";
    break;
  }
  for (const PathElem &elem : path.steps) {
    switch (elem.step) {
    case PathStep::Deref:
      out += '*';
      break;
    case PathStep::Field:
      out += "." + encode(elem.field);
      break;
    case PathStep::Index:
      out += "[" + encode(elem.field) + "]";
      break;
    }
  }
  return out;
}

std::optional<SummaryPath> parsePath(std::string_view text) {
  if (text.empty())
    return std::nullopt;
  SummaryPath path;
  std::size_t i = 1;
  auto digits = [&]() -> std::optional<std::uint32_t> {
    std::size_t start = i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9')
      ++i;
    return number<std::uint32_t>(text.substr(start, i - start));
  };
  switch (text[0]) {
  case 'p':
  case 'g': {
    auto index = digits();
    if (!index)
      return std::nullopt;
    path = text[0] == 'p' ? SummaryPath::param(*index)
                          : SummaryPath::global(*index);
    break;
  }
  case 'r':
    path = SummaryPath::result();
    break;
  default:
    return std::nullopt;
  }
  while (i < text.size()) {
    char c = text[i];
    if (c == '*') {
      path.steps.pushBack(PathElem{.step = PathStep::Deref, .field = {}});
      ++i;
      continue;
    }
    if (c == '.') {
      std::size_t start = ++i;
      while (i < text.size() && (plainChar(text[i]) || text[i] == '%'))
        ++i;
      auto name = decode(text.substr(start, i - start));
      if (!name || name->empty())
        return std::nullopt;
      path.steps.pushBack(PathElem{.step = PathStep::Field, .field = *name});
      continue;
    }
    if (c == '[') {
      std::size_t close = text.find(']', i);
      if (close == std::string_view::npos)
        return std::nullopt;
      auto name = decode(text.substr(i + 1, close - i - 1));
      if (!name)
        return std::nullopt;
      path.steps.pushBack(PathElem{.step = PathStep::Index, .field = *name});
      i = close + 1;
      continue;
    }
    return std::nullopt;
  }
  return path;
}

static std::string printTerm(const PathTerm &term) {
  if (!term.path)
    return std::to_string(term.constant);
  return printPath(*term.path) + "@" + std::to_string(term.scale) + "@" +
         std::to_string(term.constant);
}

static std::optional<PathTerm> parseTerm(std::string_view text) {
  std::vector<std::string_view> parts = split(text, '@');
  if (parts.size() == 1) {
    auto constant = number<std::int64_t>(parts[0]);
    if (!constant)
      return std::nullopt;
    return PathTerm{.path = std::nullopt, .scale = 1, .constant = *constant};
  }
  if (parts.size() != 3)
    return std::nullopt;
  auto path = parsePath(parts[0]);
  auto scale = number<std::int64_t>(parts[1]);
  auto constant = number<std::int64_t>(parts[2]);
  if (!path || !scale || !constant)
    return std::nullopt;
  return PathTerm{
      .path = std::move(path), .scale = *scale, .constant = *constant};
}

static std::string printRange(const ElementRange &range) {
  return printTerm(range.from) + "," + printTerm(range.to);
}

static std::optional<ElementRange> parseRange(std::string_view text) {
  std::vector<std::string_view> parts = split(text, ',');
  if (parts.size() != 2)
    return std::nullopt;
  auto from = parseTerm(parts[0]);
  auto to = parseTerm(parts[1]);
  if (!from || !to)
    return std::nullopt;
  return ElementRange{.from = std::move(*from), .to = std::move(*to)};
}

static std::string printClasses(const std::vector<ResultClass> &classes) {
  if (classes.empty())
    return "-";
  std::string out;
  for (std::size_t i = 0; i < classes.size(); ++i)
    out += (i == 0 ? "" : ",") + std::string(toString(classes[i]));
  return out;
}

static std::optional<std::vector<ResultClass>>
parseClasses(std::string_view text) {
  std::vector<ResultClass> out;
  if (text == "-")
    return out;
  for (std::string_view part : split(text, ',')) {
    auto c = parseResultClass(part);
    if (!c)
      return std::nullopt;
    out.push_back(*c);
  }
  return out;
}

static std::string
printParamTest(const std::optional<std::pair<std::uint32_t, bool>> &test) {
  if (!test)
    return "-";
  return std::to_string(test->first) + (test->second ? "=0" : "!=0");
}

static std::optional<std::optional<std::pair<std::uint32_t, bool>>>
parseParamTest(std::string_view text) {
  if (text == "-")
    return std::optional<std::pair<std::uint32_t, bool>>{};
  bool zero = true;
  std::size_t at = text.find("!=0");
  if (at != std::string_view::npos && at + 3 == text.size()) {
    zero = false;
  } else {
    at = text.find("=0");
    if (at == std::string_view::npos || at + 2 != text.size())
      return std::nullopt;
  }
  auto index = number<std::uint32_t>(text.substr(0, at));
  if (!index)
    return std::nullopt;
  return std::optional<std::pair<std::uint32_t, bool>>{
      std::make_pair(*index, zero)};
}

static std::string printPairTest(const std::optional<ParamPairTest> &test) {
  if (!test)
    return "";
  return ":" + std::to_string(test->first) + (test->equal ? "==" : "!=") +
         std::to_string(test->second);
}

static std::optional<ParamPairTest> parsePairTest(std::string_view text) {
  bool equal = true;
  std::size_t at = text.find("==");
  if (at == std::string_view::npos) {
    equal = false;
    at = text.find("!=");
  }
  if (at == std::string_view::npos)
    return std::nullopt;
  auto first = number<std::uint32_t>(text.substr(0, at));
  auto second = number<std::uint32_t>(text.substr(at + 2));
  if (!first || !second || *first == *second)
    return std::nullopt;
  return ParamPairTest{.first = *first, .second = *second, .equal = equal};
}

static std::string printCase(const EffectCase &when) {
  std::string out = printClasses(when.classes) + ":" +
                    printParamTest(when.paramZero) +
                    printPairTest(when.paramsEqual);
  if (when.entryZero)
    out += ":E" + printPath(when.entryZero->first) +
           (when.entryZero->second ? "=0" : "!=0");
  return out;
}

static std::optional<EffectCase> parseCase(std::string_view text) {
  std::size_t colon = text.find(':');
  if (colon == std::string_view::npos)
    return std::nullopt;
  auto classes = parseClasses(text.substr(0, colon));
  std::string_view rest = text.substr(colon + 1);
  std::optional<ParamPairTest> pair;
  std::optional<std::pair<SummaryPath, bool>> entry;
  // Optional parts after the parameter test: `N==M`, `E<path>=0`.
  std::size_t next = rest.find(':');
  std::string_view head = rest.substr(0, next);
  while (next != std::string_view::npos) {
    rest = rest.substr(next + 1);
    next = rest.find(':');
    std::string_view part = rest.substr(0, next);
    if (!part.empty() && part.front() == 'E') {
      bool zero = true;
      std::size_t at = part.rfind("!=0");
      if (at != std::string_view::npos && at + 3 == part.size()) {
        zero = false;
      } else {
        at = part.rfind("=0");
        if (at == std::string_view::npos || at + 2 != part.size())
          return std::nullopt;
      }
      auto path = parsePath(part.substr(1, at - 1));
      if (!path)
        return std::nullopt;
      entry = std::make_pair(std::move(*path), zero);
    } else {
      pair = parsePairTest(part);
      if (!pair)
        return std::nullopt;
    }
  }
  auto test = parseParamTest(head);
  if (!classes || !test)
    return std::nullopt;
  return EffectCase{.classes = std::move(*classes),
                    .paramZero = *test,
                    .paramsEqual = pair,
                    .entryZero = std::move(entry)};
}

static constexpr std::array<const char *, 8> ValueKinds = {
    "null", "fresh",    "path",    "static",
    "int",  "dangling", "unknown", "function"};

static std::string printValue(const ValueDesc &value) {
  std::string out = ValueKinds.at(static_cast<std::size_t>(value.kind));
  if (!value.family.empty())
    out += " family=" + encode(value.family);
  if (value.extent)
    out += " extent=" + printTerm(*value.extent);
  if (value.zeroed)
    out += " zeroed";
  if (value.path)
    out += " path=" + printPath(*value.path);
  if (value.offset)
    out += " offset=" + std::to_string(*value.offset);
  if (value.lo)
    out += " lo=" + std::to_string(*value.lo);
  if (value.hi)
    out += " hi=" + std::to_string(*value.hi);
  if (value.range)
    out += " range=" + value.range->toString();
  if (value.maybeNull)
    out += " maybe-null";
  if (value.object != 0)
    out += " object=" + std::to_string(value.object);
  if (value.many)
    out += " many";
  if (value.interior)
    out += " interior";
  if (value.raw)
    out += " raw";
  if (value.rawSome)
    out += " raw-some";
  for (const std::string &function : value.functions)
    out += " fn=" + encode(function);
  return out;
}

static std::optional<ValueDesc>
parseValue(const std::vector<std::string_view> &tokens) {
  if (tokens.empty())
    return std::nullopt;
  ValueDesc value;
  bool known = false;
  for (std::size_t k = 0; k < ValueKinds.size(); ++k)
    if (tokens[0] == ValueKinds.at(k)) {
      value.kind = static_cast<ValueDesc::Kind>(k);
      known = true;
    }
  if (!known)
    return std::nullopt;
  for (std::size_t i = 1; i < tokens.size(); ++i) {
    std::string_view token = tokens[i];
    if (token == "zeroed") {
      value.zeroed = true;
    } else if (token == "maybe-null") {
      value.maybeNull = true;
    } else if (token == "many") {
      value.many = true;
    } else if (token == "interior") {
      value.interior = true;
    } else if (token == "raw") {
      value.raw = true;
    } else if (token == "raw-some") {
      value.rawSome = true;
    } else if (auto family = valueOf(token, "family")) {
      auto decoded = decode(*family);
      if (!decoded)
        return std::nullopt;
      value.family = *decoded;
    } else if (auto extent = valueOf(token, "extent")) {
      value.extent = parseTerm(*extent);
      if (!value.extent)
        return std::nullopt;
    } else if (auto path = valueOf(token, "path")) {
      value.path = parsePath(*path);
      if (!value.path)
        return std::nullopt;
    } else if (auto offset = valueOf(token, "offset")) {
      value.offset = number<std::int64_t>(*offset);
      if (!value.offset)
        return std::nullopt;
    } else if (auto lo = valueOf(token, "lo")) {
      value.lo = number<std::int64_t>(*lo);
      if (!value.lo)
        return std::nullopt;
    } else if (auto hi = valueOf(token, "hi")) {
      value.hi = number<std::int64_t>(*hi);
      if (!value.hi)
        return std::nullopt;
    } else if (auto range = valueOf(token, "range")) {
      value.range = IntegerRange::parse(*range);
      if (!value.range || value.range->empty())
        return std::nullopt;
    } else if (auto object = valueOf(token, "object")) {
      auto index = number<std::uint32_t>(*object);
      if (!index)
        return std::nullopt;
      value.object = *index;
    } else if (auto function = valueOf(token, "fn")) {
      auto decoded = decode(*function);
      if (!decoded)
        return std::nullopt;
      value.functions.push_back(*decoded);
    } else {
      return std::nullopt;
    }
  }
  return value;
}

static constexpr std::array<const char *, 6> EffectKinds = {
    "release", "move", "unknown", "escape", "share-", "share+"};

//===----------------------------------------------------------------------===//
// Print and parse
//===----------------------------------------------------------------------===//

std::string printEffects(const FunctionEffects &effects) {
  std::string out;
  switch (effects.returns) {
  case FunctionEffects::Returns::Always:
    out += "returns always\n";
    break;
  case FunctionEffects::Returns::May:
    out += "returns may\n";
    break;
  case FunctionEffects::Returns::Never:
    out += "returns never\n";
    break;
  }
  if (effects.incomplete)
    out += "incomplete " + encode(*effects.incomplete) + "\n";
  if (effects.unknownGlobals)
    out += "unknown-globals\n";
  for (const PathEffect &effect : effects.effects) {
    out += "effect " +
           std::string(EffectKinds.at(static_cast<std::size_t>(effect.kind))) +
           " " + printPath(effect.path) + " when=" + printCase(effect.when);
    if (!effect.family.empty())
      out += " family=" + encode(effect.family);
    if (effect.may)
      out += " may";
    if (effect.lossy)
      out += " lossy";
    if (effect.anyOffset)
      out += " offset=?";
    else if (effect.offset != 0)
      out += " offset=" + std::to_string(effect.offset);
    if (effect.elements)
      out += " elements=" + printRange(*effect.elements);
    out += '\n';
  }
  for (const StoreEffect &store : effects.stores) {
    out += "store " + printPath(store.dest) + " when=" + printCase(store.when);
    if (store.may)
      out += " may";
    if (store.contents)
      out += " contents=" + std::to_string(*store.contents);
    if (store.elements)
      out += " elements=" + printRange(*store.elements);
    if (!store.absentOn.empty())
      out += " absent-on=" + printClasses(store.absentOn);
    if (store.bytes)
      out += " bytes=" + std::to_string(store.bytes->first) + ".." +
             std::to_string(store.bytes->second);
    out += " :: " + printValue(store.value) + "\n";
  }
  for (const ResultEffect &result : effects.results) {
    out += "result classes=" + printClasses(result.classes);
    if (result.paramZero)
      out += " param=" + printParamTest(result.paramZero);
    out += " :: " + printValue(result.value) + "\n";
  }
  for (const auto &[resultClass, paths] : effects.nonNullOn)
    for (const SummaryPath &path : paths)
      out += "nonnull-on " + std::string(toString(resultClass)) + " " +
             printPath(path) + "\n";
  for (const StringEffect &string : effects.strings) {
    out += "string " + printPath(string.path) +
           " nul-within=" + printTerm(string.nulWithin);
    if (string.nulFrom)
      out += " nul-from=" + printTerm(*string.nulFrom);
    if (string.contents)
      out += " contents=" + std::to_string(*string.contents);
    out += '\n';
  }
  for (const SummaryPath &path : effects.reads)
    out += "reads " + printPath(path) + "\n";
  for (const SummaryPath &path : effects.writes)
    out += "writes " + printPath(path) + "\n";
  return out;
}

std::optional<FunctionEffects> parseEffects(std::string_view text,
                                            std::string *error) {
  FunctionEffects effects;
  std::size_t lineNumber = 0;
  auto fail = [&](std::string_view what) -> std::optional<FunctionEffects> {
    if (error != nullptr)
      *error = "line " + std::to_string(lineNumber) + ": " + std::string(what);
    return std::nullopt;
  };
  bool sawReturns = false;
  for (std::string_view line : split(text, '\n')) {
    ++lineNumber;
    if (line.empty())
      continue;
    std::vector<std::string_view> tokens = split(line, ' ');
    // The value after `::`.
    std::vector<std::string_view> head = tokens;
    std::vector<std::string_view> tail;
    if (auto it = std::ranges::find(tokens, "::"); it != tokens.end()) {
      head.assign(tokens.begin(), it);
      tail.assign(it + 1, tokens.end());
    }
    std::string_view kind = head[0];
    if (kind == "returns" && head.size() == 2) {
      if (head[1] == "always")
        effects.returns = FunctionEffects::Returns::Always;
      else if (head[1] == "may")
        effects.returns = FunctionEffects::Returns::May;
      else if (head[1] == "never")
        effects.returns = FunctionEffects::Returns::Never;
      else
        return fail("unknown returns");
      sawReturns = true;
    } else if (kind == "incomplete" && head.size() == 2) {
      auto reason = decode(head[1]);
      if (!reason)
        return fail("malformed incomplete reason");
      effects.incomplete = *reason;
    } else if (kind == "unknown-globals" && head.size() == 1) {
      effects.unknownGlobals = true;
    } else if (kind == "effect" && head.size() >= 4) {
      PathEffect effect;
      bool known = false;
      for (std::size_t k = 0; k < EffectKinds.size(); ++k)
        if (head[1] == EffectKinds.at(k)) {
          effect.kind = static_cast<PathEffect::Kind>(k);
          known = true;
        }
      auto path = parsePath(head[2]);
      auto when = valueOf(head[3], "when");
      auto parsedCase = when ? parseCase(*when) : std::nullopt;
      if (!known || !path || !parsedCase)
        return fail("malformed effect");
      effect.path = std::move(*path);
      effect.when = std::move(*parsedCase);
      for (std::size_t i = 4; i < head.size(); ++i) {
        if (head[i] == "may") {
          effect.may = true;
        } else if (head[i] == "lossy") {
          effect.lossy = true;
        } else if (auto family = valueOf(head[i], "family")) {
          auto decoded = decode(*family);
          if (!decoded)
            return fail("malformed family");
          effect.family = *decoded;
        } else if (head[i] == "offset=?") {
          effect.anyOffset = true;
        } else if (auto offset = valueOf(head[i], "offset")) {
          auto value = number<std::int64_t>(*offset);
          if (!value)
            return fail("malformed offset");
          effect.offset = *value;
        } else if (auto range = valueOf(head[i], "elements")) {
          effect.elements = parseRange(*range);
          if (!effect.elements)
            return fail("malformed elements");
        } else {
          return fail("unknown effect attribute");
        }
      }
      effects.effects.push_back(std::move(effect));
    } else if (kind == "store" && head.size() >= 3) {
      StoreEffect store;
      auto dest = parsePath(head[1]);
      auto when = valueOf(head[2], "when");
      auto parsedCase = when ? parseCase(*when) : std::nullopt;
      auto value = parseValue(tail);
      if (!dest || !parsedCase || !value)
        return fail("malformed store");
      store.dest = std::move(*dest);
      store.when = std::move(*parsedCase);
      store.value = std::move(*value);
      for (std::size_t i = 3; i < head.size(); ++i) {
        if (head[i] == "may") {
          store.may = true;
        } else if (auto steps = valueOf(head[i], "contents")) {
          store.contents = number<std::uint32_t>(*steps);
          if (!store.contents)
            return fail("malformed contents");
        } else if (auto range = valueOf(head[i], "elements")) {
          store.elements = parseRange(*range);
          if (!store.elements)
            return fail("malformed elements");
        } else if (auto absent = valueOf(head[i], "absent-on")) {
          auto classes = parseClasses(*absent);
          if (!classes || classes->empty())
            return fail("malformed absent-on");
          store.absentOn = std::move(*classes);
        } else if (auto bytes = valueOf(head[i], "bytes")) {
          const std::size_t dots = bytes->find("..");
          auto from = dots == std::string_view::npos
                          ? std::nullopt
                          : number<std::int64_t>(bytes->substr(0, dots));
          auto to = dots == std::string_view::npos
                        ? std::nullopt
                        : number<std::int64_t>(bytes->substr(dots + 2));
          if (!from || !to || *from >= *to)
            return fail("malformed bytes");
          store.bytes = std::make_pair(*from, *to);
        } else {
          return fail("unknown store attribute");
        }
      }
      effects.stores.push_back(std::move(store));
    } else if (kind == "result" && head.size() >= 2) {
      ResultEffect result;
      auto classes = valueOf(head[1], "classes");
      auto parsedClasses = classes ? parseClasses(*classes) : std::nullopt;
      auto value = parseValue(tail);
      if (!parsedClasses || !value)
        return fail("malformed result");
      result.classes = std::move(*parsedClasses);
      result.value = std::move(*value);
      for (std::size_t i = 2; i < head.size(); ++i) {
        auto test = valueOf(head[i], "param");
        auto parsed = test ? parseParamTest(*test) : std::nullopt;
        if (!parsed)
          return fail("unknown result attribute");
        result.paramZero = *parsed;
      }
      effects.results.push_back(std::move(result));
    } else if (kind == "nonnull-on" && head.size() == 3) {
      auto resultClass = parseResultClass(head[1]);
      auto path = parsePath(head[2]);
      if (!resultClass || !path)
        return fail("malformed nonnull-on");
      effects.nonNullOn[*resultClass].push_back(std::move(*path));
    } else if (kind == "string" && head.size() >= 3) {
      StringEffect string;
      auto path = parsePath(head[1]);
      auto within = valueOf(head[2], "nul-within");
      auto term = within ? parseTerm(*within) : std::nullopt;
      if (!path || !term)
        return fail("malformed string");
      string.path = std::move(*path);
      string.nulWithin = std::move(*term);
      for (std::size_t i = 3; i < head.size(); ++i) {
        if (auto from = valueOf(head[i], "nul-from")) {
          string.nulFrom = parseTerm(*from);
          if (!string.nulFrom)
            return fail("malformed nul-from");
        } else if (auto steps = valueOf(head[i], "contents")) {
          string.contents = number<std::uint32_t>(*steps);
          if (!string.contents)
            return fail("malformed contents");
        } else {
          return fail("unknown string attribute");
        }
      }
      effects.strings.push_back(std::move(string));
    } else if ((kind == "reads" || kind == "writes") && head.size() == 2) {
      auto path = parsePath(head[1]);
      if (!path)
        return fail("malformed path");
      (kind == "reads" ? effects.reads : effects.writes)
          .push_back(std::move(*path));
    } else {
      return fail("unknown item");
    }
  }
  if (!sawReturns)
    return fail("no returns line");
  return effects;
}

//===----------------------------------------------------------------------===//
// Join
//===----------------------------------------------------------------------===//

FunctionEffects joinEffects(const FunctionEffects &left,
                            const FunctionEffects &rightIn) {
  // The two sides number their new objects separately: the right side's
  // come after the left's.
  std::uint32_t shift = 0;
  auto noteObject = [&](const ValueDesc &value) {
    if (value.kind == ValueDesc::Kind::Fresh)
      shift = std::max(shift, value.object + 1);
  };
  for (const ResultEffect &result : left.results)
    noteObject(result.value);
  for (const StoreEffect &store : left.stores)
    noteObject(store.value);
  FunctionEffects right = rightIn;
  for (ResultEffect &result : right.results)
    if (result.value.kind == ValueDesc::Kind::Fresh)
      result.value.object += shift;
  for (StoreEffect &store : right.stores)
    if (store.value.kind == ValueDesc::Kind::Fresh)
      store.value.object += shift;

  FunctionEffects out;
  out.returns = left.returns == right.returns ? left.returns
                                              : FunctionEffects::Returns::May;
  out.incomplete = left.incomplete ? left.incomplete : right.incomplete;
  out.unknownGlobals = left.unknownGlobals || right.unknownGlobals;
  // Effects: on both sides as is (possible if possible on either); on one
  // side only, possible.
  auto sameEffect = [](const PathEffect &a, const PathEffect &b) {
    return a.kind == b.kind && a.path == b.path && a.family == b.family &&
           a.when == b.when && a.offset == b.offset &&
           a.anyOffset == b.anyOffset && a.elements == b.elements;
  };
  for (const PathEffect &effect : left.effects) {
    PathEffect joined = effect;
    auto other =
        std::ranges::find_if(right.effects, [&](const PathEffect &candidate) {
          return sameEffect(effect, candidate);
        });
    if (other == right.effects.end()) {
      joined.may = true;
    } else {
      joined.may = effect.may || other->may;
      joined.lossy = effect.lossy || other->lossy;
    }
    out.effects.push_back(std::move(joined));
  }
  for (const PathEffect &effect : right.effects)
    if (std::ranges::none_of(left.effects, [&](const PathEffect &candidate) {
          return sameEffect(effect, candidate);
        })) {
      PathEffect joined = effect;
      joined.may = true;
      out.effects.push_back(std::move(joined));
    }
  // Stores: the same place on both sides keeps a value both agree on.
  auto samePlace = [](const StoreEffect &a, const StoreEffect &b) {
    return a.dest == b.dest && a.elements == b.elements &&
           a.contents == b.contents && a.bytes == b.bytes;
  };
  for (const StoreEffect &store : left.stores) {
    StoreEffect joined = store;
    auto other =
        std::ranges::find_if(right.stores, [&](const StoreEffect &candidate) {
          return samePlace(store, candidate);
        });
    if (other == right.stores.end()) {
      joined.may = true;
    } else {
      joined.may = store.may || other->may;
      if (!(store.value == other->value) || !(store.when == other->when)) {
        joined.value = ValueDesc{};
        joined.when = EffectCase{};
      }
    }
    out.stores.push_back(std::move(joined));
  }
  for (const StoreEffect &store : right.stores)
    if (std::ranges::none_of(left.stores, [&](const StoreEffect &candidate) {
          return samePlace(store, candidate);
        })) {
      StoreEffect joined = store;
      joined.may = true;
      out.stores.push_back(std::move(joined));
    }
  // Results: the alternatives of both.
  // A result alternative is kept once: the same up to which new object it
  // is (the right side's objects are renumbered after the left's, so a
  // widening join would otherwise add the same allocation each round), when
  // no store names that object.
  auto named = [&](const FunctionEffects &effects, std::uint32_t object) {
    return std::ranges::any_of(effects.stores, [&](const StoreEffect &store) {
      return store.value.kind == ValueDesc::Kind::Fresh &&
             store.value.object == object;
    });
  };
  auto sameUpToObject = [](const ResultEffect &a, const ResultEffect &b) {
    if (a.value.kind != ValueDesc::Kind::Fresh ||
        b.value.kind != ValueDesc::Kind::Fresh)
      return a == b;
    ResultEffect x = a;
    ResultEffect y = b;
    x.value.object = y.value.object = 0;
    return x == y;
  };
  out.results.clear();
  for (const ResultEffect &result : left.results)
    if (std::ranges::none_of(out.results, [&](const ResultEffect &kept) {
          return sameUpToObject(kept, result) &&
                 (kept == result || !named(left, result.value.object));
        }))
      out.results.push_back(result);
  for (const ResultEffect &result : right.results)
    if (std::ranges::none_of(out.results, [&](const ResultEffect &kept) {
          return kept == result || (sameUpToObject(kept, result) &&
                                    !named(right, result.value.object));
        }))
      out.results.push_back(result);
  // A guarantee both give.
  for (const auto &[resultClass, paths] : left.nonNullOn) {
    auto other = right.nonNullOn.find(resultClass);
    if (other == right.nonNullOn.end())
      continue;
    for (const SummaryPath &path : paths)
      if (std::ranges::find(other->second, path) != other->second.end())
        out.nonNullOn[resultClass].push_back(path);
  }
  auto unite = [](std::vector<SummaryPath> a,
                  const std::vector<SummaryPath> &b) {
    for (const SummaryPath &path : b)
      if (std::ranges::find(a, path) == a.end())
        a.push_back(path);
    return a;
  };
  out.reads = unite(left.reads, right.reads);
  out.writes = unite(left.writes, right.writes);
  // A string fact both sides give.
  for (const StringEffect &string : left.strings)
    if (std::ranges::find(right.strings, string) != right.strings.end())
      out.strings.push_back(string);
  return out;
}

//===----------------------------------------------------------------------===//
// Renumbering
//===----------------------------------------------------------------------===//

FunctionEffects widenEffects(const FunctionEffects &previous,
                             const FunctionEffects &next) {
  // An unknown effect on every case covers every object reachable from its
  // path (the caller havocs them, `Transfer::instantiate`): what else
  // either side says may happen below it is folded into it before the
  // join, or a recursion over a tree names each deeper path a round later.
  std::vector<SummaryPath> covering;
  for (const FunctionEffects *side : {&previous, &next})
    for (const PathEffect &effect : side->effects)
      if (effect.kind == PathEffect::Kind::Unknown && effect.when.always() &&
          !effect.elements)
        covering.push_back(effect.path);
  auto covered = [&](const SummaryPath &path, bool orEqual) {
    return std::ranges::any_of(covering, [&](const SummaryPath &cover) {
      return cover.isProperPrefixOf(path) || (orEqual && cover == path);
    });
  };
  auto fold = [&](FunctionEffects effects) {
    std::erase_if(effects.effects, [&](const PathEffect &effect) {
      bool foldable = effect.kind == PathEffect::Kind::Unknown ||
                      ((effect.kind == PathEffect::Kind::Release ||
                        effect.kind == PathEffect::Kind::Move) &&
                       effect.may);
      return foldable && covered(effect.path, false);
    });
    // (A store below one: the caller forgets it with the rest.)
    std::erase_if(effects.stores, [&](const StoreEffect &store) {
      return !store.value.raw && covered(store.dest, true);
    });
    return effects;
  };
  FunctionEffects out = joinEffects(fold(previous), fold(next));
  // The integer alternatives of one case, as one hull.
  using Case = std::pair<std::vector<ResultClass>,
                         std::optional<std::pair<std::uint32_t, bool>>>;
  auto hulls = [](const std::vector<ResultEffect> &results) {
    std::map<Case, ValueDesc> out;
    for (const ResultEffect &result : results) {
      if (result.value.kind != ValueDesc::Kind::Int)
        continue;
      Case key{result.classes, result.paramZero};
      auto [it, inserted] = out.try_emplace(key, result.value);
      if (inserted)
        continue;
      ValueDesc &hull = it->second;
      hull.lo = hull.lo && result.value.lo
                    ? std::optional(std::min(*hull.lo, *result.value.lo))
                    : std::nullopt;
      hull.hi = hull.hi && result.value.hi
                    ? std::optional(std::max(*hull.hi, *result.value.hi))
                    : std::nullopt;
      if (!(hull.range == result.value.range))
        hull.range.reset();
      if (!(hull.path == result.value.path))
        hull.path.reset();
    }
    return out;
  };
  const std::map<Case, ValueDesc> before = hulls(previous.results);
  std::map<Case, ValueDesc> after = hulls(out.results);
  for (auto &[key, hull] : after) {
    auto old = before.find(key);
    if (old == before.end())
      continue;
    // (A bound that moved since the last round does not stop moving.)
    if (!old->second.lo || !hull.lo || *hull.lo < *old->second.lo)
      hull.lo.reset();
    if (!old->second.hi || !hull.hi || *hull.hi > *old->second.hi)
      hull.hi.reset();
    if (!(old->second.range == hull.range))
      hull.range.reset();
  }
  std::vector<ResultEffect> results;
  std::set<Case> placed;
  for (const ResultEffect &result : out.results) {
    if (result.value.kind != ValueDesc::Kind::Int) {
      results.push_back(result);
      continue;
    }
    Case key{result.classes, result.paramZero};
    if (!placed.insert(key).second)
      continue;
    ResultEffect merged = result;
    merged.value = after.at(key);
    results.push_back(std::move(merged));
  }
  out.results = std::move(results);
  // New objects numbered by first appearance, so two rounds that differ
  // only by how the join numbered them compare equal.
  std::map<std::uint32_t, std::uint32_t> number;
  auto renumber = [&](ValueDesc &value) {
    if (value.kind != ValueDesc::Kind::Fresh)
      return;
    auto [it, inserted] = number.try_emplace(
        value.object, static_cast<std::uint32_t>(number.size()));
    value.object = it->second;
  };
  for (ResultEffect &result : out.results)
    renumber(result.value);
  for (StoreEffect &store : out.stores)
    renumber(store.value);
  return out;
}

FunctionEffects renumberGlobals(const FunctionEffects &effects,
                                const GlobalRenumbering &map) {
  // A path with its global root renumbered, or none.
  auto path = [&](const SummaryPath &in) -> std::optional<SummaryPath> {
    if (in.root != SummaryRoot::Global)
      return in;
    auto id = map(in.index);
    if (!id)
      return std::nullopt;
    SummaryPath out = in;
    out.index = *id;
    return out;
  };
  auto term = [&](const PathTerm &in) -> std::optional<PathTerm> {
    if (!in.path)
      return in;
    auto renamed = path(*in.path);
    if (!renamed)
      return std::nullopt;
    PathTerm out = in;
    out.path = std::move(renamed);
    return out;
  };
  auto range = [&](const std::optional<ElementRange> &in,
                   bool &ok) -> std::optional<ElementRange> {
    if (!in)
      return in;
    auto from = term(in->from);
    auto to = term(in->to);
    if (!from || !to) {
      ok = false;
      return std::nullopt;
    }
    return ElementRange{.from = std::move(*from), .to = std::move(*to)};
  };
  auto value = [&](const ValueDesc &in) {
    ValueDesc out = in;
    if (in.path) {
      out.path = path(*in.path);
      if (!out.path)
        return ValueDesc{};
    }
    if (in.extent)
      out.extent = term(*in.extent);
    return out;
  };
  // A case's entry test through a global the unit cannot name: dropped (the
  // effect becomes a possible one).
  auto when = [&](EffectCase in, bool &may) {
    if (in.entryZero) {
      auto renamed = path(in.entryZero->first);
      if (renamed) {
        in.entryZero->first = std::move(*renamed);
      } else {
        in.entryZero.reset();
        may = true;
      }
    }
    return in;
  };
  FunctionEffects out;
  out.returns = effects.returns;
  out.incomplete = effects.incomplete;
  out.unknownGlobals = effects.unknownGlobals;
  for (const PathEffect &effect : effects.effects) {
    auto renamed = path(effect.path);
    bool ok = true;
    auto elements = range(effect.elements, ok);
    if (!renamed || !ok) {
      if (!out.incomplete)
        out.incomplete = "an effect through a global this unit does not "
                         "declare";
      continue;
    }
    PathEffect copy = effect;
    copy.path = std::move(*renamed);
    copy.elements = std::move(elements);
    copy.when = when(copy.when, copy.may);
    out.effects.push_back(std::move(copy));
  }
  for (const StoreEffect &store : effects.stores) {
    auto renamed = path(store.dest);
    bool ok = true;
    auto elements = range(store.elements, ok);
    // A store into a global this unit cannot name still writes it: the
    // unit that can learns so from the summaries that call this one.
    if (!renamed || !ok) {
      out.unknownGlobals = true;
      continue;
    }
    StoreEffect copy = store;
    copy.dest = std::move(*renamed);
    copy.elements = std::move(elements);
    copy.value = value(store.value);
    copy.when = when(copy.when, copy.may);
    out.stores.push_back(std::move(copy));
  }
  for (const ResultEffect &result : effects.results) {
    ResultEffect copy = result;
    copy.value = value(result.value);
    out.results.push_back(std::move(copy));
  }
  for (const auto &[resultClass, paths] : effects.nonNullOn)
    for (const SummaryPath &in : paths)
      if (auto renamed = path(in))
        out.nonNullOn[resultClass].push_back(std::move(*renamed));
  for (const StringEffect &string : effects.strings) {
    auto renamed = path(string.path);
    auto within = term(string.nulWithin);
    std::optional<PathTerm> from;
    if (string.nulFrom) {
      from = term(*string.nulFrom);
      if (!from)
        continue;
    }
    if (!renamed || !within)
      continue;
    StringEffect copy = string;
    copy.path = std::move(*renamed);
    copy.nulWithin = std::move(*within);
    copy.nulFrom = std::move(from);
    out.strings.push_back(std::move(copy));
  }
  for (const SummaryPath &in : effects.reads)
    if (auto renamed = path(in))
      out.reads.push_back(std::move(*renamed));
  for (const SummaryPath &in : effects.writes)
    if (auto renamed = path(in))
      out.writes.push_back(std::move(*renamed));
  return out;
}

} // namespace weavec::core
