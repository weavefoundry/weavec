//===- PlaceBuilder.h - Clang expressions to core places -------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Maps Clang lvalue expressions onto structured `core::PlaceId`s (RFC 0002,
// *Places*) and classifies pointer-typed rvalues by how they were produced
// (allocation, copy, borrow, ...). Pointer arithmetic and pointer-to-pointer
// casts preserve the identity of the object referred to (RFC 0004, *Pointer
// identity*); an integer-to-pointer conversion yields a *raw* value; anything
// else the mapping cannot express is *opaque*: no place and no facts.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_LIB_ANALYSIS_PLACEBUILDER_H
#define WEAVEC_LIB_ANALYSIS_PLACEBUILDER_H

#include "weavec/Analysis/Summaries.h"
#include "weavec/Core/Moves.h"
#include "weavec/Core/Offset.h"
#include "weavec/Core/Place.h"
#include "weavec/Core/Raw.h"
#include "weavec/Core/Spatial.h"
#include "weavec/Core/Summary.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"

#include "llvm/ADT/DenseMap.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace weavec::analysis {

struct CallEffects;

/// The mathematical value of an integer constant expression, if it has one
/// that an `int64_t` holds: a signed constant as itself, an unsigned one only
/// when it is below 2^63 (`ULONG_MAX` and `(size_t)-1` have none). This is
/// the value RFC 0009's classes are of (*Assumptions*: "an integer's class is
/// that of its mathematical value"); reading an unsigned constant's bits as
/// signed would make `SIZE_MAX` negative.
[[nodiscard]] std::optional<std::int64_t>
integerConstant(const clang::Expr &expr, const clang::ASTContext &context);

/// The mathematical value `value` has once converted to the integer type
/// `type` (C's usual arithmetic conversions: modulo 2^N for an unsigned
/// type, two's-complement truncation for a signed one), if an `int64_t` holds
/// it. `case -1:` on an `unsigned` scrutinee selects `UINT_MAX`.
[[nodiscard]] std::optional<std::int64_t>
integerConvertedTo(std::int64_t value, clang::QualType type,
                   const clang::ASTContext &context);

/// A place denoted by an lvalue expression together with the pointer places
/// that had to be dereferenced to reach it (each of those is *read* by the
/// access, so a moved one is a use-after-free).
struct PlaceRef {
  core::PlaceId place;
  std::vector<core::PlaceId> derefs;
  /// The expression naming each entry of `derefs` (parallel vector; entries
  /// may be null when the dereference was synthesised rather than written).
  std::vector<const clang::Expr *> derefExprs;
  /// The witness in force for each entry of `derefs` (parallel vector): the
  /// element the dereferenced pointer was read from (`a[i]` in `a[i]->x`).
  std::vector<core::ElementWitness> derefElements;
  /// Which element the access named: the nearest subscript on the path
  /// (RFC 0006, *Element witnesses*), `Whole` when there is none, `Unknown`
  /// when a second subscript follows it (`m[i][j]`).
  core::ElementWitness element;

  void addDeref(core::PlaceId pointer, const clang::Expr *expr) {
    derefs.push_back(pointer);
    derefExprs.push_back(expr);
    derefElements.push_back(element);
  }
};

/// How a pointer-typed rvalue was produced.
struct ValueOrigin {
  enum class Kind : std::uint8_t {
    /// A recognised allocation call: fresh owned resource.
    Alloc,
    /// A plain copy of the pointer stored in `place`.
    Copy,
    /// The address of `place` (`&x`, array decay, `&a[i]`, `&p->f`).
    Borrow,
    /// A null pointer constant.
    Null,
    /// `c ? a : b`; see `alternatives`.
    Conditional,
    /// A raw pointer (RFC 0004): cast from an integer, handed out by a
    /// callee as raw, or returned by unchecked code under
    /// `--strict-externs`. See `rawReason`.
    Raw,
    /// Anything else: no facts can be derived.
    Opaque,
  };

