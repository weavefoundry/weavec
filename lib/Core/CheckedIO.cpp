//===- CheckedIO.cpp - Bounded checked contract transport ----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/CheckedIO.h"

#include "weavec/Core/Format.h"
#include "weavec/Core/ObjectType.h"

#include <charconv>
#include <limits>
#include <utility>

namespace weavec::core {

static constexpr std::size_t MaxRecordBytes = 4UL * 1024 * 1024;
static constexpr std::size_t MaxFieldBytes = 65536;

static bool hasResult(const PathAffine &value) {
  if (value.path && value.path->isResult())
    return true;
  return value.expression &&
         std::ranges::any_of(value.expression->all(), [](const auto &node) {
           return node.key && node.key->isResult();
         });
}

// NOLINTNEXTLINE(misc-use-internal-linkage): project namespace convention
class CheckedWriter {
public:
  void text(std::string_view value) {
    if (value.size() > MaxFieldBytes ||
        bytes.size() + value.size() + 16 > MaxRecordBytes) {
      valid = false;
      return;
    }
    bytes += std::to_string(value.size()) + ':';
    bytes += value;
  }
  template <typename T>
  void number(T value) {
    text(std::to_string(value));
  }
  void location(const SourceLocation &value) {
    text(value.file);
    number(value.line);
    number(value.column);
  }
  void affine(const PathAffine &value, const GlobalNamer &names) {
    text(value.path ? printSummaryPath(*value.path, names) : "");
    number(value.scale);
    number(value.constant);
    text(value.expression
             ? value.expression->toString([&](const SummaryPath &path) {
                 return printSummaryPath(path, names);
               })
             : "");
    number(static_cast<unsigned>(value.quantity));
  }
  void requirement(const CheckedRequirement &value, const GlobalNamer &names) {
    text(toString(value.kind));
    text(printSummaryPath(value.path, names));
    text(printSummaryPath(value.other, names));
    affine(value.begin, names);
    affine(value.end, names);
    text(value.family);
    text(printGuard(value.when, names));
    text(value.on ? toString(*value.on) : "");
    number(value.ifNonNull);
  }
  [[nodiscard]] bool good() const { return valid; }
  [[nodiscard]] std::string finish() const {
    static constexpr std::string_view Hex = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const char value : bytes) {
      const auto byte = static_cast<unsigned char>(value);
      result += Hex[byte >> 4U];
      result += Hex[byte & 15U];
    }
    return result;
  }

private:
  bool valid = true;
  std::string bytes;
};

