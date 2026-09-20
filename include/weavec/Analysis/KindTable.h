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
// `KindInference` adds the defaults, the slot kinds, the must-access
// requirements and the `reliesOnSingle` flags (§7.3–§7.5).
//
// What an entry means depends on its position:
//
//   - a parameter: `kind` is what the body may assume at entry (declared,
//     the §7.3 join for a static function, or the A1 default) and, when
//     declared, what every call must satisfy; `mustAccess` lists the §7.5
//     requirements, enforced as `enforcement` says;
//   - a result: what a call's value guarantees (declared, inferred over the
//     unit's `return`s, or the A3 default for a function outside the unit);
//   - a field or a variable with static storage (a *slot*): what every load
//     guarantees (declared, or the §7.3 greatest fixpoint), with the stores
//     that demoted it.
//
// Only a declared shape makes a *required position* for `SiteCollector`
// (§7.4 rule 4): `hasDeclaredShape`, never `hasShape`, decides it, so the
// inferred entries leave the unit's sites unchanged.
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

#include "weavec/Core/Diagnostic.h"
#include "weavec/Core/LibrarySpec.h"
#include "weavec/Core/PointerKind.h"

#include "clang/AST/Decl.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

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

/// §7.5: the rule an inferred requirement came from.
enum class MustAccessRule : std::uint8_t {
  /// A must-access `*p`, `p->f` or `p[0]`: single nonnull.
  R1,
  /// `p[i + k]` in a canonical counted loop: counted(e + k) nonnull under
  /// `c < e`.
  R2,
  /// `*x` in a canonical loop `x = p; x < q`: ended-by(q) nonnull under
  /// `p < q`.
  R3,
  /// A canonical scan, or a must-call passing `p` where a `LibrarySpec` row
  /// requires a string: nul-terminated.
  R4,
  /// An unguarded must-access `p[e]`: counted(e + 1) nonnull.
  R5,
};

[[nodiscard]] std::string_view toString(MustAccessRule rule) noexcept;

/// §7.5: the guard of a requirement, `lhs < rhs` or `lhs <= rhs`, over
/// constants and the function's parameters (a pointer comparison for R3).
/// The requirement holds only when the guard does: a loop that runs zero
/// times accesses nothing.
struct RequirementGuard {
  enum class Relation : std::uint8_t { Less, LessEqual };

  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::ExtentTerm lhs = {};
  Relation relation = Relation::Less;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::ExtentTerm rhs = {};

  /// `<lhs> < <rhs>` or `<lhs> <= <rhs>`, each term spelled by
  /// `ExtentTerm::toString`.
  [[nodiscard]] std::string toString() const;
  /// Reads back what `toString` wrote (§13.1 `functions[].requirements[]`).
  [[nodiscard]] static std::optional<RequirementGuard>
  parse(std::string_view text);

  friend bool operator==(const RequirementGuard &,
                         const RequirementGuard &) = default;
};

/// §7.5: one requirement inferred for a pointer parameter from the body:
/// `kind` (its shape, extent term and nullability; source `Inferred`) when
/// `guard` holds, or always without one. Requirements on one parameter
/// combine by conjunction.
struct MustAccessRequirement {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::PointerKind kind = {};
  std::optional<RequirementGuard> guard = std::nullopt;
  MustAccessRule rule = MustAccessRule::R1;
  /// The access (or, for R4, the call or loop condition) that gave it.
  const clang::Stmt *access = nullptr;
  /// Every must-access that gives this requirement, `access` first: the
  /// accesses it covers in the body (§7.5 enforcement).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<const clang::Stmt *> covers = {};

  /// `[<guard> -> ]<kind> (<rule>)`.
  [[nodiscard]] std::string toString() const;

  friend bool operator==(const MustAccessRequirement &,
                         const MustAccessRequirement &) = default;
};

/// §7.5: how a function's inferred requirements are enforced.
enum class RequirementEnforcement : std::uint8_t {
  /// A `static` function whose address is not taken: every direct call
  /// gets a spatial or null facet for the argument, checked under the
  /// guard; inside the body the requirement holds.
  CallSites,
  /// An exported or address-taken function: inside the body, accesses
  /// covered only by an extent requirement beyond `single` are
  /// `trusted(caller-contract)`, their null facets stay checked, and the
  /// requirement is exported and verified at link.
  CallerContract,
};

