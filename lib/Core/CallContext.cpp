//===- CallContext.cpp - Portable caller identity contexts ----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/CallContext.h"

#include "weavec/Core/AliasRelation.h"
#include "weavec/Core/Array.h"

#include <algorithm>

namespace weavec::core {

static bool validContextPath(const SummaryPath &path) {
  if ((!path.isParam() && !path.isGlobal()) ||
      path.steps.size() > MaxHeapPathDepth)
    return false;
  for (const auto &step : path.steps) {
    if (step.step == PathStep::Field) {
      if (step.field.empty())
        return false;
      for (std::size_t i = 0; i < step.field.size(); ++i) {
        const auto c = static_cast<unsigned char>(step.field[i]);
        const bool identifierCharacter =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
            c >= 128 || (i != 0 && c >= '0' && c <= '9');
        if (!identifierCharacter)
          return false;
      }
    } else if (step.step == PathStep::Index && !step.field.empty()) {
      const auto index = ArrayIndex::parse(step.field);
      if (!index || index->toString() != step.field)
        return false;
    } else if (step.step != PathStep::Index &&
               (step.step != PathStep::Deref || !step.field.empty())) {
      return false;
    }
  }
  return true;
}

bool CallContext::addAlias(ContextAlias alias) {
  if (alias.first == alias.second)
    return alias.offset.isZero() && alias.sameShare;
  if (alias.second < alias.first) {
    std::swap(alias.first, alias.second);
    alias.offset = alias.offset.negated();
  }
  for (const auto &existing : aliases)
    if (existing.first == alias.first && existing.second == alias.second)
      return existing == alias;
  if (aliases.size() + facts.size() + separations.size() + orders.size() >=
      MaxCallContextFacts)
    return false;
  const auto [it, inserted] = aliases.insert(std::move(alias));
  if (valid())
    return true;
  if (inserted)
    aliases.erase(it);
  return false;
}

bool CallContext::valid() const {
  if (empty() ||
      aliases.size() + facts.size() + separations.size() + orders.size() >
          MaxCallContextFacts ||
      callbacks.size() > MaxCallbackContexts)
    return false;
  std::set<SummaryPath> paths;
  std::set<std::pair<SummaryPath, SummaryPath>> pairs;
  std::map<SummaryPath, PlaceId> ids;
  const auto idOf = [&ids](const SummaryPath &path) {
    return ids
        .try_emplace(path, PlaceId{static_cast<std::uint32_t>(ids.size())})
        .first->second;
  };
  AliasRelation definite;
  AliasRelation sameShares;
  for (const auto &[path, targets] : callbacks) {
    if (!validContextPath(path) || !path.isParam() || targets.empty() ||
        CallTargets::parse(targets.toString()) != targets)
      return false;
  }
  for (const auto &alias : aliases) {
    if (!(alias.first < alias.second) || !validContextPath(alias.first) ||
        !validContextPath(alias.second) ||
        !pairs.emplace(alias.first, alias.second).second ||
        PointerOffset::parse(alias.offset.toString()) != alias.offset)
      return false;
    paths.insert(alias.first);
    paths.insert(alias.second);
    if (alias.definite) {
      const auto a = idOf(alias.first);
      const auto b = idOf(alias.second);
      const auto previous = definite.offsetOf(b, a);
      if (previous && !previous->isIndefinite() &&
          !alias.offset.isIndefinite() && *previous != alias.offset)
        return false;
      definite.unite(a, b, alias.offset);
      if (alias.sameShare)
        sameShares.unite(a, b);
    }
    if (alias.definite && alias.offset.isZero()) {
      const auto a = facts.find(alias.first);
      const auto b = facts.find(alias.second);
      if (a != facts.end() && b != facts.end() &&
          a->second.disjointFrom(b->second))
        return false;
    }
  }
  for (const auto &[a, b] : separations) {
    if (!(a < b) || !validContextPath(a) || !validContextPath(b) ||
        definite.mayAlias(idOf(a), idOf(b)) || pairs.contains({a, b}))
      return false;
    paths.insert(a);
    paths.insert(b);
  }
  auto ordered = orders;
  for (const auto &[a, b] : orders) {
    if (a == b || !validContextPath(a) || !validContextPath(b) ||
        !definite.mayAlias(idOf(a), idOf(b)))
      return false;
    paths.insert(a);
    paths.insert(b);
    for (const auto &path : {a, b})
      if (const auto fact = facts.find(path);
          fact != facts.end() && (!fact->second.isPointer() ||
                                  fact->second.classes.contains(Outcome::Null)))
        return false;
  }
  if (paths.size() > MaxCallContextPaths)
    return false;
  // Close at most 32 paths, then reject an order contradicted by an exact
  // displacement, including contradictions reached through other orders.
  if (!orders.empty())
    for (const auto &middle : paths)
      for (const auto &a : paths)
        for (const auto &b : paths)
          if (ordered.contains({a, middle}) && ordered.contains({middle, b}))
            ordered.emplace(a, b);
  for (const auto &[a, b] : ordered)
    if (const auto offset = definite.offsetOf(idOf(b), idOf(a));
        offset &&
        ((offset->isElements() && offset->elements > 0) || offset->isField()))
      return false;
  for (const auto &alias : aliases)
    if (alias.definite && !alias.sameShare &&
        sameShares.mayAlias(idOf(alias.first), idOf(alias.second)))
      return false;
  for (const auto &[path, fact] : facts) {
    if (!validContextPath(path) || fact.classes.empty() || fact.trivial() ||
        ValueFact::parse(fact.toString()) != fact)
      return false;
    if (fact.isPointer())
      paths.insert(path);
    for (const auto &[other, otherFact] : facts)
      if (path < other && definite.isExact(idOf(path), idOf(other)) &&
          fact.disjointFrom(otherFact))
        return false;
  }
  return paths.size() <= MaxCallContextPaths;
}

std::optional<CallContext> remapCallContext(const CallContext &context,
                                            const GlobalIdMap &map) {
  if (!context.valid())
    return std::nullopt;
  const auto pathOf = [&map](SummaryPath path) -> std::optional<SummaryPath> {
    if (path.isGlobal()) {
      const auto id = map(path.index);
      if (!id)
        return std::nullopt;
      path.index = *id;
    }
    return path;
  };
  CallContext result;
  result.reportDiagnostics = context.reportDiagnostics;
  result.callbacks = context.callbacks;
  for (const auto &alias : context.aliases) {
    const auto a = pathOf(alias.first);
    const auto b = pathOf(alias.second);
    if (!a || !b || a == b ||
        !result.addAlias({.first = *a,
                          .second = *b,
                          .offset = alias.offset,
                          .definite = alias.definite,
                          .sameShare = alias.sameShare}))
      return std::nullopt;
  }
  for (const auto &[a, b] : context.separations) {
    const auto first = pathOf(a);
    const auto second = pathOf(b);
    if (!first || !second || first == second)
      return std::nullopt;
    result.separations.insert(std::minmax(*first, *second));
  }
  for (const auto &[path, fact] : context.facts) {
    const auto mapped = pathOf(path);
    if (!mapped || !result.facts.emplace(*mapped, fact).second)
      return std::nullopt;
  }
  for (const auto &[a, b] : context.orders) {
    const auto first = pathOf(a);
    const auto second = pathOf(b);
    if (!first || !second || first == second ||
        !result.orders.emplace(*first, *second).second)
      return std::nullopt;
  }
  return result.valid() ? std::optional(result) : std::nullopt;
}

std::set<SummaryPath> callMemoryFootprint(const FunctionSummary &summary) {
  std::set<SummaryPath> result;
  const auto visit = [&result](const SummaryPath &path, bool value) {
    if (path.isResult())
      return;
    SummaryPath prefix = path.rootPath();
    for (const auto &step : path.steps) {
      if (step.step == PathStep::Deref)
        result.insert(prefix);
      prefix.steps.push_back(step);
    }
    if (value)
      result.insert(path);
  };
  const auto expression = [&visit](const auto &value) {
    for (const auto &node : value.all())
      if (node.key)
        visit(*node.key, true);
  };
  const auto numericGuard = [&expression](const PathGuard &guard) {
    for (const auto &predicate : guard.integers) {
      expression(predicate.lhs);
      expression(predicate.rhs);
    }
  };
  const auto affine = [&expression, &visit](const PathAffine &value) {
    if (value.expression)
      expression(*value.expression);
    else if (value.path)
      visit(*value.path, true);
  };
  const auto numericSource = [&numericGuard,
                              &affine](const ValueSource &value) {
    numericGuard(value.when);
    if (value.extent)
      affine(*value.extent);
    if (value.stringLength)
      affine(*value.stringLength);
  };
  for (const auto &[path, effect] : summary.effects) {
    visit(path, effect.consumed() || effect.read);
    numericGuard(effect.when);
    for (const auto &[condition, fact] : effect.when.conditions) {
      (void)fact;
      visit(condition, true);
    }
    for (const auto &[pair, equal] : effect.when.pointers) {
      (void)equal;
      visit(pair.first, true);
      visit(pair.second, true);
    }
  }
  for (const auto &store : summary.stores) {
    visit(store.dest, false);
    if (store.value.path)
      visit(*store.value.path, true);
    numericSource(store.value);
  }
  for (const auto &value : summary.returns)
    numericSource(value);
  for (const auto &[root, graph] : summary.heap)
    for (const auto &field : graph.fields)
      numericSource(field.value);
  for (const auto &[path, outputs] : summary.numericOutputs) {
    visit(path, true);
    for (const auto &output : outputs) {
      numericGuard(output.when);
      if (output.value)
        expression(*output.value);
    }
  }
  for (const auto &[param, requirements] : summary.requiresExtent)
    for (const auto &requirement : requirements) {
      numericGuard(requirement.when);
      affine(requirement.need);
      if (requirement.start)
        affine(*requirement.start);
    }
  return result;
}

static std::string encodeContextText(std::string_view text) {
  static constexpr std::string_view Digits = "0123456789abcdef";
  std::string encoded;
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    encoded += Digits[byte >> 4U];
    encoded += Digits[byte & 15U];
  }
  return encoded;
}