// NOLINTNEXTLINE(misc-use-internal-linkage): project namespace convention
class CheckedReader {
public:
  explicit CheckedReader(std::string_view encoded) {
    if (encoded.size() > MaxRecordBytes * 2 || encoded.size() % 2 != 0) {
      valid = false;
      return;
    }
    const auto digit = [](char c) -> int {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      return -1;
    };
    bytes.reserve(encoded.size() / 2);
    for (std::size_t i = 0; i < encoded.size(); i += 2) {
      const int high = digit(encoded[i]);
      const int low = digit(encoded[i + 1]);
      if (high < 0 || low < 0) {
        valid = false;
        return;
      }
      bytes += static_cast<char>((high * 16) + low);
    }
  }
  std::string_view text() {
    if (!valid)
      return {};
    const std::size_t colon = bytes.find(':', position);
    if (colon == std::string::npos || colon == position ||
        colon - position > 10) {
      valid = false;
      return {};
    }
    std::size_t count = 0;
    const auto parsed =
        std::from_chars(bytes.data() + position, bytes.data() + colon, count);
    if (parsed.ec != std::errc{} || parsed.ptr != bytes.data() + colon ||
        count > MaxFieldBytes || count > bytes.size() - colon - 1) {
      valid = false;
      return {};
    }
    position = colon + 1 + count;
    return std::string_view(bytes).substr(colon + 1, count);
  }
  template <typename T>
  T number() {
    const auto value = text();
    T result{};
    if (value.empty()) {
      valid = false;
      return result;
    }
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size())
      valid = false;
    return result;
  }
  bool flag() {
    const auto value = number<unsigned>();
    if (value > 1)
      valid = false;
    return value == 1;
  }
  SourceLocation location() {
    SourceLocation result;
    result.file = text();
    result.line = number<std::uint32_t>();
    result.column = number<std::uint32_t>();
    return result;
  }
  PathAffine affine(const GlobalResolver &resolve) {
    PathAffine result;
    const auto path = text();
    if (!path.empty()) {
      result.path = parseSummaryPath(path, resolve);
      valid &= result.path.has_value();
    }
    result.scale = number<std::int64_t>();
    result.constant = number<std::int64_t>();
    const auto expression = text();
    if (!expression.empty()) {
      result.expression = IntegerExpression<SummaryPath>::parse(
          expression, [&](std::string_view name) {
            return parseSummaryPath(name, resolve);
          });
      valid &= result.expression.has_value() && !result.path;
    }
    const auto quantity = number<unsigned>();
    valid &= quantity <= static_cast<unsigned>(AffineQuantity::Terminator);
    if (quantity <= static_cast<unsigned>(AffineQuantity::Terminator))
      result.quantity = static_cast<AffineQuantity>(quantity);
    if (result.quantity == AffineQuantity::Terminator)
      valid &=
          result.path.has_value() && !result.expression && !hasResult(result);
    return result;
  }
  CheckedRequirement requirement(const GlobalResolver &resolve) {
    CheckedRequirement result;
    const auto kind = parseCheckedRequirementKind(text());
    const auto path = parseSummaryPath(text(), resolve);
    const auto other = parseSummaryPath(text(), resolve);
    valid &= kind.has_value() && path.has_value() && other.has_value();
    if (kind)
      result.kind = *kind;
    if (path)
      result.path = *path;
    if (other)
      result.other = *other;
    result.begin = affine(resolve);
    result.end = affine(resolve);
    result.family = text();
    if (result.kind == CheckedRequirementKind::StandardStream)
      valid &= result.path == SummaryPath{} && result.other == SummaryPath{} &&
               result.begin == PathAffine::ofConstant(0) &&
               result.end == PathAffine::ofConstant(0) &&
               result.family == "stdout";
    if (result.kind == CheckedRequirementKind::FormatArguments) {
      const auto literal = decodeFormatLiteral(result.family);
      valid &= result.path.isParam() && result.path.isRoot() &&
               result.begin.isConstant() && result.begin.constant >= -1 &&
               std::cmp_less_equal(result.begin.constant, MaxFormatArguments) &&
               result.end == PathAffine::ofConstant(0) &&
               (result.family.empty() ||
                (literal && OutputFormat::parse(*literal).valid())) &&
               (result.begin.constant != -1 ||
                (result.other.isParam() && result.other.isRoot())) &&
               (result.begin.constant == -1 || result.other == SummaryPath{});
    }
    if (result.kind == CheckedRequirementKind::ArgumentList ||
        result.kind == CheckedRequirementKind::ArgumentListConsumed)
      valid &= result.path.isParam() && result.path.isRoot() &&
               result.other == SummaryPath{} && result.family.empty() &&
               result.begin == PathAffine::ofConstant(0) &&
               result.end == PathAffine::ofConstant(0);
    if (result.kind == CheckedRequirementKind::TerminatedWithin)
      valid &= result.family.empty() &&
               (!result.begin.isConstant() || result.begin.constant >= 0) &&
               (!result.begin.isConstant() || !result.end.isConstant() ||
                result.begin.constant < result.end.constant);
    if (result.kind == CheckedRequirementKind::UnionMember)
      valid &= UnionMember::decode(result.family).has_value() &&
               result.begin == PathAffine::ofConstant(0) &&
               result.end == PathAffine::ofConstant(0);
    if (result.kind == CheckedRequirementKind::ObjectType)
      valid &= ObjectType::parse(result.family).has_value();
    if (result.kind == CheckedRequirementKind::Container ||
        result.kind == CheckedRequirementKind::ContainerFresh ||
        result.kind == CheckedRequirementKind::ContainerTail)
      valid &= ContainerShape::decode(result.family).has_value() &&
               result.begin == PathAffine::ofConstant(0) &&
               result.end == PathAffine::ofConstant(0);
    if (result.kind == CheckedRequirementKind::ContainerDerived) {
      const auto source = [](const PathAffine &value) {
        return value == PathAffine::ofConstant(0) ||
               (value.path && !value.path->isResult() && value.scale == 1 &&
                value.constant == 0 && !value.expression &&
                value.quantity == PathAffine{}.quantity);
      };
      valid &= ContainerShape::decode(result.family).has_value() &&
               !result.other.isResult() && source(result.begin) &&
               source(result.end) &&
               (!result.end.path || result.begin.path.has_value()) &&
               (!result.begin.path || *result.begin.path != result.other) &&
               (!result.end.path || (*result.end.path != result.other &&
                                     result.end.path != result.begin.path));
    }
    if (result.kind == CheckedRequirementKind::ContainerSeparated)
      valid &= result.family.empty() && result.path != result.other &&
               result.begin == PathAffine::ofConstant(0) &&
               result.end == PathAffine::ofConstant(0);
    const auto guard = parseSummaryGuard(text(), resolve);
    valid &= guard.has_value();
    if (guard)
      result.when = *guard;
    const auto outcome = text();
    if (!outcome.empty()) {
      result.on = parseOutcome(outcome);
      valid &= result.on.has_value();
    }
    result.ifNonNull = flag();
    if (result.kind == CheckedRequirementKind::UnionMember ||
        result.kind == CheckedRequirementKind::ObjectType ||
        result.kind == CheckedRequirementKind::Container ||
        result.kind == CheckedRequirementKind::ContainerSeparated ||
        result.kind == CheckedRequirementKind::ContainerFresh ||
        result.kind == CheckedRequirementKind::ContainerTail)
      valid &= result.begin == result.end && !result.ifNonNull;
    if (result.kind == CheckedRequirementKind::ContainerDerived)
      valid &= !result.ifNonNull;
    return result;
  }
  [[nodiscard]] bool good() const { return valid; }
  [[nodiscard]] bool finished() const {
    return valid && position == bytes.size();
  }

