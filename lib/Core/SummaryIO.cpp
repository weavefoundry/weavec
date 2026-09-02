//===- SummaryIO.cpp - Text form of function summaries --------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/SummaryIO.h"

#include <charconv>
#include <string>
#include <utility>
#include <vector>

namespace weavec::core {

// -- Printing -----------------------------------------------------------------

static std::string printSteps(const SummaryPath &path) {
  std::string steps;
  for (const PathElem &elem : path.steps) {
    switch (elem.step) {
    case PathStep::Deref:
      steps += '*';
      break;
    case PathStep::Field:
      steps += '.';
      steps += elem.field;
      break;
    case PathStep::Index:
      steps += "[]";
      break;
    }
  }
  return steps;
}

std::string printSummaryPath(const SummaryPath &path,
                             const GlobalNamer &names) {
  std::string text = path.isParam() ? "param " + std::to_string(path.index)
                                    : "global " + names(path.index);
  if (!path.steps.empty()) {
    text += ' ';
    text += printSteps(path);
  }
  return text;
}

std::string printValueSource(const ValueSource &source,
                             const GlobalNamer &names) {
  std::string text(toString(source.kind));
  if ((source.kind == ValueSource::Kind::Copy ||
       source.kind == ValueSource::Kind::Borrow) &&
      source.path) {
    text += ' ';
    text += printSummaryPath(*source.path, names);
  }
  return text;
}

static std::string printFlags(const PlaceEffect &effect) {
  std::string flags;
  const auto add = [&flags](bool set, const char *name) {
    if (!set)
      return;
    if (!flags.empty())
      flags += ',';
    flags += name;
  };
  add(effect.read, "read");
  add(effect.written, "written");
  add(effect.freed, "freed");
  add(effect.moved, "moved");
  return flags;
}

std::string printSummary(const FunctionSummary &summary,
                         const GlobalNamer &names) {
  std::string text = "summary\n";
  for (const auto &[path, effect] : summary.effects) {
    if (effect.empty())
      continue;
    text += "  effect " + printSummaryPath(path, names) + ' ' +
            printFlags(effect) + '\n';
  }
  for (const Store &store : summary.stores) {
    text += "  store " + printSummaryPath(store.dest, names) + ' ' +
            printValueSource(store.value, names) + '\n';
  }
  for (const ValueSource &source : summary.returns)
    text += "  return " + printValueSource(source, names) + '\n';
  if (summary.reallocLike)
    text += "  realloc-like\n";
  text += "end\n";
  return text;
}

// -- Parsing ------------------------------------------------------------------

namespace {

/// Whitespace-separated tokens of one line, consumed left to right.
class Tokens {
public:
  explicit Tokens(std::string_view line) {
    std::size_t pos = 0;
    while (pos < line.size()) {
      while (pos < line.size() && isSpace(line[pos]))
        ++pos;
      const std::size_t start = pos;
      while (pos < line.size() && !isSpace(line[pos]))
        ++pos;
      if (pos > start)
        items.push_back(line.substr(start, pos - start));
    }
  }

  [[nodiscard]] bool empty() const noexcept { return next >= items.size(); }
  [[nodiscard]] std::string_view peek() const noexcept {
    return empty() ? std::string_view() : items[next];
  }
  std::string_view take() noexcept {
    return empty() ? std::string_view() : items[next++];
  }

private:
  std::vector<std::string_view> items;
  std::size_t next = 0;

  static bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r';
  }
};

/// A parsed path, or the marker that its global root was declined.
struct ParsedPath {
  std::optional<SummaryPath> path;
};

} // namespace

static bool parseSteps(std::string_view text, SummaryPath &path) {
  std::size_t pos = 0;
  while (pos < text.size()) {
    switch (text[pos]) {
    case '*':
      path.steps.push_back(PathElem{.step = PathStep::Deref, .field = {}});
      ++pos;
      break;
    case '[':
      if (pos + 1 >= text.size() || text[pos + 1] != ']')
        return false;
      path.steps.push_back(PathElem{.step = PathStep::Index, .field = {}});
      pos += 2;
      break;
    case '.': {
      const std::size_t start = ++pos;
      while (pos < text.size() && text[pos] != '*' && text[pos] != '.' &&
             text[pos] != '[')
        ++pos;
      if (pos == start)
        return false;
      path.steps.push_back(
          PathElem{.step = PathStep::Field,
                   .field = std::string(text.substr(start, pos - start))});
      break;
    }
    default:
      return false;
    }
  }
  return true;
}

static bool looksLikeSteps(std::string_view token) noexcept {
  return !token.empty() &&
         (token[0] == '*' || token[0] == '.' || token[0] == '[');
}

