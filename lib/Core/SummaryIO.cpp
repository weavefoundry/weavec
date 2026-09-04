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
  std::string text;
  switch (path.root) {
  case SummaryRoot::Param:
    text = "param " + std::to_string(path.index);
    break;
  case SummaryRoot::Global:
    text = "global " + names(path.index);
    break;
  case SummaryRoot::Result:
    text = "result";
    break;
  }
  if (!path.steps.empty()) {
    text += ' ';
    text += printSteps(path);
  }
  return text;
}

std::string printValueSource(const ValueSource &source,
                             const GlobalNamer &names) {
  std::string text(source.kind == ValueSource::Kind::Copy && source.interior
                       ? std::string_view("interior")
                       : toString(source.kind));
  if (source.kind == ValueSource::Kind::Fresh && !source.family.empty())
    text += '(' + source.family + ')';
  if ((source.kind == ValueSource::Kind::Copy ||
       source.kind == ValueSource::Kind::Borrow) &&
      source.path) {
    text += ' ';
    text += printSummaryPath(*source.path, names);
  }
  return text;
}

std::string printFlags(const PlaceEffect &effect) {
  std::string flags;
  const auto add = [&flags, &effect](bool set, const char *name,
                                     bool withFamily) {
    if (!set)
      return;
    if (!flags.empty())
      flags += ',';
    flags += name;
    if (withFamily && !effect.family.empty())
      flags += '(' + effect.family + ')';
  };
  add(effect.read, "read", false);
  add(effect.written, "written", false);
  add(effect.freed, "freed", true);
  add(effect.moved, "moved", true);
  add(effect.consumed() && effect.replaced, "replaced", false);
  add(effect.consumed() && effect.element, "element", false);
  return flags;
}

/// Splits `name(family)` into its parts; `family` is left empty for a bare
/// name. Returns false on a malformed spelling (`name(`, `name()x`).
static bool splitFamily(std::string_view token, std::string_view &name,
                        std::string_view &family) {
  const std::size_t open = token.find('(');
  if (open == std::string_view::npos) {
    name = token;
    family = {};
    return true;
  }
  if (token.back() != ')' || open + 1 >= token.size() - 1)
    return false;
  name = token.substr(0, open);
  family = token.substr(open + 1, token.size() - open - 2);
  return family.find_first_of("(), \t") == std::string_view::npos;
}

std::string printGuard(const PathGuard &guard, const GlobalNamer &names) {
  std::string text;
  for (const auto &[path, fact] : guard.conditions) {
    text += text.empty() ? " when " : " and ";
    text += printSummaryPath(path, names) + ' ' + fact.toString();
  }
  return text;
}