private:
  std::string bytes;
  std::size_t position = 0;
  bool valid = true;
};

std::string printCheckedContract(const CheckedContract &contract,
                                 const GlobalNamer &names) {
  CheckedWriter out;
  out.text("7");
  out.text(contract.signature);
  out.number(contract.computed);
  out.number(contract.selected);
  out.number(contract.deferred);
  out.number(contract.limited);
  out.number(contract.obligations.limited());
  out.number(contract.caseInputs.size());
  for (const auto &input : contract.caseInputs)
    out.text(printSummaryPath(input, names));
  out.number(contract.requirements.size());
  for (const auto &requirement : contract.requirements)
    out.requirement(requirement, names);
  out.number(contract.establishes.size());
  for (const auto &requirement : contract.establishes)
    out.requirement(requirement, names);
  out.number(contract.obligations.entries().size());
  for (const auto &[key, obligation] : contract.obligations.entries()) {
    (void)key;
    out.text(toString(obligation.property));
    out.text(toString(obligation.outcome));
    out.location(obligation.location);
    out.text(obligation.function);
    out.text(obligation.subject);
    out.text(obligation.reason);
    out.number(obligation.calls.size());
    for (const auto &call : obligation.calls)
      out.location(call);
  }
  if (!out.good()) {
    CheckedContract limited;
    limited.computed = true;
    limited.selected = contract.selected;
    limited.limited = true;
    return printCheckedContract(limited, names);
  }
  return out.finish();
}

