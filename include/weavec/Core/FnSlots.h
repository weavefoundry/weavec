//===- FnSlots.h - Function-pointer slots (RFC 0030) -----------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §9.3: flow-insensitive, field-based constraints over every
// function-pointer value, solved to a fixpoint over function names. The
// solution says which functions a call through a slot can reach and whether
// the slot is *closed* (every value it can hold is visible to the solver).
//
// Slot keys (§9.3):
//
//   field <record type key> <field>   array elements share the field
//   global <name>
//   static <unit>:<name>              a TU-local static global
//   param <function> <i>
//   result <function>
//   local <function> <name>           TU-private, eliminated before export
//
// The dynamic call constraints ("for an indirect call through S and each
// target f of S, the arguments flow into `param f <i>` and `result f` flows
// to the receiver") are expressed with two more keys, so that every
// constraint is a plain inclusion and the §13.1 export rows
// `{slot, targets, sources, open}` can carry them:
//
//   call-param <i> <S>                what calls through S pass as argument i
//   call-result <S>                   what calls through S return
//
// For each target `f` of `S`, the solver adds `call-param <i> <S> ⊆ param f
// <i>` and `result f ⊆ call-result <S>`.
//
// Constraints are `f ∈ S`, `S ⊆ T` and `open(S)`. Function names are opaque
// strings; the collector qualifies internal-linkage functions (and the
// record type keys of structs defined in a main file) with their unit, as
// `static` globals are, so that names from different units never collide.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_FNSLOTS_H
#define WEAVEC_CORE_FNSLOTS_H

#include "weavec/Core/Ledger.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace weavec::core {

enum class SlotKind : std::uint8_t {
  Field,
  Global,
  Static,
  Param,
  Result,
  Local,
  CallParam,
  CallResult,
};

/// A function name that no program defines: the target of a call through a
/// value from outside. Its result slot is open, and its parameter slots pass
/// their values to unknown code.
inline constexpr std::string_view UnknownFunction = "<unknown>";

struct SlotKey {
  SlotKind kind = SlotKind::Global;
  /// Field: the record type key. Static: the unit. Param, Result, Local: the
  /// function. CallParam, CallResult: the spelling of the callee slot.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string scope = {};
  /// Field: the field. Global, Static, Local: the variable.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string name = {};
  /// Param, CallParam: the 0-based parameter index.
  std::uint32_t index = 0;

  [[nodiscard]] static SlotKey field(std::string record, std::string field);
  [[nodiscard]] static SlotKey global(std::string name);
  [[nodiscard]] static SlotKey staticGlobal(std::string unit, std::string name);
  [[nodiscard]] static SlotKey param(std::string function, std::uint32_t index);
  [[nodiscard]] static SlotKey result(std::string function);
  [[nodiscard]] static SlotKey local(std::string function, std::string name);
  [[nodiscard]] static SlotKey callParam(const SlotKey &callee,
                                         std::uint32_t index);
  [[nodiscard]] static SlotKey callResult(const SlotKey &callee);

  /// The spelling in the file comment.
  [[nodiscard]] std::string toString() const;
  [[nodiscard]] static std::optional<SlotKey> parse(std::string_view text);

  /// The callee slot of a `call-param` or `call-result` key.
  [[nodiscard]] std::optional<SlotKey> callee() const;
  /// `local` keys, and call keys through a local callee: TU-private.
  [[nodiscard]] bool isLocal() const;
  /// `<unit>:<name>` of a `static` key, as `SlotRules::escapedStatics`
  /// spells it.
  [[nodiscard]] std::string staticName() const { return scope + ":" + name; }

  friend bool operator==(const SlotKey &, const SlotKey &) = default;
  friend std::strong_ordering operator<=>(const SlotKey &,
                                          const SlotKey &) = default;
};

/// One constraint: `function ∈ slot`, `from ⊆ slot`, or `open(slot)`.
struct SlotConstraint {
  enum class Kind : std::uint8_t { Member, Subset, Open };

  Kind kind = Kind::Member;
  /// Member and Open: the slot. Subset: the destination.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  SlotKey slot = {};
  /// Subset: the source.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  SlotKey from = {};
  /// Member: the function.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string function = {};
  /// Open: where the outside value comes from, in words.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string detail = {};

  [[nodiscard]] static SlotConstraint member(std::string function,
                                             SlotKey slot);
  [[nodiscard]] static SlotConstraint subset(SlotKey from, SlotKey to);
  [[nodiscard]] static SlotConstraint open(SlotKey slot, std::string detail);

  friend bool operator==(const SlotConstraint &,
                         const SlotConstraint &) = default;
  friend std::strong_ordering operator<=>(const SlotConstraint &,
                                          const SlotConstraint &) = default;
};

/// §13.1 `slots`: one exported slot with the constraints that flow into it.
struct SlotRow {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  SlotKey slot = {};
  /// `f ∈ slot`, sorted.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<std::string> targets = {};
  /// `source ⊆ slot`, sorted.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<SlotKey> sources = {};
  /// `open(slot)` with its detail (the least one, when there are several).
  std::optional<std::string> open = std::nullopt;