std::string printSummary(const FunctionSummary &summary,
                         const GlobalNamer &names) {
  std::string text = "summary\n";
  if (summary.neverReturns)
    text += "  never-returns\n";
  for (const auto &[path, effect] : summary.effects) {
    if (effect.empty())
      continue;
    text += "  effect " + printSummaryPath(path, names) + ' ' +
            printFlags(effect) + printGuard(effect.when, names) + '\n';
  }
  for (const Store &store : summary.stores) {
    text += "  store " + printSummaryPath(store.dest, names) + ' ' +
            printValueSource(store.value, names) +
            printGuard(store.value.when, names) + '\n';
  }
  for (const ValueSource &source : summary.returns) {
    text += "  return " + printValueSource(source, names) +
            printGuard(source.when, names) + '\n';
  }
  for (const auto &[outcome, effects] : summary.outcomes) {
    // A class with effects is implied by its effect lines; a bare line
    // records a class that is possible but consumes nothing.
    bool printed = false;
    for (const auto &[path, effect] : effects) {
      if (effect.empty())
        continue;
      text += "  outcome " + std::string(toString(outcome)) + ' ' +
              printSummaryPath(path, names) + ' ' + printFlags(effect) +
              printGuard(effect.when, names) + '\n';
      printed = true;
    }
    if (!printed)
      text += "  outcome " + std::string(toString(outcome)) + '\n';
  }
  for (const auto &[outcome, paths] : summary.nullOn) {
    for (const SummaryPath &path : paths) {
      text += "  null " + std::string(toString(outcome)) + ' ' +
              printSummaryPath(path, names) + '\n';
    }
  }
  for (const auto &[outcome, paths] : summary.nonNullOn) {
    for (const SummaryPath &path : paths) {
      text += "  notnull " + std::string(toString(outcome)) + ' ' +
              printSummaryPath(path, names) + '\n';
    }
  }
  for (const std::uint32_t param : summary.requiresNonNull)
    text += "  requires " + std::to_string(param) + '\n';
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

/// Parses `param N [steps]`, `global NAME [steps]` or `result [steps]`.
/// Returns false on a malformed path; a declined global yields `result.path
/// == nullopt`.
static bool parsePath(Tokens &tokens, const GlobalResolver &resolve,
                      ParsedPath &result) {
  const std::string_view root = tokens.take();
  SummaryPath path;
  bool declined = false;
  if (root == "result") {
    path = SummaryPath::result();
    if (looksLikeSteps(tokens.peek()) && !parseSteps(tokens.take(), path))
      return false;
    result.path = std::move(path);
    return true;
  }
  const std::string_view name = tokens.take();
  if (name.empty())
    return false;
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
  std::string_view kind;
  std::string_view family;
  if (!splitFamily(tokens.take(), kind, family))
    return false;
  // Only `fresh` carries a family.
  if (kind != "fresh" && !family.empty())
    return false;
  if (kind == "fresh") {
    source = ValueSource::fresh(std::string(family));
  } else if (kind == "null") {
    source = ValueSource::null();
  } else if (kind == "unknown") {
    source = ValueSource::unknown();
  } else if (kind == "raw") {
    source = ValueSource::raw();
  } else if (kind == "copy" || kind == "interior" || kind == "borrow") {
    ParsedPath path;
    if (!parsePath(tokens, resolve, path))
      return false;
    if (!path.path)
      source = ValueSource::unknown();
    else if (kind == "copy")
      source = ValueSource::copy(std::move(*path.path));
    else if (kind == "interior")
      source = ValueSource::interiorCopy(std::move(*path.path));
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
    const std::string_view token = text.substr(
        pos,
        comma == std::string_view::npos ? std::string_view::npos : comma - pos);
    std::string_view flag;
    std::string_view family;
    if (!splitFamily(token, flag, family))
      return false;
    if (!family.empty() && flag != "freed" && flag != "moved")
      return false;
    if (flag == "read")
      effect.read = true;
    else if (flag == "written")
      effect.written = true;
    else if (flag == "freed")
      effect.freed = true;
    else if (flag == "moved")
      effect.moved = true;
    else if (flag == "replaced")
      effect.replaced = true;
    else if (flag == "element")
      effect.element = true;
    else
      return false;
    if (!family.empty()) {
      // `freed(free),moved(fclose)` cannot describe one consume; a
      // disagreement is "unknown", as in `PlaceEffect::join`.
      if (effect.family.empty())
        effect.family = std::string(family);
      else if (effect.family != family)
        effect.family.clear();
    }
    if (comma == std::string_view::npos)
      break;
    pos = comma + 1;
  }
  // `replaced` and `element` qualify a consume; alone they describe nothing.
  if ((effect.replaced || effect.element) && !effect.consumed())
    return false;
  return !effect.empty();
}

/// Parses an optional trailing `when <path> <fact> [and <path> <fact>]...`
/// (RFC 0009). A conjunct on a declined global is dropped (the guard
/// weakens).
static bool parseGuard(Tokens &tokens, const GlobalResolver &resolve,
                       PathGuard &guard) {
  if (tokens.empty())
    return true;
  if (tokens.take() != "when")
    return false;
  while (true) {
    ParsedPath path;
    if (!parsePath(tokens, resolve, path))
      return false;
    const std::optional<ValueFact> fact = ValueFact::parse(tokens.take());
    if (!fact)
      return false;
    if (path.path)
      guard.require(*path.path, *fact);
    if (tokens.empty())
      return true;
    if (tokens.take() != "and")
      return false;
  }
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
    if (kind == "never-returns") {
      summary.neverReturns = true;
    } else if (kind == "effect") {
      ParsedPath path;
      PlaceEffect effect;
      ok = parsePath(tokens, resolve, path) &&
           parseFlags(tokens.take(), effect) &&
           parseGuard(tokens, resolve, effect.when);
      // A guard qualifies a consume; it says nothing about a read.
      if (ok && !effect.when.trivial() && !effect.consumed())
        ok = false;
      if (ok && path.path)
        summary.addEffect(*path.path, effect);
    } else if (kind == "store") {
      ParsedPath dest;
      ValueSource value;
      ok = parsePath(tokens, resolve, dest) &&
           parseSource(tokens, resolve, value) &&
           parseGuard(tokens, resolve, value.when);
      if (ok && dest.path)
        summary.addStore(Store{.dest = std::move(*dest.path), .value = value});
    } else if (kind == "return") {
      ValueSource value;
      ok = parseSource(tokens, resolve, value) &&
           parseGuard(tokens, resolve, value.when);
      if (ok)
        summary.addReturn(value);
    } else if (kind == "outcome") {
      const std::optional<Outcome> outcome = parseOutcome(tokens.take());
      if (!outcome) {
        ok = false;
      } else if (tokens.empty()) {
        summary.addOutcome(*outcome);
      } else {
        ParsedPath path;
        PlaceEffect effect;
        ok = parsePath(tokens, resolve, path) &&
             parseFlags(tokens.take(), effect) &&
             parseGuard(tokens, resolve, effect.when);
        if (ok && !effect.when.trivial() && !effect.consumed())
          ok = false;
        summary.addOutcome(*outcome);
        if (ok && path.path)
          summary.addOutcome(*outcome, *path.path, effect);
      }
    } else if (kind == "null") {
      const std::optional<Outcome> outcome = parseOutcome(tokens.take());
      ParsedPath path;
      ok = outcome.has_value() && parsePath(tokens, resolve, path);
      if (ok && path.path) {
        summary.addOutcome(*outcome);
        summary.nullOn[*outcome].insert(*path.path);
      }
    } else if (kind == "notnull") {
      const std::optional<Outcome> outcome = parseOutcome(tokens.take());
      ParsedPath path;
      ok = outcome.has_value() && parsePath(tokens, resolve, path);
      if (ok && path.path) {
        summary.addOutcome(*outcome);
        summary.nonNullOn[*outcome].insert(*path.path);
      }
    } else if (kind == "requires") {
      const std::string_view index = tokens.take();
      std::uint32_t param = 0;
      const auto [end, ec] =
          std::from_chars(index.data(), index.data() + index.size(), param);
      ok = !index.empty() && ec == std::errc() &&
           end == index.data() + index.size();
      if (ok)
        summary.requiresNonNull.insert(param);
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
