//===- DataflowStrings.cpp - String facts and checks (RFC 0012) -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0012, *String facts*: what the checker knows about the NUL-terminated
// string an object holds (its length, or that it has no terminator), where
// the knowledge comes from (literals, initialisers, `strlen`, the copying
// functions of the library, byte stores), and what it is checked against
// (the needs of `strcpy`, `strcat` and `sprintf`; terminator-seeking reads
// of an object known to have no terminator). The facts live on the spatial
// record of the pointer place (or the array's storage place), so they
// travel with copies of the pointer as extents do.
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"

#include "clang/AST/Expr.h"
#include "clang/AST/OperationKinds.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

using namespace clang;

namespace weavec::analysis {

// -- The string rows of the library table -------------------------------------

/// The call argument that carries row argument `rowArg` of `library`, or
/// null (RFC 0030 §8: a fortified alias's arguments are remapped).
static const Expr *rowArgumentOf(const CallExpr &call,
                                 const core::LibraryMatch &library,
                                 unsigned rowArg) {
  const int index = library.callArgument(rowArg);
  if (index < 0 || static_cast<unsigned>(index) >= call.getNumArgs())
    return nullptr;
  return call.getArg(static_cast<unsigned>(index));
}

/// The call argument of a `printf`-family row's format, when the conversions
/// can be matched against the arguments (not a `v…` function).
static std::optional<unsigned>
formatIndexOf(const CallExpr &call, const core::LibraryMatch &library) {
  const auto &format = library.entry->format;
  if (!format || format->kind != core::LibFormat::Kind::Printf ||
      format->vaList)
    return std::nullopt;
  const int index = library.callArgument(format->format);
  if (index < 0 || static_cast<unsigned>(index) >= call.getNumArgs())
    return std::nullopt;
  return static_cast<unsigned>(index);
}

/// `strlen(aN) + 1`: the extent of a string the row duplicates (`strdup`).
static std::optional<unsigned> duplicatedArgument(const core::LibTerm &term) {
  if (term.kind != core::LibTerm::Kind::Sum || term.operands.size() != 2 ||
      term.operands[0].kind != core::LibTerm::Kind::StringLength ||
      term.operands[1] != core::LibTerm::constant(1))
    return std::nullopt;
  return term.operands[0].arg;
}

/// The string literal behind ordinary storage-preserving pointer casts.
static const StringLiteral *literalOf(const Expr &expr) {
  return dyn_cast<StringLiteral>(&PlaceBuilder::stripTransparent(expr));
}

/// A byte offset in elements of `unit` bytes, when the tracker follows it:
/// zero, or a constant count of one-byte elements.
static std::optional<std::int64_t>
byteOffsetOf(const core::PointerOffset &offset,
             std::optional<std::int64_t> unit) {
  if (offset.isZero())
    return 0;
  if (!offset.isElements() || unit != 1)
    return std::nullopt;
  return offset.elements;
}

// -- Subjects -----------------------------------------------------------------

std::optional<FunctionDataflow::StringSubject>
FunctionDataflow::stringSubjectOf(const Expr &arg,
                                  const core::AnalysisState &state) {
  // The element the argument's offset counts in: what the pointer points
  // at, or the array's element (`stripTransparent` looks through the
  // decay).
  const QualType type = PlaceBuilder::stripTransparent(arg).getType();
  std::optional<std::int64_t> unit;
  if (type->isPointerType())
    unit = byteSizeOf(type->getPointeeType(), context);
  else if (const auto *array = type->getAsArrayTypeUnsafe())
    unit = byteSizeOf(array->getElementType(), context);
  else
    return std::nullopt;
  const ValueOrigin origin = builder.classifyValue(arg);
  switch (origin.kind) {
  case ValueOrigin::Kind::Borrow: {
    if (!origin.place || builder.isLiteralPlace(origin.place->place))
      return std::nullopt;
    // `buf`, `&buf[2]`, `s.name`: the array's storage.
    core::PlaceId storage = origin.place->place;
    if (!places.isBase(storage) &&
        places.step(storage) == core::PathStep::Index)
      storage = *places.parent(storage);
    if (!isStorageOfVariable(storage))
      return std::nullopt;
    const auto offset = byteOffsetOf(origin.offset, unit);
    if (!offset)
      return std::nullopt;
    StringSubject subject{.key = storage,
                          .offset = *offset,
                          .extent = std::nullopt,
                          .name = nameOf(storage)};
    if (const auto record = storageRecordOf(*origin.place, {});
        record && record->extent) {
      subject.extent = KnownExtent{.have = *record->extent,
                                   .origin = record->location,
                                   .pointer = std::nullopt,
                                   .offset = {},
                                   .unit = 1,
                                   .declared = true};
    }
    return subject;
  }
  case ValueOrigin::Kind::Copy: {
    if (!origin.place || !origin.place->element.isWhole())
      return std::nullopt;
    const core::PlaceId key = origin.place->place;
    // RFC 0012, *Sized fields*: `strcpy(b->data, s)` sees the count.
    const auto record = spatialRecordAt(key, state);
    // Where the pointer itself points, then where the argument does.
    const auto own =
        byteOffsetOf(record ? record->offset : core::PointerOffset{}, unit);
    const auto step = byteOffsetOf(origin.offset, unit);
    if (!own || !step)
      return std::nullopt;
    std::int64_t offset = 0;
    if (__builtin_add_overflow(*own, *step, &offset))
      return std::nullopt;
    StringSubject subject{.key = key,
                          .offset = offset,
                          .extent = std::nullopt,
                          .name = nameOf(key)};
    if (record && record->extent) {
      subject.extent = KnownExtent{.have = *record->extent,
                                   .origin = record->location,
                                   .pointer = key,
                                   .offset = {},
                                   .unit = 1,
                                   .declared = record->declared,
                                   .extentClass = record->extentClass};
    }
    return subject;
  }
  default:
    return std::nullopt;
  }
}

std::optional<core::StringFact>
FunctionDataflow::stringFactOf(const Expr &arg,
                               const core::AnalysisState &state) {
  const auto subject = stringSubjectOf(arg, state);
  if (!subject)
    return std::nullopt;
  const auto record = state.spatial.recordOf(subject->key);
  if (!record || !record->string)
    return std::nullopt;
  core::StringFact fact = *record->string;
  if (fact.unterminated)
    return fact;
  if (!fact.length)
    return std::nullopt;
  // The length is stated from the object's start; from `offset` bytes in,
  // the string is that much shorter, when the terminator is not behind.
  if (subject->offset != 0) {
    if (!fact.length->isConstant() || fact.length->constant < subject->offset)
      return std::nullopt;
    fact.length =
        core::Affine::ofConstant(fact.length->constant - subject->offset);
  }
  return fact;
}

std::optional<core::Affine>
FunctionDataflow::stringLengthOf(const Expr &arg,
                                 const core::AnalysisState &state) {
  if (literalOf(arg) != nullptr) {
    const ValueOrigin origin = builder.classifyValue(arg);
    if (origin.literalLength)
      return core::Affine::ofConstant(*origin.literalLength);
    return std::nullopt;
  }
  const auto fact = stringFactOf(arg, state);
  if (!fact || fact->unterminated || !fact->length)
    return std::nullopt;
  return fact->length;
}

// -- Setting facts ------------------------------------------------------------

/// The storage an index place stands in (`buf[*]` is `buf`'s).
static core::PlaceId storageOfIndexed(const core::PlaceTable &places,
                                      core::PlaceId place) {
  if (!places.isBase(place) && places.step(place) == core::PathStep::Index)
    return *places.parent(place);
  return place;
}

std::vector<core::PlaceId>
FunctionDataflow::stringTargets(core::PlaceId key,
                                const core::AnalysisState &state) {
  std::vector<core::PlaceId> result{key};
  const auto add = [&result](core::PlaceId place) {
    if (!llvm::is_contained(result, place))
      result.push_back(place);
  };
  // The pointer's exact aliases hold the same value.
  for (const auto &[other, edge] : state.aliases.edgesFrom(key)) {
    if (edge.exact())
      add(other);
  }
  // The storage it borrows, when it borrows one thing, and every holder of
  // a loan on that storage; or, for storage, its holders.
  std::optional<core::PlaceId> storage;
  if (isStorageOfVariable(key)) {
    storage = key;
  } else {
    const std::vector<core::Loan> loans = state.loans.heldBy(key);
    if (loans.size() == 1 && isStorageOfVariable(loans.front().place))
      storage = storageOfIndexed(places, loans.front().place);
  }
  if (storage) {
    add(*storage);
    for (const core::Loan &loan : state.loans.loans()) {
      if (storageOfIndexed(places, loan.place) == *storage)
        add(loan.holder);
    }
  }
  return result;
}

void FunctionDataflow::setStringFact(
    core::PlaceId key, const std::optional<core::StringFact> &fact,
    core::AnalysisState &state) {
  for (const core::PlaceId target : stringTargets(key, state))
    state.spatial.setString(target, fact);
}

void FunctionDataflow::dropStringFact(core::PlaceId key,
                                      core::AnalysisState &state) {
  std::vector<core::PlaceId> targets = stringTargets(key, state);
  // A drop reaches every name that may point into the object, exact or
  // not: what was known is not known about the object under any name.
  for (const auto &[other, edge] : state.aliases.edgesFrom(key)) {
    if (!llvm::is_contained(targets, other))
      targets.push_back(other);
  }
  for (const core::PlaceId target : targets) {
    // RFC 0012, *Length places*: the old length place named the old length;
    // nothing is known about the new one.
    if (const auto length = builder.lookupLengthPlace(target)) {
      state.relations.forget(*length);
      state.scalars.forget(*length);
      state.spatial.dropExtentsOn(*length);
      state.dropGuardsOn(*length);
    }
    state.spatial.dropStringFacts(target);
  }
}

// -- Deciding comparisons -----------------------------------------------------

std::optional<bool>
FunctionDataflow::decideAtLeast(const core::Affine &a, const core::Affine &b,
                                const core::AnalysisState &state) {
  const core::Affine fa = foldAffine(a, state);
  const core::Affine fb = foldAffine(b, state);
  if (fa.isConstant() && fb.isConstant())
    return fa.constant >= fb.constant;
  if (fa.place && fb.place) {
    if (fa.scale != fb.scale || fa.scale <= 0)
      return std::nullopt;
    if (*fa.place == *fb.place)
      return fa.constant >= fb.constant;
    // `a - b = scale * (pa - pb) + (ca - cb)` under `pa REL pb + k`.
    const auto edge = state.relations.edgeBetween(*fa.place, *fb.place);
    if (!edge)
      return std::nullopt;
    std::int64_t constants = 0;
    if (__builtin_sub_overflow(fa.constant, fb.constant, &constants))
      return std::nullopt;
    const auto at = [&](std::int64_t k) -> std::optional<std::int64_t> {
      std::int64_t scaled = 0;
      std::int64_t total = 0;
      if (__builtin_mul_overflow(fa.scale, k, &scaled) ||
          __builtin_add_overflow(scaled, constants, &total))
        return std::nullopt;
      return total;
    };
    switch (edge->relation) {
    case core::Relation::Equal: {
      const auto d = at(edge->offset);
      return d ? std::optional(*d >= 0) : std::nullopt;
    }
    case core::Relation::GreaterEqual:
    case core::Relation::Greater: {
      // `pa - pb >= k` (or `k + 1`): `a - b` is at least that; decided when
      // the least is non-negative.
      const std::int64_t k = edge->relation == core::Relation::Greater
                                 ? edge->offset + 1
                                 : edge->offset;
      const auto d = at(k);
      if (d && *d >= 0)
        return true;
      return std::nullopt;
    }
    case core::Relation::LessEqual:
    case core::Relation::Less: {
      const std::int64_t k = edge->relation == core::Relation::Less
                                 ? edge->offset - 1
                                 : edge->offset;
      const auto d = at(k);
      if (d && *d < 0)
        return false;
      return std::nullopt;
    }
    }
    return std::nullopt;
  }
  // One constant against a place with a bound.
  const core::Affine &symbolic = fa.place ? fa : fb;
  const std::int64_t constant = fa.place ? fb.constant : fa.constant;
  if (symbolic.scale <= 0)
    return std::nullopt;
  const auto valueAt =
      [&symbolic](std::int64_t bound) -> std::optional<std::int64_t> {
    std::int64_t scaled = 0;
    std::int64_t total = 0;
    if (__builtin_mul_overflow(symbolic.scale, bound, &scaled) ||
        __builtin_add_overflow(scaled, symbolic.constant, &total))
      return std::nullopt;
    return total;
  };
  const auto least = state.relations.atLeast(*symbolic.place);
  const auto most = state.relations.atMost(*symbolic.place);
  if (fa.place) {
    // `a >= b`: the least `a` decides yes, the largest decides no.
    if (least) {
      if (const auto v = valueAt(*least); v && *v >= constant)
        return true;
    }
    if (most) {
      if (const auto v = valueAt(*most); v && *v < constant)
        return false;
    }
    return std::nullopt;
  }
  // `a` constant, `b` symbolic: `a >= b` when the largest `b` fits, not
  // when the least does not.
  if (most) {
    if (const auto v = valueAt(*most); v && constant >= *v)
      return true;
  }
  if (least) {
    if (const auto v = valueAt(*least); v && constant < *v)
      return false;
  }
  return std::nullopt;
}

// -- Formats ------------------------------------------------------------------

std::optional<FunctionDataflow::FormatNeed>
FunctionDataflow::formatNeedOf(const CallExpr &call, unsigned formatIndex,
                               const core::AnalysisState &state) {
  if (formatIndex >= call.getNumArgs())
    return std::nullopt;
  const StringLiteral *text = literalOf(*call.getArg(formatIndex));
  if (text == nullptr || text->getCharByteWidth() != 1)
    return std::nullopt;
  const llvm::StringRef format = text->getString();
  FormatNeed need;
  unsigned argument = formatIndex + 1;
  std::int64_t literal = 0;
  for (std::size_t i = 0; i < format.size(); ++i) {
    if (format[i] != '%') {
      ++literal;
      continue;
    }
    ++i;
    if (i >= format.size())
      break; // a trailing `%`: undefined, nothing more is known
    if (format[i] == '%') {
      ++literal;
      continue;
    }
    // Flags, width, precision, length modifiers.
    while (i < format.size() && llvm::StringRef("-+ #0").contains(format[i]))
      ++i;
    bool sized = false;
    if (i < format.size() && format[i] == '*') {
      ++argument;
      ++i;
      sized = true;
    } else {
      while (i < format.size() && llvm::isDigit(format[i])) {
        ++i;
        sized = true;
      }
    }
    if (i < format.size() && format[i] == '.') {
      ++i;
      sized = true;
      if (i < format.size() && format[i] == '*') {
        ++argument;
        ++i;
      } else {
        while (i < format.size() && llvm::isDigit(format[i]))
          ++i;
      }
    }
    while (i < format.size() && llvm::StringRef("hljztLq").contains(format[i]))
      ++i;
    if (i >= format.size())
      break;
    const char conversion = format[i];
    switch (conversion) {
    case 'c':
      // One byte, unless a width pads it.
      ++literal;
      if (sized)
        need.exact = false;
      ++argument;
      break;
    case 's': {
      if (argument < call.getNumArgs()) {
        need.stringArguments.push_back(argument);
        const auto length = stringLengthOf(*call.getArg(argument), state);
        // A precision may cut the string, a width may pad it: a known
        // length is then neither a lower bound nor exact.
        if (length && !sized) {
          if (const auto sum = sumOf(need.lower, *length))
            need.lower = *sum;
          else
            need.exact = false;
        } else {
          need.exact = false;
        }
      } else {
        need.exact = false;
      }
      ++argument;
      break;
    }
    case 'n':
      ++argument;
      break;
    default:
      // A number, a pointer: at least one byte, of unknown length.
      ++literal;
      need.exact = false;
      ++argument;
      break;
    }
  }
  const auto total = need.lower.shifted(literal);
  if (!total)
    return std::nullopt;
  need.lower = *total;
  return need;
}

// -- Effects of library calls -------------------------------------------------

void FunctionDataflow::applyStringEffects(const CallExpr &call,
                                          const core::FunctionSummary &summary,
                                          core::AnalysisState &state) {
  // RFC 0030 §8: what the row states about strings: `writes-str(d, t)`,
  // `copies(d, s, t)`, `fills(d, v, t)`, `int:value(strlen(aN))` and a
  // duplicate's `extent(strlen(aN)+1)`. The values are the arguments'
  // before the call: evaluated before anything is dropped.
  const core::LibraryMatch *library = resolvedLibrary(call);
  const auto argument = [&](unsigned rowArg) -> const Expr * {
    return library != nullptr ? rowArgumentOf(call, *library, rowArg) : nullptr;
  };
  struct Update {
    const Expr *dest = nullptr;
    std::optional<StringSubject> subject;
    enum class Kind : std::uint8_t { Length, Unterminated, LengthPlace } kind;
    std::optional<core::Affine> length;
  };
  std::vector<Update> updates;
  const auto subjectOf = [&](const Expr *expr) {
    return expr != nullptr ? stringSubjectOf(*expr, state)
                           : std::optional<StringSubject>();
  };
  // `n + offset >= extent(d)`: the write covers the object to its end.
  const auto coversObject = [&](const std::optional<StringSubject> &subject,
                                const core::Affine &count) -> bool {
    if (!subject || !subject->extent)
      return false;
    const auto end = count.shifted(subject->offset);
    return end && decideAtLeast(*end, subject->extent->have, state) == true;
  };
  const core::LibraryEntry *row = library != nullptr ? library->entry : nullptr;
  if (row != nullptr) {
    for (const core::LibStringWrite &write : row->writesString) {
      const Expr *dest = argument(write.dst);
      if (dest == nullptr || !write.length)
        continue;
      bool lowerBound = false;
      const auto length =
          stringTermValue(*write.length, call, *library, state, lowerBound);
      if (length && !lowerBound)
        updates.push_back({.dest = dest,
                           .subject = subjectOf(dest),
                           .kind = Update::Kind::Length,
                           .length = length});
    }
    for (const core::LibCopy &copy : row->copies) {
      // `n` bytes from a string of `len` bytes (`strncpy` copies
      // `min(n, len + 1)`): no terminator among them when `len >= n`, so a
      // copy that fills the object leaves none; the whole string and its
      // terminator when `len < n`.
      const core::LibTerm *bound = &copy.length;
      if (bound->kind == core::LibTerm::Kind::Min &&
          bound->operands.size() == 2)
        bound = bound->operands.data();
      const Expr *dest = argument(copy.dst);
      const Expr *source = argument(copy.src);
      const Expr *count = bound->kind == core::LibTerm::Kind::Argument
                              ? argument(bound->arg)
                              : nullptr;
      if (dest == nullptr || source == nullptr || count == nullptr)
        continue;
      const auto sourceLength = stringLengthOf(*source, state);
      const auto n = builder.affineOf(*count);
      if (!sourceLength || !n)
        continue;
      const auto subject = subjectOf(dest);
      const auto atLeast = decideAtLeast(*sourceLength, *n, state);
      if (atLeast == true && coversObject(subject, *n))
        updates.push_back({.dest = dest,
                           .subject = subject,
                           .kind = Update::Kind::Unterminated,
                           .length = {}});
      else if (atLeast == false)
        updates.push_back({.dest = dest,
                           .subject = subject,
                           .kind = Update::Kind::Length,
                           .length = sourceLength});
    }
    for (const core::LibFill &fill : row->fills) {
      const Expr *dest = argument(fill.dst);
      const Expr *count = fill.length.kind == core::LibTerm::Kind::Argument
                              ? argument(fill.length.arg)
                              : nullptr;
      std::optional<std::int64_t> byte;
      if (fill.value.kind == core::LibTerm::Kind::Constant)
        byte = fill.value.value;
      else if (const Expr *value =
                   fill.value.kind == core::LibTerm::Kind::Argument
                       ? argument(fill.value.arg)
                       : nullptr)
        byte = integerConstant(*value, context);
      if (dest == nullptr || count == nullptr || !byte)
        continue;
      const auto n = builder.affineOf(*count);
      if (!n)
        continue;
      const auto subject = subjectOf(dest);
      if (*byte != 0) {
        if (coversObject(subject, *n))
          updates.push_back({.dest = dest,
                             .subject = subject,
                             .kind = Update::Kind::Unterminated,
                             .length = {}});
      } else if (subject && subject->offset == 0 &&
                 decideAtLeast(*n, core::Affine::ofConstant(1), state) ==
                     true) {
        updates.push_back({.dest = dest,
                           .subject = subject,
                           .kind = Update::Kind::Length,
                           .length = core::Affine::ofConstant(0)});
      }
    }
    // RFC 0012, *Length places*: `strlen(s)`'s value, and the length a
    // duplicate (`strdup(s)`) measures, is the length place of the string.
    std::optional<unsigned> measured;
    if (row->result.value &&
        row->result.value->kind == core::LibTerm::Kind::StringLength)
      measured = row->result.value->arg;
    else if (row->result.kind == core::LibraryResult::Kind::Fresh &&
             row->result.extent)
      measured = duplicatedArgument(*row->result.extent);
    if (const Expr *string = measured ? argument(*measured) : nullptr;
        string != nullptr &&
        string->IgnoreParenImpCasts()->getType()->isPointerType() &&
        byteSizeOf(string->IgnoreParenImpCasts()->getType()->getPointeeType(),
                   context) == 1)
      updates.push_back({.dest = string,
                         .subject = subjectOf(string),
                         .kind = Update::Kind::LengthPlace,
                         .length = {}});
  }

  // 1. Every object the callee writes loses what was known about its
  //    string; the updates say what is known now.
  for (unsigned i = 0; i < call.getNumArgs(); ++i) {
    if (!summary.effectOf(core::SummaryPath::param(i).deref()).written)
      continue;
    if (const auto subject = stringSubjectOf(*call.getArg(i), state))
      dropStringFact(subject->key, state);
  }

  for (const Update &update : updates) {
    const auto &subject = update.subject;
    if (!subject)
      continue;
    if (update.kind == Update::Kind::Unterminated) {
      setStringFact(subject->key,
                    core::StringFact{.length = std::nullopt,
                                     .unterminated = true,
                                     .location = locate(call)},
                    state);
      continue;
    }
    if (update.kind == Update::Kind::Length) {
      const auto length = update.length->shifted(subject->offset);
      if (!length)
        continue;
      // A string the object provably cannot hold (`length + 1 > extent`):
      // the call was reported, and what the object holds now is anyone's
      // guess. Unknown, so the report is the one and not the first.
      if (subject->extent) {
        if (const auto end = length->shifted(1);
            end && decideAtLeast(*end, subject->extent->have, state) == true &&
            decideAtLeast(subject->extent->have, *end, state) != true) {
          dropStringFact(subject->key, state);
          continue;
        }
      }
      setStringFact(subject->key,
                    core::StringFact{.length = length,
                                     .unterminated = false,
                                     .location = locate(call)},
                    state);
      continue;
    }
    // The length place: when the length is already known the place equals
    // it, otherwise the place *is* the length from here on.
    const core::PlaceId length = builder.lengthPlace(subject->key);
    const auto fact = stringFactOf(*update.dest, state);
    if (fact && fact->unterminated)
      continue;
    state.relations.forget(length);
    state.scalars.forget(length);
    const core::ValueFact natural =
        core::ValueFact::of({core::Outcome::Zero, core::Outcome::Positive});
    if (fact && fact->length) {
      const core::Affine known = foldAffine(*fact->length, state);
      if (known.isConstant()) {
        state.scalars.set(length, core::ValueFact::ofConstant(known.constant));
      } else if (*known.place != length && known.scale == 1) {
        state.relations.learn(length, core::Relation::Equal, *known.place,
                              known.constant);
        state.scalars.set(length, natural);
      } else if (*known.place != length) {
        state.scalars.set(length, natural);
      }
      continue;
    }
    state.scalars.set(length, natural);
    if (const auto end = core::Affine::ofPlace(length).shifted(subject->offset))
      setStringFact(subject->key,
                    core::StringFact{.length = end,
                                     .unterminated = false,
                                     .location = locate(call)},
                    state);
  }
}

std::optional<core::Affine> FunctionDataflow::stringTermValue(
    const core::LibTerm &term, const CallExpr &call,
    const core::LibraryMatch &library, const core::AnalysisState &state,
    bool &lowerBound) {
  const auto operand = [&](std::size_t i) -> std::optional<core::Affine> {
    return i < term.operands.size()
               ? stringTermValue(term.operands[i], call, library, state,
                                 lowerBound)
               : std::nullopt;
  };
  switch (term.kind) {
  case core::LibTerm::Kind::FormatLength: {
    const auto index = formatIndexOf(call, library);
    if (!index || library.callArgument(term.arg) != static_cast<int>(*index))
      return std::nullopt;
    const auto need = formatNeedOf(call, *index, state);
    if (!need)
      return std::nullopt;
    lowerBound = lowerBound || !need->exact;
    return need->lower;
  }
  case core::LibTerm::Kind::Sum: {
    const auto lhs = operand(0);
    const auto rhs = operand(1);
    return lhs && rhs ? sumOf(*lhs, *rhs) : std::nullopt;
  }
  case core::LibTerm::Kind::Difference: {
    const auto lhs = operand(0);
    return lhs ? lhs->shifted(-term.value) : std::nullopt;
  }
  default:
    return libraryValue(term, call, library, state);
  }
}

std::optional<std::pair<core::Affine, core::StringFact>>
FunctionDataflow::duplicatedStringOf(const CallExpr &call,
                                     const core::AnalysisState &state) {
  // A fresh string of `strlen(aN) + 1` bytes (`strdup`): the length of
  // argument N, when it is known.
  const core::LibraryMatch *library = resolvedLibrary(call);
  if (library == nullptr ||
      library->entry->result.kind != core::LibraryResult::Kind::Fresh ||
      !library->entry->result.extent)
    return std::nullopt;
  const auto duplicated = duplicatedArgument(*library->entry->result.extent);
  const Expr *string =
      duplicated ? rowArgumentOf(call, *library, *duplicated) : nullptr;
  if (string == nullptr)
    return std::nullopt;
  const auto length = stringLengthOf(*string, state);
  if (!length)
    return std::nullopt;
  const auto extent = length->shifted(1);
  if (!extent)
    return std::nullopt;
  return std::make_pair(*extent, core::StringFact{.length = length,
                                                  .unterminated = false,
                                                  .location = locate(call)});
}

// -- Checks -------------------------------------------------------------------

void FunctionDataflow::checkStringArguments(
    const CallExpr &call, const core::FunctionSummary &summary,
    const core::AnalysisState &state) {
  (void)summary;
  if (!recording())
    return;
  const core::LibraryMatch *library = resolvedLibrary(call);
  if (library == nullptr)
    return;
  const core::LibraryEntry &row = *library->entry;

  // 0. RFC 0030 §8.2: a literal format reads at most the variadic arguments
  //    passed (too few is a read past them); a format that is no literal
  //    leaves the call's spatial facet inexpressible.
  if (row.format) {
    const int format = library->callArgument(row.format->format);
    const int first = library->callArgument(row.format->first);
    const StringLiteral *text =
        format >= 0 && static_cast<unsigned>(format) < call.getNumArgs()
            ? literalOf(*call.getArg(static_cast<unsigned>(format)))
            : nullptr;
    const SiteInfo *site = siteFor(call, core::Facet::Spatial);
    const auto passed =
        first >= 0 && call.getNumArgs() > static_cast<unsigned>(first)
            ? call.getNumArgs() - static_cast<unsigned>(first)
            : 0U;
    const auto reads =
        text != nullptr && text->getCharByteWidth() == 1 && !row.format->vaList
            ? core::formatArgumentCount(text->getString(), row.format->kind)
            : std::nullopt;
    if (text == nullptr) {
      decide(site, core::Facet::Spatial,
             core::FacetDecision::unresolvedFor(
                 core::UnresolvedReason::Inexpressible,
                 "the format of " + calleeName(call) + " is not a literal"));
    } else if (reads && *reads > passed) {
      decide(site, core::Facet::Spatial, core::FacetDecision::violation());
      report(makeError(core::diag::OutOfBounds,
                       "format string of " + calleeName(call) + " reads " +
                           std::to_string(*reads) + " arguments but " +
                           std::to_string(passed) + " are passed",
                       call),
             core::Certainty::Definite, site, core::Facet::Spatial);
    }
  }

  // 1. Terminator-seeking reads of an object with no terminator: the row's
  //    `str` arguments and the `%s` arguments of a literal format.
  const auto checkSeekingRead = [&](const Expr *arg) -> bool {
    if (arg == nullptr)
      return false;
    const auto fact = stringFactOf(*arg, state);
    if (!fact || !fact->unterminated)
      return false;
    const auto subject = stringSubjectOf(*arg, state);
    const std::string object =
        "'" + (subject ? subject->name : std::string("the argument")) + "'";
    core::Diagnostic diagnostic =
        makeError(core::diag::OutOfBounds,
                  calleeName(call) + " reads past the end of " + object +
                      ", which is not NUL-terminated",
                  *arg);
    if (fact->location.isValid())
      diagnostic.addNote(object + " is left without a terminator here",
                         fact->location);
    // RFC 0030 §3.3: the object holds no NUL on every path: definite.
    const SiteInfo *site = siteFor(call, core::Facet::Spatial);
    decide(site, core::Facet::Spatial, core::FacetDecision::violation());
    report(std::move(diagnostic), core::Certainty::Definite, site,
           core::Facet::Spatial);
    return true;
  };
  for (unsigned rowArg = 0; rowArg < row.params.size(); ++rowArg)
    if (row.params[rowArg].string &&
        checkSeekingRead(rowArgumentOf(call, *library, rowArg)))
      return;
  const auto formatIndex = formatIndexOf(call, *library);
  if (formatIndex)
    if (const auto format = formatNeedOf(call, *formatIndex, state))
      for (const unsigned index : format->stringArguments)
        if (checkSeekingRead(index < call.getNumArgs() ? call.getArg(index)
                                                       : nullptr))
          return;

  // 2. A destination whose need is a string's length or a format's output
  //    (`strcpy`, `stpcpy`, `strcat`, `sprintf`) against its extent.
  for (unsigned rowArg = 0; rowArg < row.params.size(); ++rowArg) {
    const core::LibraryParam &param = row.params[rowArg];
    if (!param.bytes ||
        (!param.bytes->mentions(core::LibTerm::Kind::StringLength) &&
         !param.bytes->mentions(core::LibTerm::Kind::FormatLength)))
      continue;
    const Expr *dest = rowArgumentOf(call, *library, rowArg);
    if (dest == nullptr)
      continue;
    bool lowerBound = false;
    const auto need =
        stringTermValue(*param.bytes, call, *library, state, lowerBound);
    if (!need)
      continue;
    const auto subject = stringSubjectOf(*dest, state);
    if (!subject)
      continue;
    const auto total = need->shifted(subject->offset);
    if (!total)
      continue;
    if (!subject->extent) {
      // RFC 0012, *String checks*: a constant need on a parameter of unknown
      // extent is what this function requires of its caller.
      if (total->isConstant() && !lowerBound)
        noteExtentRequirement(subject->key, *total, state);
      continue;
    }
    reportBounds(*total, *subject->extent, *dest, {}, subject->name, nullptr,
                 &call, state, lowerBound);
  }
}

// -- Stores and initialisers --------------------------------------------------

void FunctionDataflow::noteByteStore(const Expr &lvalue, const Expr *value,
                                     core::AnalysisState &state) {
  if (byteSizeOf(lvalue.getType(), context) != 1)
    return;
  const auto access = accessOf(lvalue);
  if (!access)
    return;
  std::optional<StringSubject> subject;
  if (access->storage != nullptr) {
    if (isa<ParmVarDecl>(access->storage))
      return;
    const core::PlaceId storage = builder.placeForVar(*access->storage);
    subject = StringSubject{.key = storage,
                            .offset = 0,
                            .extent = std::nullopt,
                            .name = nameOf(storage)};
  } else if (access->base != nullptr) {
    subject = stringSubjectOf(*access->base, state);
  }
  if (!subject)
    return;
  const auto record = state.spatial.recordOf(subject->key);
  const std::optional<core::StringFact> before =
      record ? record->string : std::nullopt;
  // The byte's index from the object's start, in the same counter as the
  // length when both are in one (`d[n] = 0` after `strcpy(d, s)` with `n =
  // strlen(s)`).
  const core::Affine index = foldAffine(access->start, state);
  const auto shifted = index.shifted(subject->offset);
  if (!shifted)
    return;
  const core::Affine at = *shifted;
  // `at - length` when the two are comparable: both constants, or the same
  // place at the same scale.
  const auto pastTheLength = [&]() -> std::optional<std::int64_t> {
    if (!before || !before->length)
      return std::nullopt;
    const core::Affine length = foldAffine(*before->length, state);
    if (at.isConstant() && length.isConstant())
      return at.constant - length.constant;
    if (at.place && length.place && *at.place == *length.place &&
        at.scale == length.scale)
      return at.constant - length.constant;
    return std::nullopt;
  }();
  const auto byte =
      value != nullptr ? integerConstant(*value, context) : std::nullopt;
  if (!byte) {
    // An unknown byte: nothing survives.
    dropStringFact(subject->key, state);
    return;
  }
  if (*byte == 0) {
    // A terminator at `at`: the string is at most that long. One at or
    // past the known terminator changes nothing.
    if (before && !before->unterminated && before->length && pastTheLength &&
        *pastTheLength >= 0)
      return;
    // The new length: zero for a NUL at the start; `at` when it falls
    // before the known terminator, or when there was none anywhere (so
    // nothing earlier can be one); unknown otherwise (it may be at most
    // `at`, and an earlier byte may already be a NUL).
    std::optional<core::Affine> length;
    if (at.isConstant() && at.constant == 0)
      length = core::Affine::ofConstant(0);
    else if (pastTheLength || (before && before->unterminated))
      length = at;
    if (length) {
      setStringFact(subject->key,
                    core::StringFact{.length = length,
                                     .unterminated = false,
                                     .location = locate(lvalue)},
                    state);
    } else {
      dropStringFact(subject->key, state);
    }
    return;
  }
  // A non-NUL byte: an object with no terminator still has none; a known
  // terminator elsewhere still stands, one overwritten is gone.
  if (!before)
    return;
  if (before->unterminated)
    return;
  if (pastTheLength && *pastTheLength != 0)
    return;
  dropStringFact(subject->key, state);
}

void FunctionDataflow::initStringStorage(core::PlaceId storage,
                                         const VarDecl &var,
                                         core::AnalysisState &state) {
  const Expr *init = var.getInit();
  if (init == nullptr)
    return;
  const auto *array = context.getAsConstantArrayType(var.getType());
  if (array == nullptr || byteSizeOf(array->getElementType(), context) != 1)
    return;
  const std::int64_t size = array->getSize().getSExtValue();
  const Expr &value = *init->IgnoreParens();
  std::optional<core::StringFact> fact;
  if (const auto *text = dyn_cast<StringLiteral>(&value)) {
    if (text->getCharByteWidth() != 1)
      return;
    const llvm::StringRef bytes = text->getString();
    const std::size_t nul = bytes.find('\0');
    const auto length = static_cast<std::int64_t>(
        nul == llvm::StringRef::npos ? bytes.size() : nul);
    // `char a[4] = "abcd"`: the terminator does not fit.
    if (length >= size && nul == llvm::StringRef::npos)
      fact = core::StringFact{.unterminated = true};
    else
      fact = core::StringFact{.length = core::Affine::ofConstant(length)};
  } else if (const auto *list = dyn_cast<InitListExpr>(&value)) {
    // `{'a', 'b', 0}`: the first zero; `{'a', 'b'}` in four: zero-filled
    // after; every byte a non-zero constant: no terminator.
    std::int64_t index = 0;
    bool allNonZero = true;
    std::optional<std::int64_t> firstNul;
    for (const Expr *element : list->inits()) {
      const auto byte = integerConstant(*element, context);
      if (!byte) {
        allNonZero = false;
        break;
      }
      if (*byte == 0) {
        firstNul = index;
        break;
      }
      ++index;
    }
    const auto count = static_cast<std::int64_t>(list->getNumInits());
    if (firstNul)
      fact = core::StringFact{.length = core::Affine::ofConstant(*firstNul)};
    else if (allNonZero && count < size)
      fact = core::StringFact{.length = core::Affine::ofConstant(count)};
    else if (allNonZero && count == size && size > 0)
      fact = core::StringFact{.unterminated = true};
  }
  if (!fact)
    return;
  fact->location = locate(var.getLocation());
  state.spatial.setString(storage, std::move(fact));
}

} // namespace weavec::analysis
