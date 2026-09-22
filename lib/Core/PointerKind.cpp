//===- PointerKind.cpp - The pointer-kind lattice (RFC 0030) --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/PointerKind.h"

#include <algorithm>
#include <charconv>
#include <system_error>
#include <tuple>
#include <utility>

namespace weavec::core {

std::string_view toString(PointerShape shape) noexcept {
  switch (shape) {
  case PointerShape::Single:
    return "single";
  case PointerShape::Counted:
    return "counted";
  case PointerShape::Sized:
    return "sized";
  case PointerShape::EndedBy:
    return "ended-by";
  case PointerShape::NulTerminated:
    return "nul-terminated";
  case PointerShape::Unknown:
    return "unknown";
  }
  return "<invalid>";
}

std::string_view toString(Nullability nullability) noexcept {
  switch (nullability) {
  case Nullability::Nonnull:
    return "nonnull";
  case Nullability::Nullable:
    return "nullable";
  }
  return "<invalid>";
}

std::string_view toString(KindSource source) noexcept {
  switch (source) {
  case KindSource::Declared:
    return "declared";
  case KindSource::Inferred:
    return "inferred";
  case KindSource::Default:
    return "default";
  }
  return "<invalid>";
}

std::string_view toString(ExtentClass extentClass) noexcept {
  switch (extentClass) {
  case ExtentClass::Exact:
    return "exact";
  case ExtentClass::Declared:
    return "declared";
  case ExtentClass::LowerBound:
    return "lower-bound";
  }
  return "<invalid>";
}

std::optional<Nullability> parseNullability(std::string_view text) {
  if (text == "nonnull")
    return Nullability::Nonnull;
  if (text == "nullable")
    return Nullability::Nullable;
  return std::nullopt;
}

std::optional<KindSource> parseKindSource(std::string_view text) {
  if (text == "declared")
    return KindSource::Declared;
  if (text == "inferred")
    return KindSource::Inferred;
  if (text == "default")
    return KindSource::Default;
  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// Paths and terms
//===----------------------------------------------------------------------===//

template <typename Integer>
static bool parseInteger(std::string_view text, Integer &value) {
  if (text.empty())
    return false;
  const auto [end, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return ec == std::errc() && end == text.data() + text.size();
}

static bool isIdentifier(std::string_view text) noexcept {
  if (text.empty())
    return false;
  const auto isStart = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  const auto isContinue = [&](char c) {
    return isStart(c) || (c >= '0' && c <= '9');
  };
  return isStart(text.front()) &&
         std::all_of(text.begin() + 1, text.end(), isContinue);
}

/// Splits on single spaces; an empty token (a doubled, leading or trailing
/// space) makes the whole text malformed.
static std::optional<std::vector<std::string_view>>
splitTokens(std::string_view text) {
  std::vector<std::string_view> tokens;
  std::size_t start = 0;
  while (true) {
    const std::size_t space = text.find(' ', start);
    const std::string_view token = text.substr(
        start, space == std::string_view::npos ? std::string_view::npos
                                               : space - start);
    if (token.empty())
      return std::nullopt;
    tokens.push_back(token);
    if (space == std::string_view::npos)
      return tokens;
    start = space + 1;
  }
}

ExtentPath ExtentPath::ofParam(std::uint32_t index) {
  return ExtentPath{.root = Root::Param, .param = index};
}

ExtentPath ExtentPath::ofField(std::string name) {
  return ExtentPath{.root = Root::Field, .field = std::move(name)};
}

std::string ExtentPath::toString() const {
  if (root == Root::Param)
    return "param " + std::to_string(param);
  return "." + field;
}

/// Parses a path from its tokens: `param` and an index, or `.<field>`.
static std::optional<ExtentPath>
parsePathTokens(std::span<const std::string_view> tokens) {
  if (tokens.size() == 2 && tokens[0] == "param") {
    std::uint32_t index = 0;
    if (!parseInteger(tokens[1], index))
      return std::nullopt;
    return ExtentPath::ofParam(index);
  }
  if (tokens.size() == 1 && tokens[0].size() > 1 && tokens[0].front() == '.' &&
      isIdentifier(tokens[0].substr(1)))
    return ExtentPath::ofField(std::string(tokens[0].substr(1)));
  return std::nullopt;
}

std::optional<ExtentPath> ExtentPath::parse(std::string_view text) {
  const auto tokens = splitTokens(text);
  if (!tokens)
    return std::nullopt;
  return parsePathTokens(*tokens);
}

ExtentTerm ExtentTerm::constant(std::int64_t value) {
  return ExtentTerm{.path = std::nullopt, .scale = 1, .offset = value};
}

ExtentTerm ExtentTerm::of(ExtentPath path, std::int64_t scale,
                          std::int64_t offset) {
  return ExtentTerm{.path = std::move(path), .scale = scale, .offset = offset};
}

std::string ExtentTerm::toString() const {
  if (!path)
    return std::to_string(offset);
  return path->toString() + " scale " + std::to_string(scale) + " plus " +
         std::to_string(offset);
}

std::optional<ExtentTerm> ExtentTerm::parse(std::string_view text) {
  const auto tokens = splitTokens(text);
  if (!tokens)
    return std::nullopt;
  const std::span<const std::string_view> all(*tokens);
  std::int64_t value = 0;
  if (all.size() == 1 && parseInteger(all[0], value))
    return constant(value);
  // `<path> scale <k> plus <c>`: the path is everything before `scale`.
  if (all.size() >= 5 && all[all.size() - 4] == "scale" &&
      all[all.size() - 2] == "plus") {
    std::int64_t scale = 0;
    std::int64_t offset = 0;
    const auto path = parsePathTokens(all.first(all.size() - 4));
    if (!path || !parseInteger(all[all.size() - 3], scale) ||
        !parseInteger(all[all.size() - 1], offset))
      return std::nullopt;
    return of(*path, scale, offset);
  }
  if (const auto path = parsePathTokens(all))
    return of(*path);
  return std::nullopt;
}

bool operator==(const ExtentTerm &a, const ExtentTerm &b) noexcept {
  if (a.path != b.path || a.offset != b.offset)
    return false;
  return !a.path || a.scale == b.scale;
}

//===----------------------------------------------------------------------===//
// Kinds
//===----------------------------------------------------------------------===//

PointerKind PointerKind::single(Nullability nullability, KindSource source) {
  return PointerKind{.shape = PointerShape::Single,
                     .nullability = nullability,
                     .source = source};
}

PointerKind PointerKind::counted(ExtentTerm count, Nullability nullability,
                                 KindSource source) {
  return PointerKind{.shape = PointerShape::Counted,
                     .extent = std::move(count),
                     .nullability = nullability,
                     .source = source};
}

PointerKind PointerKind::sized(ExtentTerm bytes, Nullability nullability,
                               KindSource source) {
  return PointerKind{.shape = PointerShape::Sized,
                     .extent = std::move(bytes),
                     .nullability = nullability,
                     .source = source};
}

PointerKind PointerKind::endedBy(ExtentPath end, std::int64_t offset,
                                 Nullability nullability, KindSource source) {
  return PointerKind{.shape = PointerShape::EndedBy,
                     .extent = ExtentTerm::of(std::move(end), 1, offset),
                     .nullability = nullability,
                     .source = source};
}

PointerKind PointerKind::nulTerminated(Nullability nullability,
                                       KindSource source) {
  return PointerKind{.shape = PointerShape::NulTerminated,
                     .nullability = nullability,
                     .source = source};
}

PointerKind PointerKind::unknown(Nullability nullability, KindSource source) {
  return PointerKind{.shape = PointerShape::Unknown,
                     .nullability = nullability,
                     .source = source};
}

bool PointerKind::sameShape(const PointerKind &other) const noexcept {
  return shape == other.shape && (!hasExtent(shape) || extent == other.extent);
}

std::string PointerKind::toString() const {
  std::string text(core::toString(shape));
  if (hasExtent(shape)) {
    text += '(';
    if (shape == PointerShape::EndedBy && extent.path && extent.scale == 1 &&
        extent.offset == 0)
      text += extent.path->toString();
    else
      text += extent.toString();
    text += ')';
  }
  text += ' ';
  text += core::toString(nullability);
  return text;
}

std::optional<PointerKind> PointerKind::parse(std::string_view text,
                                              KindSource source) {
  const std::size_t space = text.rfind(' ');
  if (space == std::string_view::npos)
    return std::nullopt;
  const auto nullability = parseNullability(text.substr(space + 1));
  if (!nullability)
    return std::nullopt;
  const std::string_view shape = text.substr(0, space);
  if (shape == "single")
    return single(*nullability, source);
  if (shape == "nul-terminated")
    return nulTerminated(*nullability, source);
  if (shape == "unknown")
    return unknown(*nullability, source);
  const std::size_t open = shape.find('(');
  if (open == std::string_view::npos || shape.back() != ')')
    return std::nullopt;
  const std::string_view name = shape.substr(0, open);
  const auto term =
      ExtentTerm::parse(shape.substr(open + 1, shape.size() - open - 2));
  if (!term)
    return std::nullopt;
  if (name == "counted")
    return counted(*term, *nullability, source);
  if (name == "sized")
    return sized(*term, *nullability, source);
  if (name == "ended-by" && term->path)
    return PointerKind{.shape = PointerShape::EndedBy,
                       .extent = *term,
                       .nullability = *nullability,
                       .source = source};
  return std::nullopt;
}

bool operator==(const PointerKind &a, const PointerKind &b) noexcept {
  return a.sameShape(b) && a.nullability == b.nullability &&
         a.source == b.source;
}

//===----------------------------------------------------------------------===//
// Join and conjunction
//===----------------------------------------------------------------------===//

/// Default < Inferred < Declared.
static int sourceStrength(KindSource source) noexcept {
  switch (source) {
  case KindSource::Default:
    return 0;
  case KindSource::Inferred:
    return 1;
  case KindSource::Declared:
    return 2;
  }
  return 0;
}

KindSource join(KindSource a, KindSource b) noexcept {
  return sourceStrength(a) <= sourceStrength(b) ? a : b;
}

PointerKind join(const PointerKind &a, const PointerKind &b,
                 const JoinFacts &facts) {
  PointerKind result = PointerKind::unknown(join(a.nullability, b.nullability),
                                            join(a.source, b.source));
  if (a.sameShape(b)) {
    result.shape = a.shape;
    result.extent = a.extent;
    return result;
  }
  const auto atLeastOne = [&](const ExtentTerm &term) {
    return (term.isConstant() && term.offset >= 1) ||
           (facts.provesAtLeastOne && facts.provesAtLeastOne(term));
  };
  const bool singleAndCounted =
      (a.shape == PointerShape::Single && b.shape == PointerShape::Counted &&
       atLeastOne(b.extent)) ||
      (b.shape == PointerShape::Single && a.shape == PointerShape::Counted &&
       atLeastOne(a.extent));
  if (singleAndCounted)
    result.shape = PointerShape::Single;
  return result;
}

/// The canonical order of shape requirements.
static bool requirementLess(const PointerKind &a, const PointerKind &b) {
  return std::forward_as_tuple(a.shape, a.extent.toString(),
                               sourceStrength(a.source)) <
         std::forward_as_tuple(b.shape, b.extent.toString(),
                               sourceStrength(b.source));
}

void KindRequirements::add(const PointerKind &requirement) {
  if (requirement.nullability == Nullability::Nonnull)
    required = Nullability::Nonnull;
  if (requirement.shape != PointerShape::Unknown) {
    PointerKind entry = requirement;
    if (!hasExtent(entry.shape))
      entry.extent = ExtentTerm{};
    bool merged = false;
    for (PointerKind &existing : entries) {
      if (existing.sameShape(entry)) {
        // Equal requirements: the stronger source is enforced more widely.
        if (sourceStrength(entry.source) > sourceStrength(existing.source))
          existing.source = entry.source;
        merged = true;
        break;
      }
      // Two constant terms of one shape and source: the larger implies the
      // smaller.
      if (existing.shape == entry.shape && hasExtent(entry.shape) &&
          entry.shape != PointerShape::EndedBy &&
          existing.extent.isConstant() && entry.extent.isConstant() &&
          existing.source == entry.source) {
        existing.extent.offset =
            std::max(existing.extent.offset, entry.extent.offset);
        merged = true;
        break;
      }
    }
    if (!merged)
      entries.push_back(std::move(entry));
  }
  for (PointerKind &entry : entries)
    entry.nullability = required;
  std::ranges::sort(entries, requirementLess);
}

void KindRequirements::add(const KindRequirements &other) {
  if (other.required == Nullability::Nonnull)
    add(PointerKind::unknown(Nullability::Nonnull));
  for (const PointerKind &entry : other.entries)
    add(entry);
}

std::string KindRequirements::toString() const {
  if (entries.empty())
    return "unknown " + std::string(core::toString(required));
  std::string text;
  for (const PointerKind &entry : entries) {
    if (!text.empty())
      text += " & ";
    text += entry.toString();
  }
  return text;
}

std::optional<KindRequirements> KindRequirements::parse(std::string_view text,
                                                        KindSource source) {
  KindRequirements requirements;
  std::size_t start = 0;
  while (true) {
    const std::size_t separator = text.find(" & ", start);
    const auto kind = PointerKind::parse(
        text.substr(start, separator == std::string_view::npos
                               ? std::string_view::npos
                               : separator - start),
        source);
    if (!kind)
      return std::nullopt;
    requirements.add(*kind);
    if (separator == std::string_view::npos)
      return requirements;
    start = separator + 3;
  }
}

KindRequirements conjoin(const KindRequirements &a, const KindRequirements &b) {
  KindRequirements result = a;
  result.add(b);
  return result;
}

std::optional<ExtentClass> extentClassOf(const PointerKind &kind) noexcept {
  switch (kind.shape) {
  case PointerShape::Unknown:
  case PointerShape::NulTerminated:
    return std::nullopt;
  case PointerShape::Single:
    return ExtentClass::LowerBound;
  case PointerShape::Counted:
  case PointerShape::Sized:
  case PointerShape::EndedBy:
    return kind.source == KindSource::Declared ? ExtentClass::Declared
                                               : ExtentClass::LowerBound;
  }
  return std::nullopt;
}

} // namespace weavec::core