  Kind kind = Kind::Opaque;
  /// Copy: the source pointer place. Borrow: the borrowed place.
  std::optional<PlaceRef> place;
  /// Alloc/Raw (callee-produced), or a value returned by a call whose
  /// consumption depends on its result: the call.
  const clang::CallExpr *call = nullptr;
  /// Copy, Alloc, Borrow (RFC 0011, *Derived pointers*): where in its
  /// object the value points. `p + 1` is one element past `p`; `&p->in` is
  /// the field `in`; `container_of(q, T, in)` is that field subtracted;
  /// `strchr(s, c)` is somewhere unknown. A copy at a non-zero offset points
  /// into the same object as `place` but not at the same address (RFC 0006,
  /// *Alias exactness*).
  core::PointerOffset offset;
  /// Alloc (RFC 0011): the extent of the allocation in bytes, when the size
  /// argument or the callee's summary says.
  std::optional<core::Affine> extent;
  /// Raw (integer cast): the cast expression, where notes point.
  const clang::Expr *source = nullptr;
  /// Conditional: the two arms.
  std::vector<ValueOrigin> alternatives;
  /// Borrow: whether the borrowed object is `const`-qualified, in which case
  /// the borrow is shared regardless of the destination type.
  bool constObject = false;
  /// Raw: why.
  core::RawReason rawReason = core::RawReason::IntegerCast;
  /// Alloc: the release family of the allocation (RFC 0007); empty when
  /// unknown.
  std::string family;
  /// RFC 0009: the value has this origin only when the guard holds (a
  /// callee's argument-conditional return or store, translated to the
  /// caller's places). Trivial for anything else. A refuted alternative is
  /// dropped by the dataflow before the value is applied.
  core::PlaceGuard guard;
};

class PlaceBuilder {
public:
  PlaceBuilder(core::PlaceTable &table, SummaryStore &summaryStore,
               const clang::ASTContext &astContext)
      : places(table), summaries(summaryStore), context(astContext) {}

  /// The base place for `var`, created on first use.
  core::PlaceId placeForVar(const clang::VarDecl &var);

  /// The base place for `var` if one has been created.
  [[nodiscard]] std::optional<core::PlaceId>
  lookupVar(const clang::VarDecl &var) const;

  /// The place of `member` within the record at `parent`, created on first
  /// use and remembered for `declFor`.
  [[nodiscard]] core::PlaceId fieldPlace(core::PlaceId parent,
                                         const clang::ValueDecl &member);

  /// The variable a base place stands for; null for the string-literal
  /// place.
  [[nodiscard]] const clang::VarDecl *varForPlace(core::PlaceId place) const;

  /// The synthetic place standing for every string literal in the function
  /// (RFC 0008, *Invalid releases*): storage with static lifetime that is
  /// not a heap object. Created on first use.
  [[nodiscard]] core::PlaceId literalPlace();

  /// True if `place` is the string-literal place.
  [[nodiscard]] bool isLiteralPlace(core::PlaceId place) const noexcept {
    return literal && *literal == place;
  }

  /// Resolves an lvalue expression to a place path, or `std::nullopt` if it
  /// is opaque or not a place at all.
  [[nodiscard]] std::optional<PlaceRef> resolve(const clang::Expr &expr);

  /// The element witness of a subscript expression (RFC 0006): an integer
  /// constant, a variable, or unknown.
  [[nodiscard]] core::ElementWitness witnessOf(const clang::Expr &index);

  /// For a place expression that `resolve` cannot map because its
  /// dereferenced base is not a place (`((T *)(uintptr_t)x)->f`), the
  /// origin of that base if it is a raw value; otherwise `std::nullopt`.
  [[nodiscard]] std::optional<ValueOrigin>
  rawBaseOf(const clang::Expr &placeExpr);

  /// True if the variable or field `place` names is declared `WEAVEC_RAW`
  /// (RFC 0004, *Annotation surface*): every value read from it is raw.
  [[nodiscard]] bool isDeclaredRaw(core::PlaceId place) const;

  /// The declaration `place` names (its variable for a base, its field for
  /// a field step), for locating notes; null when unknown.
  [[nodiscard]] const clang::NamedDecl *declFor(core::PlaceId place) const;

  /// Under `--strict-externs`, a call into code with no summary yields a raw
  /// result rather than an unknown one (RFC 0004, *Boundaries*).
  void setStrictExterns(bool strict) noexcept { strictExterns = strict; }

