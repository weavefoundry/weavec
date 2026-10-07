//===- Effects.cpp - Function summaries (RFC 0031) ------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Effects.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace weavec::core {

/// A path with its root spelled: `paramN`, `globalN` or `result`.
static std::string pathText(const SummaryPath &path) {
  switch (path.root) {
  case SummaryRoot::Param:
    return path.toString("param" + std::to_string(path.index));
  case SummaryRoot::Global:
    return path.toString("global" + std::to_string(path.index));
  case SummaryRoot::Result:
    return path.toString("result");
  }
  return path.toString("?");
}

std::string_view toString(ResultClass value) noexcept {
  switch (value) {
  case ResultClass::Null:
    return "null";
  case ResultClass::NonNull:
    return "nonnull";
  case ResultClass::Zero:
    return "zero";
  case ResultClass::Positive:
    return "positive";
  case ResultClass::Negative:
    return "negative";
  }
  return "?";
}

/// `null|nonnull`, for the store text.
static std::string classesText(const std::vector<ResultClass> &classes) {
  std::string out;
  for (std::size_t i = 0; i < classes.size(); ++i)
    out += (i == 0 ? "" : "|") + std::string(core::toString(classes[i]));
  return out;
}

std::string EffectCase::toString() const {
  if (always())
    return "always";
  std::string out;
  if (!classes.empty()) {
    out = "result";
    for (std::size_t i = 0; i < classes.size(); ++i) {
      out += i == 0 ? " " : "|";
      out += core::toString(classes[i]);
    }
  }
  if (paramZero)
    out += std::string(out.empty() ? "" : " and ") + "param " +
           std::to_string(paramZero->first) +
           (paramZero->second ? " =0" : " !=0");
  if (entryZero)
    out += std::string(out.empty() ? "" : " and ") + "entry " +
           pathText(entryZero->first) + (entryZero->second ? " =0" : " !=0");
  if (paramsEqual)
    out += std::string(out.empty() ? "" : " and ") + "param " +
           std::to_string(paramsEqual->first) +
           (paramsEqual->equal ? " == " : " != ") + "param " +
           std::to_string(paramsEqual->second);
  return out;
}

std::string PathTerm::toString() const {
  if (!path)
    return std::to_string(constant);
  std::string out = pathText(*path);
  if (scale != 1)
    out += " scale " + std::to_string(scale);
  if (constant != 0)
    out += " plus " + std::to_string(constant);
  return out;
}

std::string ElementRange::toString() const {
  return "[" + from.toString() + ", " + to.toString() + ")";
}

std::string ValueDesc::toString() const {
  switch (kind) {
  case Kind::Null:
    return "null";
  case Kind::Fresh:
    return "fresh#" + std::to_string(object) + " " +
           (family.empty() ? std::string("free") : family) +
           (extent ? " extent " + extent->toString() : std::string()) +
           (zeroed ? " zeroed" : "") + (many ? " many" : "") +
           (maybeNull ? " maybe-null" : "") + (interior ? " interior" : "") +
           (offset && *offset != 0 ? " offset " + std::to_string(*offset)
                                   : std::string());
  case Kind::Path:
    return "path " + (path ? pathText(*path) : std::string("?")) +
           (offset && *offset != 0 ? " offset " + std::to_string(*offset)
                                   : std::string()) +
           (maybeNull ? " maybe-null" : "");
  case Kind::Static:
    return "static";
  case Kind::Int:
    return "int [" + (lo ? std::to_string(*lo) : std::string("-inf")) + ", " +
           (hi ? std::to_string(*hi) : std::string("inf")) + "]" +
           (range ? " " + range->toString() : std::string());
  case Kind::Dangling:
    return "dangling";
  case Kind::Unknown:
    if (!raw)
      return "unknown";
    return rawSome ? "unknown raw-some" : "unknown raw";
  case Kind::Function: {
    std::string out = "function";
    for (std::size_t i = 0; i < functions.size(); ++i)
      out += (i == 0 ? " " : ",") + functions[i];
    return out;
  }
  }
  return "?";
}

std::string toText(const FunctionEffects &effects) {
  std::string out;
  switch (effects.returns) {
  case FunctionEffects::Returns::Always:
    out += "  always-returns\n";
    break;
  case FunctionEffects::Returns::May:
    out += "  may-not-return\n";
    break;
  case FunctionEffects::Returns::Never:
    out += "  never-returns\n";
    break;
  }
  if (effects.incomplete)
    out += "  incomplete " + *effects.incomplete + "\n";
  if (effects.unknownGlobals)
    out += "  unknown globals\n";
  for (const ResultEffect &result : effects.results) {
    out += "  result " + result.value.toString();
    if (result.value.maybeNull && result.value.kind != ValueDesc::Kind::Fresh)
      out += " maybe-null";
    out += " when";
    for (ResultClass c : result.classes)
      out += " " + std::string(toString(c));
    if (result.paramZero)
      out += " and param " + std::to_string(result.paramZero->first) +
             (result.paramZero->second ? " =0" : " !=0");
    out += '\n';
  }
  for (const PathEffect &effect : effects.effects) {
    static constexpr std::array<const char *, 6> Names = {
        "release", "move", "unknown", "escape", "share -1", "share +1"};
    out += "  " + std::string(Names.at(static_cast<std::size_t>(effect.kind))) +
           " " + pathText(effect.path);
    if (effect.elements)
      out += " elements " + effect.elements->toString();
    if (!effect.family.empty())
      out += " " + effect.family;
    if (effect.anyOffset)
      out += " offset unknown";
    else if (effect.offset != 0)
      out += " offset " + std::to_string(effect.offset);
    if (effect.lossy)
      out += " lossy";
    if (effect.may)
      out += " may";
    out += " when " + effect.when.toString() + "\n";
  }
  for (const StoreEffect &store : effects.stores)
    out +=
        "  store " + std::string(store.contents ? "(new) " : "") +
        pathText(store.dest) +
        (store.elements ? " elements " + store.elements->toString()
                        : std::string()) +
        (store.bytes ? " bytes " + std::to_string(store.bytes->first) + ".." +
                           std::to_string(store.bytes->second)
                     : std::string()) +
        " := " + store.value.toString() + (store.may ? " may" : "") +
        (store.when.always() ? std::string()
                             : " when " + store.when.toString()) +
        (store.absentOn.empty() ? std::string()
                                : " absent on " + classesText(store.absentOn)) +
        "\n";
  for (const auto &[resultClass, paths] : effects.nonNullOn)
    for (const SummaryPath &path : paths)
      out += "  nonnull-on " + std::string(toString(resultClass)) + " " +
             pathText(path) + "\n";
  for (const StringEffect &string : effects.strings)
    out += "  string " + std::string(string.contents ? "(new) " : "") +
           pathText(string.path) + " nul-within " +
           string.nulWithin.toString() +
           (string.nulFrom ? " from " + string.nulFrom->toString()
                           : std::string()) +
           "\n";
  return out;
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