  friend bool operator==(const SlotRow &, const SlotRow &) = default;
};

enum class SlotScope : std::uint8_t {
  /// A per-TU compile.
  Unit,
  /// The link step, or `weavec --whole-program`.
  Link,
};

/// The inputs of §9.3's closed-slot rules.
///
/// Openness is seeded by these rules and by `open(S)` constraints, and
/// propagates along inclusions: a slot that can receive a value from an
/// open slot is open. Seeds:
///
///   - `field`: open unless its record is in `confinedRecords`, or at a
///     closed-world link;
///   - `global`: open unless at a closed-world link;
///   - `static`: open when in `escapedStatics`;
///   - `param f i`: open when `f` is exported (unless at a closed-world
///     link), or when `f`'s address reaches an open position;
///   - `result f`: open when `f` has no body in the solved program.
///
/// A function's address reaches an open position when it is a target of a
/// slot whose values code outside the solved program can see: a `field`,
/// `global` or `static` slot seeded open, `param g i` of a function `g`
/// without a body, `result g` of an exported `g` (unless at a closed-world
/// link), or `call-param i S` with `S` open. Local, call and the remaining
/// parameter and result slots are closed unless openness reaches them.
struct SlotRules {
  SlotScope scope = SlotScope::Unit;
  /// Functions with a body in the solved program: the unit, or every unit
  /// with a record at link.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::set<std::string> defined = {};
  /// Externally visible functions.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::set<std::string> exported = {};
  /// Record type keys of structs defined in a main file (not a header)
  /// whose objects never flow through parameters, results, unknown callees
  /// or externally visible globals. Their fields are closed in both scopes.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::set<std::string> confinedRecords = {};
  /// `<unit>:<name>` of `static` globals whose address escapes.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::set<std::string> escapedStatics = {};
  /// Link: the output is an executable (neither `-shared` nor `-r`).
  bool executable = true;
  /// Link: `-rdynamic` or `-Wl,-export-dynamic` was given.
  bool exportDynamic = false;
  /// Link: an `unanalyzed-input` exists.
  bool unanalyzedInputs = false;

  /// At link, with an executable output, no dynamic export and every
  /// non-system input recorded, no code outside the solved program can
  /// store into its slots or call its functions.
  [[nodiscard]] bool closedWorld() const noexcept {
    return scope == SlotScope::Link && executable && !exportDynamic &&
           !unanalyzedInputs;
  }
};

/// Why a slot is open: the slot where the openness entered (`seed`) and why
/// that one is open. At an open indirect call, the call's temporal detail
/// names it (for example "values stored by 'json_set_alloc_funcs' parameter
/// 1").
struct OpenSource {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  SlotKey seed = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string detail = {};

  friend bool operator==(const OpenSource &, const OpenSource &) = default;
};

/// §9.3's four cases at an indirect call, plus a closed slot with no value
/// (a call through it can only be a call through null, which its `nonnull`
/// check traps).
enum class IndirectCallKind : std::uint8_t {
  /// Closed, no target.
  ClosedEmpty,
  /// Closed, one target: analysed exactly as a direct call.
  ClosedSingle,
  /// Closed, several targets: the join of their summaries.
  ClosedJoin,
  /// Open with known targets: as closed for temporal facts only; the
  /// temporal facet is `trusted(extern-contract)`.
  OpenKnown,
  /// Open without known targets: the unknown-callee default with reason
  /// `callback`.
  OpenUnknown,
};

[[nodiscard]] std::string_view toString(IndirectCallKind kind) noexcept;

struct CallResolution {
  IndirectCallKind kind = IndirectCallKind::ClosedEmpty;
  /// Sorted.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<std::string> targets = {};
  std::optional<OpenSource> open = std::nullopt;

  friend bool operator==(const CallResolution &,
                         const CallResolution &) = default;
};

/// The temporal facet of a call through an open slot (§9.3):
/// `trusted(extern-contract)` with known targets, `unresolved(callback)`
/// without; the detail names the open source. None for closed slots, whose
/// calls are analysed through their targets.
[[nodiscard]] std::optional<FacetDecision>
openCallTemporalDecision(const CallResolution &resolution);

/// §9.3, several closed targets: whether a target consumes an argument. A
/// consume is unconditional only if every target consumes unconditionally;
/// a may-effect from any target is a may-effect.
enum class TargetConsume : std::uint8_t { None, May, Unconditional };

[[nodiscard]] constexpr TargetConsume
joinTargetConsume(TargetConsume a, TargetConsume b) noexcept {
  return a == b ? a : TargetConsume::May;
}