static std::optional<std::string> decodeContextText(std::string_view text) {
  if (text.empty() || text.size() % 2 != 0 || text.size() > 32768)
    return std::nullopt;
  const auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
  };
  std::string result;
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int hi = digit(text[i]);
    const int lo = digit(text[i + 1]);
    if (hi < 0 || lo < 0)
      return std::nullopt;
    const char c = static_cast<char>((hi * 16) + lo);
    if (c == '\0' || c == '\n' || c == '\r')
      return std::nullopt;
    result += c;
  }
  return result;
}

std::string printCallContext(const CallContext &context,
                             const GlobalNamer &names) {
  const auto path = [&names](const SummaryPath &p) {
    return encodeContextText(printSummaryPath(p, names));
  };
  std::string result;
  const auto append = [&result](const std::string &record) {
    if (!result.empty())
      result += ';';
    result += record;
  };
  append(context.reportDiagnostics ? "r:1" : "r:0");
  if (!context.callbacks.empty())
    append("c:" + encodeContextText(printCallbackBindings(context.callbacks)));
  for (const auto &alias : context.aliases)
    append("a:" + path(alias.first) + ':' + path(alias.second) + ':' +
           encodeContextText(alias.offset.toString()) + ':' +
           (alias.definite ? "1" : "0") + (alias.sameShare ? "1" : "0"));
  for (const auto &[a, b] : context.separations)
    append("d:" + path(a) + ':' + path(b));
  for (const auto &[a, b] : context.orders)
    append("o:" + path(a) + ':' + path(b));
  for (const auto &[p, fact] : context.facts)
    append("v:" + path(p) + ':' + encodeContextText(fact.toString()));
  return result;
}

