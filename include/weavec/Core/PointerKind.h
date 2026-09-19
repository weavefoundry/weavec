//===- PointerKind.h - The pointer-kind lattice (RFC 0030) -----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §7.1: what a pointer value guarantees, or what a parameter
// requires, about the memory accessible from it. For a value `p` of type
// `T *` that is not null:
//
//   single            at least one `T` is accessible from `p`
//   counted(e)        at least `e` elements of `T` are accessible
//   sized(e)          at least `e` bytes are accessible
//   ended-by(q)       `p <= q`, and `[p, q)` lies in one object
//   nul-terminated    a zero element lies at or after `p` within its object
//   unknown           nothing
//
// followed by `nonnull` or `nullable`. Every kind is a lower bound: none
// claims that the object ends there. Whether a kind may be compared against
// by a check is its `ExtentClass`: exact extents and declared kinds may,
// lower bounds (Single, the defaults, inferred requirements) may not.
//
// Extent terms are `k * <path> + c` or the constant `c`, where the path is a
// sibling parameter (`param <i>`) or a sibling field (`.<field>`) of the
// same object. They are spelled as in the RFC 0011 extent grammar:
// `<c>` or `<path> scale <k> plus <c>`.
//
// The path is an `ExtentPath` rather than a `SummaryPath`: summaries carry
// kinds (summary format 27), so `Summary.h` depends on this header, and a
// summary path has no root for "a sibling field of the same object".
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_POINTERKIND_H
#define WEAVEC_CORE_POINTERKIND_H

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::core {

enum class PointerShape : std::uint8_t {
  Single,
  Counted,
  Sized,
  EndedBy,
  NulTerminated,
  Unknown,
};

enum class Nullability : std::uint8_t { Nonnull, Nullable };

/// Where a kind came from (§7.2, §7.3, §7.5).
enum class KindSource : std::uint8_t { Declared, Inferred, Default };

[[nodiscard]] std::string_view toString(PointerShape shape) noexcept;
[[nodiscard]] std::string_view toString(Nullability nullability) noexcept;
[[nodiscard]] std::string_view toString(KindSource source) noexcept;
[[nodiscard]] std::optional<Nullability>
parseNullability(std::string_view text);
[[nodiscard]] std::optional<KindSource> parseKindSource(std::string_view text);

/// Whether a shape carries an extent term (Counted, Sized, EndedBy).
[[nodiscard]] constexpr bool hasExtent(PointerShape shape) noexcept {
  return shape == PointerShape::Counted || shape == PointerShape::Sized ||
         shape == PointerShape::EndedBy;
}

/// The place an extent term reads: a sibling parameter or a sibling field
/// of the same object.
struct ExtentPath {
  enum class Root : std::uint8_t { Param, Field };

  Root root = Root::Param;
  /// `Root::Param`: the parameter's 0-based index.
  std::uint32_t param = 0;
  /// `Root::Field`: the field's name.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string field = {};

  [[nodiscard]] static ExtentPath ofParam(std::uint32_t index);
  [[nodiscard]] static ExtentPath ofField(std::string name);

  /// `param <index>` or `.<field>`.
  [[nodiscard]] std::string toString() const;
  [[nodiscard]] static std::optional<ExtentPath> parse(std::string_view text);

  friend bool operator==(const ExtentPath &, const ExtentPath &) = default;
  friend std::strong_ordering operator<=>(const ExtentPath &,
                                          const ExtentPath &) = default;
};

/// `scale * path + offset`, or the constant `offset` when there is no path.
/// Counted: elements; Sized: bytes; EndedBy: the end pointer's path, with
/// `offset` elements past it (`EndedBy(q + 1)`, §7.5 R3).
struct ExtentTerm {
  std::optional<ExtentPath> path = std::nullopt;
  std::int64_t scale = 1;
  std::int64_t offset = 0;

  [[nodiscard]] static ExtentTerm constant(std::int64_t value);
  [[nodiscard]] static ExtentTerm of(ExtentPath path, std::int64_t scale = 1,
                                     std::int64_t offset = 0);

  [[nodiscard]] bool isConstant() const noexcept { return !path.has_value(); }

  /// RFC 0011: `<c>`, or `<path> scale <k> plus <c>`.
  [[nodiscard]] std::string toString() const;
  /// Accepts the RFC 0011 forms and, leniently, a bare `<path>` (scale 1,
  /// plus 0).
  [[nodiscard]] static std::optional<ExtentTerm> parse(std::string_view text);

  /// Constant terms compare by value alone; their scale is meaningless.
  friend bool operator==(const ExtentTerm &a, const ExtentTerm &b) noexcept;
};

struct PointerKind {
  PointerShape shape = PointerShape::Unknown;
  /// Meaningful only when `hasExtent(shape)`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  ExtentTerm extent = {};
  Nullability nullability = Nullability::Nullable;
  KindSource source = KindSource::Default;

  [[nodiscard]] static PointerKind
  single(Nullability nullability = Nullability::Nullable,
         KindSource source = KindSource::Default);
  [[nodiscard]] static PointerKind
  counted(ExtentTerm count, Nullability nullability = Nullability::Nullable,
          KindSource source = KindSource::Default);
  [[nodiscard]] static PointerKind
  sized(ExtentTerm bytes, Nullability nullability = Nullability::Nullable,
        KindSource source = KindSource::Default);
  [[nodiscard]] static PointerKind
  endedBy(ExtentPath end, std::int64_t offset = 0,
          Nullability nullability = Nullability::Nullable,
          KindSource source = KindSource::Default);
  [[nodiscard]] static PointerKind
  nulTerminated(Nullability nullability = Nullability::Nullable,
                KindSource source = KindSource::Default);
  [[nodiscard]] static PointerKind
  unknown(Nullability nullability = Nullability::Nullable,
          KindSource source = KindSource::Default);

  /// Same shape and, where the shape has one, the same extent term.
  [[nodiscard]] bool sameShape(const PointerKind &other) const noexcept;

  /// §7.1: `single`, `counted(<term>)`, `sized(<term>)`, `ended-by(<path>)`,
  /// `nul-terminated` or `unknown`, then ` nonnull` or ` nullable`. The
  /// source is not part of the spelling. An `ended-by` with a non-trivial
  /// scale or offset spells the whole term.
  [[nodiscard]] std::string toString() const;
  /// Parses the spelling of `toString`; the result has source `source`.
  [[nodiscard]] static std::optional<PointerKind>
  parse(std::string_view text, KindSource source = KindSource::Default);

  /// The extent term is compared only for shapes that have one.
  friend bool operator==(const PointerKind &a, const PointerKind &b) noexcept;
};

/// What the join may assume at a merge point.
struct JoinFacts {
  /// Whether `e >= 1` is proven at the join, for `Single ⊔ Counted(e)`.
  /// When empty, only constant terms are judged.
  std::function<bool(const ExtentTerm &)> provesAtLeastOne = nullptr;
};

/// `Nullable ⊔ Nonnull = Nullable`.
[[nodiscard]] constexpr Nullability join(Nullability a,
                                         Nullability b) noexcept {
  return a == Nullability::Nonnull && b == Nullability::Nonnull
             ? Nullability::Nonnull
             : Nullability::Nullable;
}

/// The weaker of two sources (Default < Inferred < Declared), so a joined
/// guarantee is declared only when both sides were.
[[nodiscard]] KindSource join(KindSource a, KindSource b) noexcept;

/// §7.1, the merge of guarantees at a CFG merge or across stores:
///   - `x ⊔ x = x`;
///   - different shapes or extent terms join to `Unknown`, except
///     `Single ⊔ Counted(e) = Single` when `e >= 1` is proven;
///   - `Nullable ⊔ Nonnull = Nullable`.
[[nodiscard]] PointerKind join(const PointerKind &a, const PointerKind &b,
                               const JoinFacts &facts = {});

/// §7.1: requirements on one pointer combine by conjunction, and every one
/// of them is checked. The canonical form keeps one nullability (nonnull if
/// any requirement says so) and the distinct shape requirements, dropping
/// `unknown`, keeping the larger of two constant terms of the same shape,
/// and keeping the stronger source of two equal requirements (a declared
/// requirement is enforced at every call site, §7.5).
class KindRequirements {
public:
  KindRequirements() = default;
  explicit KindRequirements(const PointerKind &requirement) {
    add(requirement);
  }

  void add(const PointerKind &requirement);
  void add(const KindRequirements &other);

  [[nodiscard]] Nullability nullability() const noexcept { return required; }
  /// The shape requirements; each carries the conjunction's nullability.
  [[nodiscard]] std::span<const PointerKind> shapes() const noexcept {
    return entries;
  }
  /// Nothing is required.
  [[nodiscard]] bool empty() const noexcept {
    return entries.empty() && required == Nullability::Nullable;
  }

  /// The requirements spelled by `PointerKind::toString`, joined by ` & `;
  /// `unknown <nullability>` when there is no shape requirement.
  [[nodiscard]] std::string toString() const;
  [[nodiscard]] static std::optional<KindRequirements>
  parse(std::string_view text, KindSource source = KindSource::Default);

  friend bool operator==(const KindRequirements &,
                         const KindRequirements &) = default;

private:
  Nullability required = Nullability::Nullable;
  std::vector<PointerKind> entries;
};

/// The conjunction of two requirement sets.
[[nodiscard]] KindRequirements conjoin(const KindRequirements &a,
                                       const KindRequirements &b);

/// §7.1: what a check may compare against. An *exact* extent (the type of
/// an array or VLA object, an allocation size in scope, a surviving §7.6
/// invariant) and a *declared* kind may be check operands; a *lower bound*
/// (Single, the A1 and A3 defaults, a §7.5 requirement inferred inside the
/// body) discharges only the accesses it covers. Only an exact extent can
/// make an access a violation.
enum class ExtentClass : std::uint8_t { Exact, Declared, LowerBound };

[[nodiscard]] std::string_view toString(ExtentClass extentClass) noexcept;

/// The class of a kind by its shape and source: none for shapes without an
/// extent (`unknown`, `nul-terminated`); `LowerBound` for `single` and for
/// inferred and default kinds; `Declared` for declared kinds with an extent.
/// `Exact` is never derived from a kind alone: `KindTable` assigns it to
/// surviving §7.6 invariants, and array and allocation extents are exact by
/// construction.
[[nodiscard]] std::optional<ExtentClass>
extentClassOf(const PointerKind &kind) noexcept;

/// Exact extents and declared kinds may be compared against by a check.
[[nodiscard]] constexpr bool isCheckOperand(ExtentClass extentClass) noexcept {
  return extentClass != ExtentClass::LowerBound;
}

/// Only an exact extent can make an access a violation (§3.3).
[[nodiscard]] constexpr bool
canProveViolation(ExtentClass extentClass) noexcept {
  return extentClass == ExtentClass::Exact;
}

} // namespace weavec::core

#endif // WEAVEC_CORE_POINTERKIND_H