[[nodiscard]] std::string_view
toString(RequirementEnforcement enforcement) noexcept;

/// §7.3, §13.1 `slotKinds.demotedBy`: a store, or a store the syntax does
/// not show, that made a slot `unknown`.
struct KindDemotion {
  /// The assignment, initialiser, call or conversion.
  const clang::Stmt *store = nullptr;
  /// Why the stored value is not Single-valid, or which hidden store it is
  /// ("pointer arithmetic", "a byte-wise write by 'memcpy'", ...).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string reason = {};

  friend bool operator==(const KindDemotion &, const KindDemotion &) = default;
};

/// The kind of one parameter, result or slot.
struct KindEntry {
  /// The shape, extent term, nullability, and the shape's source:
  /// `Declared` for what `AttributeReader` reads, `Inferred` for the §7.3
  /// static joins, results and slot kinds, `Default` for the A1 parameter
  /// and A3 result defaults. `unknown` when only the nullability was
  /// declared and nothing was inferred.
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
  /// for parameters whose shape is the A1 default.
  bool reliesOnSingle = false;
  /// §7.5 (parameters): the inferred requirements, conjoined.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<MustAccessRequirement> mustAccess = {};
  /// §7.5: how `mustAccess` is enforced; none when it is empty.
  std::optional<RequirementEnforcement> enforcement = std::nullopt;
  /// §7.3: `argv` of `main`, never assigned: `counted(param 0 plus 1)
  /// nonnull` at level 4, with each `argv[i]`, `i < argc`, nul-terminated
  /// and nonnull and `argv[argc]` null. Facets resting on it are
  /// `trusted(system-api)`.
  bool mainArgv = false;
  /// §7.3 (slots): the stores that demoted the slot to `unknown`, in source
  /// order; empty for a slot that stayed `single` or has a declared kind.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<KindDemotion> demotedBy = {};

  /// A shape other than `unknown`, whatever its source.
  [[nodiscard]] bool hasShape() const noexcept;
  /// A shape some declaration states (§7.2), not inferred or defaulted: the
  /// entry is a *required position* for PtrArith and Cast sites (§7.4) and
  /// gives a Call site a spatial facet.
  [[nodiscard]] bool hasDeclaredShape() const noexcept;
  /// The value is never null, whatever the source of that fact.
  [[nodiscard]] bool isNonnull() const noexcept;
  /// Some declaration says the value is never null (or requires that).
  [[nodiscard]] bool declaresNonnull() const noexcept;
  /// Some §7.5 requirement is enforced at every direct call.
  [[nodiscard]] bool hasEnforcedRequirement() const noexcept;
  /// The shape rests only on system-header attributes (level 4).
  [[nodiscard]] bool shapeFromSystemHeader() const noexcept;
  /// The nullability rests only on system-header attributes (level 4).
  [[nodiscard]] bool nullabilityFromSystemHeader() const noexcept;
  /// A check may compare against the extent: it is exact or declared
  /// (§7.1), and not only a system-header attribute (§7.2).
  [[nodiscard]] bool isCheckOperand() const noexcept;

  friend bool operator==(const KindEntry &, const KindEntry &) = default;
};

/// A malformed or conflicting declaration `AttributeReader` found: an
/// `invalid-annotation` warning (RFC 0030, *Diagnostics*), which
/// `kindProblemDiagnostics` spells for the unit's sink.
struct KindProblem {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  clang::SourceLocation location = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string message = {};
};

/// §7.2: `T p[N]` without `static` is no requirement, because C gives it
/// none; it only yields this suggestion, whose fix-it inserts `insert` at
/// `location` (`static ` after the `[`).
struct KindSuggestion {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  clang::SourceLocation location = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string message = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string insert = {};
};

/// §7.2 `malloc`, `ownership_returns(m)`, `ownership_takes(m, i)` and
/// `ownership_holds(m, i)`: the ownership contract a function's declarations
/// state. A contract, not a kind: the unknown-callee rules (§5.1) read it.
struct OwnershipContract {
  /// The result is fresh, of this family: the heap's (`free`) for
  /// `malloc`, `m` for `ownership_returns(m)`.
  std::optional<std::string> freshResult = std::nullopt;
  struct Argument {
    /// 0-based.
    unsigned index = 0;
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    std::string family = {};
    /// `ownership_holds`: retained; `ownership_takes`: released.
    bool retains = false;