  /// Resolves an expression yielding a pointer *value* to the place that
  /// pointer is stored in (`p`, `s.p`, `q->next`), looking through parens
  /// and transparent casts.
  [[nodiscard]] std::optional<PlaceRef>
  resolvePointerValue(const clang::Expr &expr);

  /// Classifies a pointer-typed rvalue. Calls are classified through the
  /// callee's summary (RFC 0003): a fresh return is an allocation, a return
  /// of argument `k` is a copy of that argument, and so on.
  [[nodiscard]] ValueOrigin classifyValue(const clang::Expr &expr);

  /// The place a value is a copy of: a `Copy`'s place, or the one place the
  /// non-null alternatives of a `Conditional` all copy (`obj_ref(p)`, whose
  /// result is `p` or null; RFC 0010). `std::nullopt` otherwise.
  [[nodiscard]] static std::optional<PlaceRef>
  copyOrNull(const ValueOrigin &origin);
  /// The caller-side place a summary path denotes at `call` (RFC 0003,
  /// *Applying a summary at a call*): `param(i)` is the place holding the
  /// `i`-th argument (or the place it copies, `copyOrNull`), `param(i)*`
  /// what it points to (or `x` itself when the argument is `&x`),
  /// `global(g)` the global's place. `std::nullopt` when the argument is not
  /// a place.
  [[nodiscard]] std::optional<PlaceRef>
  resolveSummaryPath(const core::SummaryPath &path,
                     const clang::CallExpr &call);

  /// The place `path`'s steps reach from `base` (the record a `result` path
  /// was assigned to, RFC 0008, *Struct-by-value results*). Fields and
  /// indices only; a path with a dereference is `std::nullopt`.
  [[nodiscard]] std::optional<core::PlaceId>
  resolveBelow(core::PlaceId base, const core::SummaryPath &path);

  /// Like `resolveSummaryPath`, but only finds a place this function has
  /// already named: nothing is interned, and a path below an unknown place
  /// is `std::nullopt`. For effects that touch only what is known (a
  /// callee's writes forgetting the facts below a place).
  [[nodiscard]] std::optional<core::PlaceId>
  lookupSummaryPath(const core::SummaryPath &path, const clang::CallExpr &call);

  /// State for `lookupSummaryPath` over a run of paths in summary order at
  /// one call: consecutive paths share a root and a prefix, so the root is
  /// classified once and the previous chain of places is extended rather
  /// than rebuilt (a Lua summary names dozens of written fields below one
  /// state pointer, applied at thousands of calls).
  struct PathLookupCache {
    std::optional<core::SummaryPath> last;
    /// `chain[k]` is the place after `k` steps past `firstStep` of `last`;
    /// shorter than the steps when a step found nothing.
    std::vector<core::PlaceId> chain;
    std::size_t firstStep = 0;
    bool rootKnown = false;
  };
  [[nodiscard]] std::optional<core::PlaceId>
  lookupSummaryPath(const core::SummaryPath &path, const clang::CallExpr &call,
                    PathLookupCache &cache);

  /// Translates a summary value source at `call` into a caller value
  /// origin. A copy of an argument the callee consumed is reported as a
  /// fresh allocation: ownership went in and came back out. The source's
  /// guard becomes the origin's (RFC 0009); a source whose guard the
  /// arguments refute outright is `std::nullopt`.
  [[nodiscard]] std::optional<ValueOrigin>
  originFromSource(const core::ValueSource &source, const clang::CallExpr &call,
                   const core::FunctionSummary &of);
  /// `originFromSource` without the guard.
  [[nodiscard]] ValueOrigin
  originFromUnguardedSource(const core::ValueSource &source,
                            const clang::CallExpr &call,
                            const core::FunctionSummary &of);

  /// Translates a callee's guard to the caller's places at `call` (RFC 0009,
  /// *Deriving guards, at a call*): `param i` is the class of a constant
  /// argument (decided on the spot) or the caller place that holds the
  /// argument, through casts and multiplication by a positive constant;
  /// paths below an argument and globals resolve as `resolveSummaryPath`
  /// does. A conjunct with no caller place is dropped. `std::nullopt` when a
  /// constant argument refutes a conjunct: the guarded effect does not
  /// happen at this call.
  [[nodiscard]] std::optional<core::PlaceGuard>
  translateGuard(const core::PathGuard &guard, const clang::CallExpr &call);