std::optional<CallContext> parseCallContext(std::string_view text,
                                            const GlobalResolver &resolve) {
  if (text.empty() || text.size() > 262144)
    return std::nullopt;
  const auto pathOf =
      [&resolve](std::string_view token) -> std::optional<SummaryPath> {
    const auto decoded = decodeContextText(token);
    if (!decoded)
      return std::nullopt;
    const auto path = parseSummaryPath(*decoded, resolve);
    return path && validContextPath(*path) ? path : std::nullopt;
  };
  CallContext result;
  bool haveReporting = false;
  std::size_t records = 0;
  while (!text.empty()) {
    if (++records > MaxCallContextFacts + 2)
      return std::nullopt;
    const auto end = text.find(';');
    std::string_view record = text.substr(0, end);
    std::vector<std::string_view> fields;
    for (;;) {
      const auto colon = record.find(':');
      fields.push_back(record.substr(0, colon));
      if (colon == std::string_view::npos)
        break;
      record.remove_prefix(colon + 1);
    }
    if (fields[0] == "r" && fields.size() == 2 && !haveReporting &&
        (fields[1] == "0" || fields[1] == "1")) {
      result.reportDiagnostics = fields[1] == "1";
      haveReporting = true;
    } else if (fields[0] == "c" && fields.size() == 2) {
      const auto decoded = decodeContextText(fields[1]);
      const auto callbacks =
          decoded ? parseCallbackBindings(*decoded) : std::nullopt;
      if (!callbacks || !result.callbacks.empty())
        return std::nullopt;
      result.callbacks = *callbacks;
    } else if (fields[0] == "a" && fields.size() == 5) {
      const auto a = pathOf(fields[1]);
      const auto b = pathOf(fields[2]);
      const auto decoded = decodeContextText(fields[3]);
      const auto offset =
          decoded ? PointerOffset::parse(*decoded) : std::nullopt;
      if (!a || !b || !offset || !(*a < *b) || fields[4].size() != 2 ||
          fields[4].find_first_not_of("01") != std::string_view::npos)
        return std::nullopt;
      const auto count = result.aliases.size();
      if (!result.addAlias({.first = *a,
                            .second = *b,
                            .offset = *offset,
                            .definite = fields[4][0] == '1',
                            .sameShare = fields[4][1] == '1'}) ||
          count == result.aliases.size())
        return std::nullopt;
    } else if (fields[0] == "d" && fields.size() == 3) {
      const auto a = pathOf(fields[1]);
      const auto b = pathOf(fields[2]);
      if (!a || !b || !(*a < *b) || !result.separations.emplace(*a, *b).second)
        return std::nullopt;
    } else if (fields[0] == "o" && fields.size() == 3) {
      const auto a = pathOf(fields[1]);
      const auto b = pathOf(fields[2]);
      if (!a || !b || a == b || !result.orders.emplace(*a, *b).second)
        return std::nullopt;
    } else if (fields[0] == "v" && fields.size() == 3) {
      const auto path = pathOf(fields[1]);
      const auto decoded = decodeContextText(fields[2]);
      const auto fact = decoded ? ValueFact::parse(*decoded) : std::nullopt;
      if (!path || !fact || !result.facts.emplace(*path, *fact).second)
        return std::nullopt;
    } else {
      return std::nullopt;
    }
    if (end == std::string_view::npos)
      break;
    text.remove_prefix(end + 1);
    if (text.empty())
      return std::nullopt;
  }
  return haveReporting && result.valid() ? std::optional(result) : std::nullopt;
}

} // namespace weavec::core