std::optional<CheckedContract>
parseCheckedContract(std::string_view record, const GlobalResolver &resolve) {
  CheckedReader in(record);
  if (in.text() != "7")
    return std::nullopt;
  CheckedContract result;
  result.signature = in.text();
  result.computed = in.flag();
  result.selected = in.flag();
  result.deferred = in.flag();
  result.limited = in.flag();
  if (in.flag())
    result.obligations.markLimited();
  const auto inputs = in.number<std::size_t>();
  if (!in.good() || inputs > 64)
    return std::nullopt;
  for (std::size_t i = 0; i < inputs; ++i) {
    const auto path = parseSummaryPath(in.text(), resolve);
    if (!path || path->isResult() || path->steps.size() > MaxHeapPathDepth ||
        !result.caseInputs.insert(*path).second)
      return std::nullopt;
  }
  const auto readRequirements = [&](auto &requirements) {
    const auto count = in.number<std::size_t>();
    if (!in.good() || count > MaxSafetyRequirements)
      return false;
    for (std::size_t i = 0; i < count && in.good(); ++i)
      if (!requirements.insert(in.requirement(resolve)).second)
        return false;
    return in.good();
  };
  if (!readRequirements(result.requirements) ||
      !readRequirements(result.establishes))
    return std::nullopt;
  for (const auto &requirement : result.requirements)
    if (requirement.path.isResult() || requirement.other.isResult() ||
        requirement.on || requirement.ifNonNull ||
        hasResult(requirement.begin) || hasResult(requirement.end) ||
        requirement.kind == CheckedRequirementKind::Copied ||
        requirement.kind == CheckedRequirementKind::Zeroed ||
        requirement.kind == CheckedRequirementKind::Position ||
        requirement.kind == CheckedRequirementKind::Progress ||
        requirement.kind == CheckedRequirementKind::ContainerDerived ||
        requirement.kind == CheckedRequirementKind::ContainerFresh ||
        requirement.kind == CheckedRequirementKind::ContainerTail)
      return std::nullopt;
  for (const auto &requirement : result.requirements)
    if (requirement.kind == CheckedRequirementKind::ArgumentListConsumed ||
        requirement.kind == CheckedRequirementKind::TerminatedWithin)
      return std::nullopt;
  for (const auto &requirement : result.requirements)
    if (requirement.kind == CheckedRequirementKind::Terminated &&
        (!requirement.end.isConstant() || requirement.end.constant != 0 ||
         !requirement.family.empty() ||
         (requirement.begin.isConstant() && requirement.begin.constant < 0)))
      return std::nullopt;
  for (const auto &post : result.establishes) {
    if (post.kind == CheckedRequirementKind::StandardStream ||
        post.kind == CheckedRequirementKind::ArgumentList ||
        post.kind == CheckedRequirementKind::FormatArguments ||
        (post.kind == CheckedRequirementKind::ArgumentListConsumed &&
         (!post.when.trivial() || post.on || post.ifNonNull)))
      return std::nullopt;
    if (post.kind == CheckedRequirementKind::ContainerDerived ||
        post.kind == CheckedRequirementKind::ContainerTail) {
      const auto shape = ContainerShape::decode(post.family);
      const auto hasPremise = [&](const SummaryPath &path) {
        return std::ranges::any_of(result.requirements, [&](const auto &entry) {
          if (entry.kind != CheckedRequirementKind::Container ||
              entry.path != path || !entry.when.trivial())
            return false;
          const auto input = ContainerShape::decode(entry.family);
          return input && input->object == shape->object &&
                 input->link == shape->link;
        });
      };
      if (!hasPremise(post.other) ||
          (post.begin.path && !hasPremise(*post.begin.path)) ||
          (post.end.path && !hasPremise(*post.end.path)))
        return std::nullopt;
    }
    if ((post.kind == CheckedRequirementKind::ContainerDerived ||
         post.kind == CheckedRequirementKind::ContainerTail) &&
        post.other.isResult())
      return std::nullopt;
    if (post.kind == CheckedRequirementKind::ContainerFresh &&
        ContainerShape::decode(post.family)->access != ContainerAccess::Release)
      return std::nullopt;
    if (post.ifNonNull && post.kind != CheckedRequirementKind::Initialized &&
        post.kind != CheckedRequirementKind::Zeroed &&
        post.kind != CheckedRequirementKind::TerminatedWithin &&
        post.kind != CheckedRequirementKind::Copied)
      return std::nullopt;
    if (post.kind == CheckedRequirementKind::Progress &&
        (post.path.isResult() || post.other.isResult() ||
         post.path == post.other ||
         (post.path.isParam() && post.path.isRoot()) ||
         (post.other.isParam() && post.other.isRoot()) ||
         !post.begin.isConstant() || post.begin.constant != 0 ||
         !post.end.isConstant() || !post.family.empty()))
      return std::nullopt;
    if (post.kind != CheckedRequirementKind::Position)
      continue;
    if ((post.path.isParam() && post.path.isRoot()) || !post.family.empty() ||
        hasResult(post.begin) || hasResult(post.end) ||
        (post.begin.isConstant() && post.end.isConstant() &&
         post.begin.constant > post.end.constant))
      return std::nullopt;
  }
  for (const auto &post : result.establishes)
    if (((post.kind == CheckedRequirementKind::Copied ||
          post.kind == CheckedRequirementKind::Position) &&
         post.other.isResult()) ||
        post.kind == CheckedRequirementKind::SumFits)
      return std::nullopt;
  const auto count = in.number<std::size_t>();
  if (!in.good() || count > MaxSafetyObligations)
    return std::nullopt;
  for (std::size_t i = 0; i < count && in.good(); ++i) {
    SafetyObligation obligation;
    const auto property = parseSafetyProperty(in.text());
    const auto outcome = parseSafetyOutcome(in.text());
    if (!property || !outcome)
      return std::nullopt;
    obligation.property = *property;
    obligation.outcome = *outcome;
    obligation.location = in.location();
    obligation.function = in.text();
    obligation.subject = in.text();
    obligation.reason = in.text();
    const auto calls = in.number<std::size_t>();
    if (!in.good() || calls > MaxSafetyCallDepth)
      return std::nullopt;
    for (std::size_t c = 0; c < calls; ++c)
      obligation.calls.pushBack(in.location());
    if (result.obligations.entries().contains(obligation.identity()))
      return std::nullopt;
    result.obligations.add(std::move(obligation));
  }
  if (!in.finished() || !result.computed)
    return std::nullopt;
  result.obligations.shareSnapshot();
  return result;
}

} // namespace weavec::core