  /// The integer place an integer-valued expression reads, looking through
  /// parentheses, casts and multiplication by a positive constant (whose
  /// zero-ness and sign it shares), if it reads one; and the fact the
  /// expression itself establishes when it is a constant.
  struct ScalarOperand {
    std::optional<PlaceRef> place;
    std::optional<core::ValueFact> constant;
    /// The value is the place's scaled or converted, not the place's own:
    /// an exact constant on the place says nothing exact about the value.
    bool scaled = false;
    /// RFC 0010, *Recognising increments and decrements*: the value is the
    /// place's *after* the expression plus this offset (`x--` yields the
    /// old value, `x + 1`; `--x` yields `x`). Zero for a plain read.
    std::int64_t offset = 0;
  };
  [[nodiscard]] ScalarOperand scalarOperand(const clang::Expr &expr);

  /// RFC 0011: the value of an integer expression as `scale * place +
  /// constant`: a constant, an integer place, `x + k`, `x - k`, `k * x`,
  /// through parentheses and integral casts. Nothing for any other shape.
  [[nodiscard]] std::optional<core::Affine> affineOf(const clang::Expr &expr);

  /// RFC 0011: the key of the field path `fields` below the record type
  /// `record`, spelled as RFC 0010 spells count fields (`struct outer.in`);
  /// empty when the path does not name fields of the record.
  [[nodiscard]] std::string
  fieldKeyFor(clang::QualType record,
              llvm::ArrayRef<core::PathElem> fields) const;

  /// RFC 0011, *Deriving a pointer*: `E` in `&E` reached through a pointer
  /// place with only field and index steps below the dereference. `&p->in`
  /// derives `p` at the field `in`; `&p[3]` derives `p` at `+3`; `&*p` is
  /// `p`. `std::nullopt` when `E` is a variable's own storage (a borrow).
  struct Derivation {
    PlaceRef pointer;
    core::PointerOffset offset;
  };
  [[nodiscard]] std::optional<Derivation>
  derivationOf(const clang::Expr &lvalue);

  /// RFC 0011: the object a pointer value refers to, as a place: the
  /// borrowed place of a borrow; for a copy of `p`, `*p` translated through
  /// the copy's offset (`&p->in` refers to `(*p).in`; an element offset
  /// refers to `*p`, the element summary; an unknown one to `*p` too).
  /// Nothing for any other origin.
  [[nodiscard]] std::optional<PlaceRef> pointeeOf(const ValueOrigin &origin);

  /// The place whose object a consumed argument (`free(E)`, an owned
  /// parameter) names: `resolvePointerValue`, or for `&p->f` / `&p[i]` the
  /// pointer the address derives from (RFC 0011, *Deriving a pointer*).
  [[nodiscard]] std::optional<PlaceRef>
  resolveConsumedValue(const clang::Expr &expr);

  /// RFC 0011: the field steps a `Field` offset spells, outermost first
  /// (`in`, `buf` for `&p->in.buf`); empty for any other offset.
  [[nodiscard]] static std::vector<std::string>
  fieldsOfOffset(const core::PointerOffset &offset);

  /// RFC 0011: the offset a pointer place's own value moves by in `++p`,
  /// `p--`, `p += k`, `p -= k`; nothing for any other expression.
  [[nodiscard]] std::optional<core::PointerOffset>
  pointerStepOf(const clang::Expr &expr);

  /// RFC 0011: the offset `p + k` / `p - k` / `(char *)p - offsetof(T, f)`
  /// steps `pointer` by; `pointer` is the pointer operand of `binary`.
  [[nodiscard]] core::PointerOffset
  arithmeticStepOf(const clang::BinaryOperator &binary,
                   const clang::Expr &pointer);

  /// RFC 0011: the extent, in the caller's places, of a callee's `PathAffine`
  /// at `call`: a `param i` root is the argument (`affineOf`, scaled), a
  /// path below one or a global is the caller place it names.
  [[nodiscard]] std::optional<core::Affine>
  affineFromPath(const core::PathAffine &affine, const clang::CallExpr &call);