    friend bool operator==(const Argument &, const Argument &) = default;
  };
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<Argument> arguments = {};
  /// Level 2, or 4 when every attribute sits in a system header.
  KindLevel level = KindLevel::Ecosystem;

  [[nodiscard]] bool empty() const noexcept {
    return !freshResult && arguments.empty();
  }
  friend bool operator==(const OwnershipContract &,
                         const OwnershipContract &) = default;
};

/// §7.5: a requirement of a parameter whose covered accesses include a
/// statement (`KindTable::covering`).
struct CoveringRequirement {
  /// The canonical declaration of the function that has the parameter.
  const clang::FunctionDecl *function = nullptr;
  unsigned param = 0;
  /// The index in the parameter's `KindEntry::mustAccess`.
  unsigned requirement = 0;
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

  /// §6.3: `WEAVEC_REQUIRE_SAFE` on some declaration of `function`.
  [[nodiscard]] bool requireSafe(const clang::FunctionDecl &function) const;
  void setRequireSafe(const clang::FunctionDecl &function);

  /// §7.2: the ownership contract `function`'s attributes state, or null.
  [[nodiscard]] const OwnershipContract *
  ownership(const clang::FunctionDecl &function) const;
  void setOwnership(const clang::FunctionDecl &function,
                    OwnershipContract contract);

  /// §7.2: a fix-it that would declare a kind the code implies but C does
  /// not require (`T p[N]` without `static`).
  void addSuggestion(KindSuggestion suggestion);
  [[nodiscard]] const std::vector<KindSuggestion> &
  suggestions() const noexcept {
    return suggestionList;
  }

  [[nodiscard]] const std::vector<KindProblem> &problems() const noexcept {
    return problemList;
  }
  /// The number of entries of every kind.
  [[nodiscard]] std::size_t size() const noexcept;
  /// §7.5: the requirements that cover `access` (one of their `covers`).
  [[nodiscard]] llvm::ArrayRef<CoveringRequirement>
  covering(const clang::Stmt &access) const;

  using ParamKey = std::pair<const clang::FunctionDecl *, unsigned>;
  /// Every entry, keyed by canonical declaration (unordered).
  [[nodiscard]] const llvm::DenseMap<ParamKey, KindEntry> &
  paramEntries() const noexcept {
    return params;
  }
  [[nodiscard]] const llvm::DenseMap<const clang::FunctionDecl *, KindEntry> &
  resultEntries() const noexcept {
    return results;
  }
  [[nodiscard]] const llvm::DenseMap<const clang::FieldDecl *, KindEntry> &
  fieldEntries() const noexcept {
    return fields;
  }
  [[nodiscard]] const llvm::DenseMap<const clang::VarDecl *, KindEntry> &
  variableEntries() const noexcept {
    return variables;
  }
  [[nodiscard]] const llvm::DenseMap<const clang::FunctionDecl *,
                                     OwnershipContract> &
  ownershipEntries() const noexcept {
    return contracts;
  }

private:
  llvm::DenseMap<ParamKey, KindEntry> params;
  llvm::DenseMap<const clang::FunctionDecl *, KindEntry> results;
  llvm::DenseMap<const clang::FieldDecl *, KindEntry> fields;
  llvm::DenseMap<const clang::VarDecl *, KindEntry> variables;
  llvm::DenseSet<const clang::FunctionDecl *> library;
  llvm::DenseSet<const clang::FunctionDecl *> safe;
  llvm::DenseMap<const clang::FunctionDecl *, OwnershipContract> contracts;
  std::vector<KindProblem> problemList;
  std::vector<KindSuggestion> suggestionList;
  /// `covering`, built on first use after the last `setParam`.
  mutable llvm::DenseMap<const clang::Stmt *,
                         llvm::SmallVector<CoveringRequirement, 1>>
      coveringIndex;
  mutable bool coveringBuilt = false;
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

/// The table's problems as `invalid-annotation` warnings, in source order,
/// for the unit's diagnostic sink.
[[nodiscard]] std::vector<core::Diagnostic>
kindProblemDiagnostics(const KindTable &table, const clang::SourceManager &sm);

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_KINDTABLE_H