/// Joins a per-target value (a summary, an effect) over `targets` with
/// `join`. `lookup(target)` returns `std::optional<T>`; a target without a
/// value, or no target at all, makes the join unknown (none).
template <typename T, typename Lookup, typename Join>
[[nodiscard]] std::optional<T>
joinOverTargets(std::span<const std::string> targets, const Lookup &lookup,
                const Join &join) {
  std::optional<T> result;
  for (const std::string &target : targets) {
    std::optional<T> value = lookup(target);
    if (!value)
      return std::nullopt;
    if (result)
      result = join(*result, *value);
    else
      result = std::move(value);
  }
  return result;
}

/// The solved slots.
class SlotSolution {
public:
  /// The functions `slot` may hold; empty for a slot the solver never saw.
  [[nodiscard]] const std::set<std::string> &targets(const SlotKey &slot) const;
  [[nodiscard]] bool isOpen(const SlotKey &slot) const {
    return openSource(slot).has_value();
  }
  [[nodiscard]] bool isClosed(const SlotKey &slot) const {
    return !isOpen(slot);
  }
  /// Why `slot` is open. Slots the solver never saw are judged by the seed
  /// rules alone.
  [[nodiscard]] std::optional<OpenSource> openSource(const SlotKey &slot) const;
  /// Whether `function`'s address reaches an open position, so that code
  /// outside the solved program may call it.
  [[nodiscard]] bool escapes(std::string_view function) const;
  /// §9.3: how to treat an indirect call through `callee`.
  [[nodiscard]] CallResolution resolveCall(const SlotKey &callee) const;
  /// Every slot the solver saw, sorted.
  [[nodiscard]] std::vector<SlotKey> slots() const;
  /// Worklist steps the solver took (for statistics and tests).
  [[nodiscard]] std::size_t steps() const noexcept { return stepCount; }

private:
  friend class FnSlots;

  struct SlotState {
    std::set<std::string> targets;
    std::optional<OpenSource> open;
  };

  SlotRules rules;
  std::map<SlotKey, SlotState> states;
  std::set<std::string, std::less<>> escaped;
  std::size_t stepCount = 0;
};

/// The constraints of one unit, or of a whole program at link.
class FnSlots {
public:
  void add(SlotConstraint constraint);
  void addMember(std::string function, SlotKey slot);
  void addSubset(SlotKey from, SlotKey to);
  void addOpen(SlotKey slot, std::string detail);
  /// A direct call to `callee`: argument-to-parameter and
  /// result-to-receiver copies. `arguments[n]` is the slot argument `n` is
  /// read from, or none when it is not a function pointer. A function
  /// designator argument is a member of `SlotKey::param(callee, n)`
  /// (`addMember`).
  void addDirectCall(std::string_view callee,
                     std::span<const std::optional<SlotKey>> arguments,
                     const std::optional<SlotKey> &receiver);
  /// An indirect call through `callee`, the dynamic call constraint:
  /// argument `n` flows into `SlotKey::callParam(callee, n)`, and
  /// `SlotKey::callResult(callee)` flows to the receiver. A function
  /// designator argument is a member of the call-param slot.
  void addIndirectCall(const SlotKey &callee,
                       std::span<const std::optional<SlotKey>> arguments,
                       const std::optional<SlotKey> &receiver);
  /// The union with `other`'s constraints (the link step).
  void merge(const FnSlots &other);

  /// Sorted and without duplicates.
  [[nodiscard]] const std::set<SlotConstraint> &constraints() const noexcept {
    return all;
  }
  [[nodiscard]] bool empty() const noexcept { return all.empty(); }

  /// Solves the constraints under `rules`: set inclusion to a fixpoint over
  /// function names, adding the dynamic call inclusions as targets appear.
  [[nodiscard]] SlotSolution solve(const SlotRules &rules) const;

  /// The same constraints over non-local slots only (§9.3: "with local
  /// slots eliminated"). Values that pass through locals are connected
  /// directly: a local's sources flow to its sinks, a call through a local
  /// becomes a call through each non-local slot the local reads (or a direct
  /// call of each function stored in it), and a call through a local that
  /// holds an outside value becomes a call of `UnknownFunction`. Solving the
  /// result gives every non-local slot the same targets and openness as
  /// solving the original.
  [[nodiscard]] FnSlots withoutLocals() const;

  /// The §13.1 export rows: one per destination slot, sorted.
  [[nodiscard]] std::vector<SlotRow> rows() const;
  [[nodiscard]] static FnSlots fromRows(std::span<const SlotRow> rows);

  /// The textual form: one constraint per line,
  ///
  ///   member<TAB><function><TAB><slot>
  ///   subset<TAB><from><TAB><to>
  ///   open<TAB><slot><TAB><detail>
  ///
  /// with `\`, tab and newline escaped as `\\`, `\t` and `\n`.
  [[nodiscard]] std::string print() const;
  [[nodiscard]] static std::optional<FnSlots>
  parse(std::string_view text, std::string *error = nullptr);

  friend bool operator==(const FnSlots &, const FnSlots &) = default;

private:
  std::set<SlotConstraint> all;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_FNSLOTS_H