  /// RFC 0010, *Recognising increments and decrements*: an expression that
  /// adds or subtracts exactly one from an integer place: `++x`, `x++`,
  /// `--x`, `x--`, `x += 1`, `x -= 1`, and the adjusting builtins
  /// (`__atomic_fetch_add(&x, 1, o)`, `__sync_sub_and_fetch(&x, 1)`, ...).
  struct Adjustment {
    /// The integer place adjusted.
    PlaceRef place;
    /// `+1` or `-1`.
    int delta = 0;
    /// The expression's value is the place's new value plus this offset:
    /// `0` for the pre-forms and `*_fetch` builtins, `-delta` for the
    /// post-forms and `fetch_*` builtins (which yield the old value).
    std::int64_t valueOffset = 0;
    /// The operand the place was reached through (`x` in `x++`, `&x` in the
    /// builtins), for locating notes; null for a builtin whose pointer
    /// argument is not an address-of.
    const clang::Expr *operand = nullptr;
  };
  [[nodiscard]] std::optional<Adjustment> adjustmentOf(const clang::Expr &expr);

  /// Longest place path a summary spells, and the longest the checker
  /// synthesises when mirroring facts between aliases. Paths written in the
  /// source are never truncated; a summary keeps what fits in a place, so a
  /// recursive structure walked across a call cycle (`L->l_G->gray->l_G->
  /// gray->...`, one more hop per fixpoint round) stops growing and the
  /// whole-program fixpoint is over a finite lattice (RFC 0011,
  /// *Whole-program widening*).
  static constexpr std::size_t MaxPlaceDepth = 8;

  /// The summary path of a place rooted at a parameter or a global of
  /// `function`, or `std::nullopt` for places rooted at locals or deeper
  /// than `MaxPlaceDepth`.
  [[nodiscard]] std::optional<core::SummaryPath>
  summaryPathOf(core::PlaceId place);

  [[nodiscard]] SummaryStore &summaryStore() noexcept { return summaries; }

  /// True if `expr` is an lvalue expression whose shape the builder
  /// understands (a variable, member, subscript or dereference).
  [[nodiscard]] static bool isPlaceExpr(const clang::Expr &expr);

  /// The pointer operand of `p + k`, `k + p` or `p - k` (the value denotes
  /// an element of whatever `p` points to), or null for any other shape.
  [[nodiscard]] static const clang::Expr *
  pointerOperandOfArithmetic(const clang::Expr &expr);

  /// True if a cast between these two types preserves place identity: every
  /// pointer-to-pointer cast does (RFC 0004, *Pointer identity*); a cast
  /// from or to an integer does not.
  [[nodiscard]] static bool isTransparentCast(clang::QualType from,
                                              clang::QualType to);

  /// Strips parens and transparent casts (implicit or explicit).
  [[nodiscard]] static const clang::Expr &
  stripTransparent(const clang::Expr &expr);

  [[nodiscard]] core::PlaceTable &table() noexcept { return places; }

  /// Variables in the order their places were created.
  [[nodiscard]] const std::vector<const clang::VarDecl *> &
  variables() const noexcept {
    return order;
  }

private:
  [[nodiscard]] std::optional<std::pair<core::PlaceId, std::size_t>>
  lookupSummaryRoot(const core::SummaryPath &path, const clang::CallExpr &call);
  /// The place `&x` (or a decayed array) names when `expr` is one; the
  /// argument shape for which `param(i)*` is `x` itself.
  [[nodiscard]] std::optional<PlaceRef> addressedPlace(const clang::Expr &expr);
  /// RFC 0011: the extent of `calloc(n, size)`-shaped calls.
  [[nodiscard]] std::optional<core::Affine>
  productExtentOf(const clang::CallExpr &call);

  core::PlaceTable &places;
  SummaryStore &summaries;
  const clang::ASTContext &context;
  bool strictExterns = false;
  llvm::DenseMap<const clang::VarDecl *, core::PlaceId> varPlaces;
  llvm::DenseMap<std::uint32_t, const clang::VarDecl *> placeVars;
  llvm::DenseMap<std::uint32_t, const clang::FieldDecl *> placeFields;
  std::vector<const clang::VarDecl *> order;
  std::optional<core::PlaceId> literal;
};

} // namespace weavec::analysis

#endif // WEAVEC_LIB_ANALYSIS_PLACEBUILDER_H