/// Parses `param N [steps]` or `global NAME [steps]`. Returns false on a
/// malformed path; a declined global yields `result.path == nullopt`.
static bool parsePath(Tokens &tokens, const GlobalResolver &resolve,
                      ParsedPath &result) {
  const std::string_view root = tokens.take();
  const std::string_view name = tokens.take();
  if (name.empty())
    return false;
  SummaryPath path;
  bool declined = false;
  if (root == "param") {
    std::uint32_t index = 0;
    const auto [end, ec] =
        std::from_chars(name.data(), name.data() + name.size(), index);
    if (ec != std::errc() || end != name.data() + name.size())
      return false;
    path = SummaryPath::param(index);
  } else if (root == "global") {
    if (const auto id = resolve(name))
      path = SummaryPath::global(*id);
    else
      declined = true;
  } else {
    return false;
  }
  if (looksLikeSteps(tokens.peek()) && !parseSteps(tokens.take(), path))
    return false;
  if (!declined)
    result.path = std::move(path);
  return true;
}

/// Parses a value source. A declined global makes the source `unknown`.
static bool parseSource(Tokens &tokens, const GlobalResolver &resolve,
                        ValueSource &source) {
  const std::string_view kind = tokens.take();
  if (kind == "fresh") {
    source = ValueSource::fresh();
  } else if (kind == "null") {
    source = ValueSource::null();
  } else if (kind == "unknown") {
    source = ValueSource::unknown();
  } else if (kind == "raw") {
    source = ValueSource::raw();
  } else if (kind == "copy" || kind == "borrow") {
    ParsedPath path;
    if (!parsePath(tokens, resolve, path))
      return false;
    if (!path.path)
      source = ValueSource::unknown();
    else if (kind == "copy")
      source = ValueSource::copy(std::move(*path.path));
    else
      source = ValueSource::borrow(std::move(*path.path));
  } else {
    return false;
  }
  return true;
}

static bool parseFlags(std::string_view text, PlaceEffect &effect) {
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const std::size_t comma = text.find(',', pos);
    const std::string_view flag = text.substr(
        pos,
        comma == std::string_view::npos ? std::string_view::npos : comma - pos);
    if (flag == "read")
      effect.read = true;
    else if (flag == "written")
      effect.written = true;
    else if (flag == "freed")
      effect.freed = true;
    else if (flag == "moved")
      effect.moved = true;
    else
      return false;
    if (comma == std::string_view::npos)
      break;
    pos = comma + 1;
  }
  return !effect.empty();
}

static std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() &&
         (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
    text.remove_prefix(1);
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
    text.remove_suffix(1);
  return text;
}

std::optional<FunctionSummary> parseSummary(std::string_view record,
                                            const GlobalResolver &resolve,
                                            std::string *error) {
  const auto fail = [error](std::string message) {
    if (error != nullptr)
      *error = std::move(message);
    return std::nullopt;
  };

  FunctionSummary summary;
  bool open = false;
  bool closed = false;
  std::size_t lineNumber = 0;
  std::size_t pos = 0;
  while (pos <= record.size()) {
    const std::size_t newline = record.find('\n', pos);
    const std::string_view line = trim(record.substr(
        pos, newline == std::string_view::npos ? std::string_view::npos
                                               : newline - pos));
    pos = newline == std::string_view::npos ? record.size() + 1 : newline + 1;
    ++lineNumber;
    if (line.empty())
      continue;
    if (closed)
      return fail("line " + std::to_string(lineNumber) + ": text after 'end'");

    Tokens tokens(line);
    const std::string_view kind = tokens.take();
    if (!open) {
      if (kind != "summary")
        return fail("line " + std::to_string(lineNumber) +
                    ": expected 'summary'");
      open = true;
      continue;
    }
    if (kind == "end") {
      closed = true;
      continue;
    }
    bool ok = true;
    if (kind == "effect") {
      ParsedPath path;
      PlaceEffect effect;
      ok =
          parsePath(tokens, resolve, path) && parseFlags(tokens.take(), effect);
      if (ok && path.path)
        summary.addEffect(*path.path, effect);
    } else if (kind == "store") {
      ParsedPath dest;
      ValueSource value;
      ok = parsePath(tokens, resolve, dest) &&
           parseSource(tokens, resolve, value);
      if (ok && dest.path)
        summary.addStore(Store{.dest = std::move(*dest.path), .value = value});
    } else if (kind == "return") {
      ValueSource value;
      ok = parseSource(tokens, resolve, value);
      if (ok)
        summary.addReturn(value);
    } else if (kind == "realloc-like") {
      summary.reallocLike = true;
    } else {
      // Unknown line kinds are skipped for forward compatibility.
      continue;
    }
    if (!ok || !tokens.empty())
      return fail("line " + std::to_string(lineNumber) + ": malformed '" +
                  std::string(kind) + "' line");
  }
  if (!open)
    return fail("empty record");
  if (!closed)
    return fail("missing 'end'");
  return summary;
}

} // namespace weavec::core
