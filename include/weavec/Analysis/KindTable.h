//===- KindTable.h - Pointer kinds by declaration (RFC 0030) ----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §7, §14: the kind of every parameter, result and slot (field or
// variable) the unit's analysis consults, with where it came from. Before
// the engine runs, `AttributeReader` fills in the declared kinds (§7.2);
// `KindInference` (stage S6) adds the defaults, the slot kinds, the
// must-access requirements and the `reliesOnSingle` flags (§7.3–§7.5).
//
// An entry keeps the §7.2 precedence level of its shape and of its
// nullability separately, because the two are resolved separately:
// `WEAVEC_SIZED_BY(n)` together with a `nonnull` attribute is
// `counted(param n) nonnull`. A kind that rests only on attributes in system
// headers (level 4) is never definite and never checked; facets resting on
// it are `trusted(system-api)`.
//
// Entries are keyed by canonical declaration; a function's entries merge
// every redeclaration.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_KINDTABLE_H
#define WEAVEC_ANALYSIS_KINDTABLE_H

#include "weavec/Core/LibrarySpec.h"
#include "weavec/Core/PointerKind.h"

#include "clang/AST/Decl.h"
#include "clang/Basic/SourceLocation.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace weavec::analysis {

/// §7.2 precedence, strongest first.
enum class KindLevel : std::uint8_t {
  /// 1. `WEAVEC_*` annotations, wherever they are written.
  Annotation,
  /// 2. Ecosystem attributes (`counted_by`, `alloc_size`, `nonnull`,
  /// `_Nonnull`, `[static N]`, VLA parameters) outside system headers.
  Ecosystem,
  /// 3. The `LibrarySpec` entry. The calls an entry governs are LibCall and
  /// Release sites, which read the entry itself, so the table records only
  /// that the level exists (`KindTable::governedByLibrary`): it outranks
  /// level 4.
  LibrarySpec,
  /// 4. Attributes in system headers.
  SystemHeader,
};
inline constexpr std::size_t KindLevelCount = 4;

[[nodiscard]] std::string_view toString(KindLevel level) noexcept;

/// The kind of one parameter, result or slot.
struct KindEntry {
  /// The shape, extent term, nullability and source (`Declared` for
  /// everything `AttributeReader` reads). `unknown` when only the
  /// nullability was declared.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::PointerKind kind = {};
  /// §7.1: exact, declared or a lower bound; none for shapes without an
  /// extent term (`unknown`, `nul-terminated`, `single`'s implicit one).
  std::optional<core::ExtentClass> extentClass = std::nullopt;
  /// The level the shape came from; none when no level gave one.
  std::optional<KindLevel> shapeLevel = std::nullopt;
  /// The level the nullability came from; none when no level gave one.
  std::optional<KindLevel> nullabilityLevel = std::nullopt;
  /// `alloc_size(i, j)`: a `sized` result is `param i * param j` bytes; the
  /// extent term holds `param i` and this holds `j`.
  std::optional<std::uint32_t> extentFactor = std::nullopt;
  /// §7.3: some access in the body, or some store of the parameter into a
  /// slot, is proven only by the Single default. Set by `KindInference`
  /// (S6); false until then.
  bool reliesOnSingle = false;

  /// A shape other than `unknown`: the entry is a *required position* for
  /// PtrArith and Cast sites (§7.4) and gives a Call site a spatial facet.
  [[nodiscard]] bool hasShape() const noexcept;
  /// The declaration says the value is never null (or requires that).
  [[nodiscard]] bool isNonnull() const noexcept;
  /// The shape rests only on system-header attributes (level 4).
  [[nodiscard]] bool shapeFromSystemHeader() const noexcept;
  /// The nullability rests only on system-header attributes (level 4).
  [[nodiscard]] bool nullabilityFromSystemHeader() const noexcept;
  /// A check may compare against the extent: it is exact or declared
  /// (§7.1), and not only a system-header attribute (§7.2).
  [[nodiscard]] bool isCheckOperand() const noexcept;

  friend bool operator==(const KindEntry &, const KindEntry &) = default;
};

/// A malformed or conflicting declaration `AttributeReader` found. Stage S6
/// reports these as `invalid-annotation` warnings (RFC 0030,
/// *Diagnostics*); until then they are data only, so that the unit's
/// diagnostics do not change.
struct KindProblem {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  clang::SourceLocation location = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string message = {};
};

/// Kinds by canonical declaration.
class KindTable {
public:
  [[nodiscard]] const KindEntry *param(const clang::FunctionDecl &function,
                                       unsigned index) const;
  [[nodiscard]] const KindEntry *
  result(const clang::FunctionDecl &function) const;
  [[nodiscard]] const KindEntry *field(const clang::FieldDecl &field) const;
  [[nodiscard]] const KindEntry *variable(const clang::VarDecl &variable) const;
  /// A `LibrarySpec` entry governs the calls to `function` (level 3).
  [[nodiscard]] bool
  governedByLibrary(const clang::FunctionDecl &function) const;

  void setParam(const clang::FunctionDecl &function, unsigned index,
                KindEntry entry);
  void setResult(const clang::FunctionDecl &function, KindEntry entry);
  void setField(const clang::FieldDecl &field, KindEntry entry);
  void setVariable(const clang::VarDecl &variable, KindEntry entry);
  void setGovernedByLibrary(const clang::FunctionDecl &function);
  void addProblem(KindProblem problem);

  [[nodiscard]] const std::vector<KindProblem> &problems() const noexcept {
    return problemList;
  }
  /// The number of entries of every kind.
  [[nodiscard]] std::size_t size() const noexcept;

private:
  llvm::DenseMap<std::pair<const clang::FunctionDecl *, unsigned>, KindEntry>
      params;
  llvm::DenseMap<const clang::FunctionDecl *, KindEntry> results;
  llvm::DenseMap<const clang::FieldDecl *, KindEntry> fields;
  llvm::DenseMap<const clang::VarDecl *, KindEntry> variables;
  llvm::DenseSet<const clang::FunctionDecl *> library;
  std::vector<KindProblem> problemList;
};

/// §8: the `LibrarySpec` row that governs calls to `function`: its name, a
/// `chk` alias or either behind `__builtin_` names a row that accepts the
/// declaration's signature, and the unit does not define the function.
[[nodiscard]] std::optional<core::LibraryMatch>
governingLibraryEntry(const clang::FunctionDecl &function,
                      const core::LibrarySpec &library);

/// The parameter classes of a declaration, as `LibrarySpec` rows state them.
/// None for a declaration without a prototype.
[[nodiscard]] std::optional<core::LibSignature>
librarySignatureOf(const clang::FunctionDecl &function);

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_KINDTABLE_H
