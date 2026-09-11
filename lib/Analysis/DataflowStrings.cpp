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

// -- The string functions of the library table --------------------------------

namespace {

/// A call to a library function, as the string rules know it: the plain
/// name (`strcpy` for `__builtin___strcpy_chk`) and whether it is the
/// `_FORTIFY_SOURCE` form, whose `*printf` members insert a flag and a size
/// before the format.
struct StringCallee {
  llvm::StringRef name;
  bool checked = false;
};

/// A library function that reads an argument up to its terminator: the
/// argument indices, as a bit mask.
struct SeekingRead {
  llvm::StringLiteral name;
  unsigned arguments;

  constexpr SeekingRead(llvm::StringLiteral fn, unsigned mask)
      : name(fn), arguments(mask) {}
};

} // namespace

// clang-format off
static constexpr auto SeekingReads = std::to_array<SeekingRead>({
    {"strcpy", 0b10},   {"stpcpy", 0b10},  {"strcat", 0b11},   {"strdup", 0b1},
    {"strchr", 0b1},    {"strrchr", 0b1},  {"strstr", 0b11},   {"strpbrk", 0b11},
    {"strspn", 0b11},   {"strcspn", 0b11}, {"strcmp", 0b11},   {"strcoll", 0b11},
    {"strcasecmp", 0b11}, {"strlen", 0b1}, {"puts", 0b1},      {"fputs", 0b1},
    {"perror", 0b1},    {"system", 0b1},   {"getenv", 0b1},    {"atoi", 0b1},
    {"atol", 0b1},      {"atoll", 0b1},    {"atof", 0b1},      {"strtol", 0b1},
    {"strtoll", 0b1},   {"strtoul", 0b1},  {"strtoull", 0b1},  {"strtod", 0b1},
    {"strtof", 0b1},    {"strtold", 0b1},  {"fopen", 0b11},    {"open", 0b1},
    {"access", 0b1},    {"stat", 0b1},     {"lstat", 0b1},     {"unlink", 0b1},
    {"remove", 0b1},    {"rename", 0b11},  {"mkdir", 0b1},     {"rmdir", 0b1},
    {"chdir", 0b1},     {"opendir", 0b1},  {"strtok", 0b11},   {"strxfrm", 0b10},
    {"printf", 0b1},    {"fprintf", 0b10}, {"sprintf", 0b10},  {"snprintf", 0b100},
});
// clang-format on

static std::optional<StringCallee> stringCalleeOf(llvm::StringRef name) {
  if (name.empty())
    return std::nullopt;
  StringCallee result;
  if (name.consume_front("__builtin___"))
    result.checked = name.consume_back("_chk");
  else
    name.consume_front("__builtin_");
  result.name = name;
  return result;
}

/// The index of the format argument of a `*printf` function, or nothing
/// for anything else.
static std::optional<unsigned> formatIndexOf(const StringCallee &callee) {
  if (callee.name == "printf")
    return 0;
  if (callee.name == "fprintf" || callee.name == "sprintf" ||
      callee.name == "vsprintf" || callee.name == "vfprintf")
    return callee.checked ? 3 : 1;
  if (callee.name == "snprintf" || callee.name == "vsnprintf")
    return callee.checked ? 4 : 2;
  return std::nullopt;
}

/// The string literal `expr` is, through the array decay and parentheses.
static const StringLiteral *literalOf(const Expr &expr) {
  return dyn_cast<StringLiteral>(expr.IgnoreParenImpCasts());
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
                                   .declared = record->declared};
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
  const std::string libraryName = resolvedLibraryName(call);
  const auto callee = stringCalleeOf(libraryName);
  const llvm::StringRef name = callee ? callee->name : llvm::StringRef();
  const auto argument = [&call](unsigned index) -> const Expr * {
    return index < call.getNumArgs() ? call.getArg(index) : nullptr;
  };
  // What the sources say before anything is dropped: `strcat` reads the
  // destination's own length.
  const Expr *dest = argument(0);
  const Expr *source = argument(1);
  const auto destBefore = dest != nullptr ? stringLengthOf(*dest, state)
                                          : std::optional<core::Affine>();
  const auto sourceLength = source != nullptr ? stringLengthOf(*source, state)
                                              : std::optional<core::Affine>();
  const auto destSubject = dest != nullptr ? stringSubjectOf(*dest, state)
                                           : std::optional<StringSubject>();

  // 1. Every object the callee writes loses what was known about its
  //    string; the rules below say what is known now.
  for (unsigned i = 0; i < call.getNumArgs(); ++i) {
    if (!summary.effectOf(core::SummaryPath::param(i).deref()).written)
      continue;
    if (const auto subject = stringSubjectOf(*call.getArg(i), state))
      dropStringFact(subject->key, state);
  }
  if (!callee || !dest)
    return;

  // The object-level length a length `seen` from the destination's offset
  // establishes: `offset + seen`.
  const auto objectLength =
      [&destSubject](const core::Affine &seen) -> std::optional<core::Affine> {
    return seen.shifted(destSubject->offset);
  };
  const auto setLength = [&](const core::Affine &seen) {
    if (!destSubject)
      return;
    if (const auto length = objectLength(seen)) {
      // A string the object provably cannot hold (`length + 1 > extent`):
      // the call was reported, and what the object holds now is anyone's
      // guess. Unknown, so the report is the one and not the first.
      if (destSubject->extent) {
        if (const auto end = length->shifted(1);
            end &&
            decideAtLeast(*end, destSubject->extent->have, state) == true &&
            decideAtLeast(destSubject->extent->have, *end, state) != true) {
          dropStringFact(destSubject->key, state);
          return;
        }
      }
      setStringFact(destSubject->key,
                    core::StringFact{.length = length,
                                     .unterminated = false,
                                     .location = locate(call)},
                    state);
    }
  };
  const auto setUnterminated = [&] {
    if (!destSubject)
      return;
    setStringFact(destSubject->key,
                  core::StringFact{.length = std::nullopt,
                                   .unterminated = true,
                                   .location = locate(call)},
                  state);
  };
  // `n + offset >= extent(d)`: the write covers the object to its end.
  const auto coversObject = [&](const core::Affine &count) -> bool {
    if (!destSubject || !destSubject->extent)
      return false;
    const auto end = count.shifted(destSubject->offset);
    if (!end)
      return false;
    return decideAtLeast(*end, destSubject->extent->have, state) == true;
  };

  if (name == "strcpy" || name == "stpcpy") {
    if (sourceLength)
      setLength(*sourceLength);
    return;
  }
  if (name == "strcat") {
    if (destBefore && sourceLength) {
      if (const auto sum = sumOf(*destBefore, *sourceLength))
        setLength(*sum);
    }
    return;
  }
  if (name == "sprintf" || name == "vsprintf") {
    if (const auto index = formatIndexOf(*callee)) {
      if (const auto need = formatNeedOf(call, *index, state);
          need && need->exact && name == "sprintf")
        setLength(need->lower);
    }
    return;
  }
  if (name == "strncpy" || name == "memcpy" || name == "memmove") {
    // `n` bytes from a string of `len` bytes: no terminator among them
    // when `len >= n`, so a copy that fills the object leaves none; the
    // whole string and its terminator when `len < n`.
    const Expr *count = argument(2);
    if (count == nullptr || !sourceLength)
      return;
    const auto n = builder.affineOf(*count);
    if (!n)
      return;
    const auto atLeast = decideAtLeast(*sourceLength, *n, state);
    if (atLeast == true) {
      if (coversObject(*n))
        setUnterminated();
    } else if (atLeast == false) {
      setLength(*sourceLength);
    }
    return;
  }
  if (name == "memset") {
    const Expr *value = argument(1);
    const Expr *count = argument(2);
    if (value == nullptr || count == nullptr)
      return;
    const auto byte = integerConstant(*value, context);
    const auto n = builder.affineOf(*count);
    if (!byte || !n)
      return;
    if (*byte != 0) {
      if (coversObject(*n))
        setUnterminated();
    } else if (destSubject && destSubject->offset == 0 &&
               decideAtLeast(*n, core::Affine::ofConstant(1), state) == true) {
      setLength(core::Affine::ofConstant(0));
    }
    return;
  }
  if (name == "strlen" || name == "strdup") {
    // RFC 0012, *Length places*: the call's value is the length place of
    // the string; when the length is already known the place equals it,
    // otherwise the place *is* the length from here on. `strdup` measures
    // its argument the same way (its result's extent is read from the fact
    // when the result is assigned).
    if (!destSubject)
      return;
    const core::PlaceId length = builder.lengthPlace(destSubject->key);
    const auto fact = stringFactOf(*dest, state);
    if (fact && fact->unterminated)
      return;
    state.relations.forget(length);
    state.scalars.forget(length);
    if (fact && fact->length) {
      const core::Affine known = foldAffine(*fact->length, state);
      if (known.isConstant()) {
        state.scalars.set(length, core::ValueFact::ofConstant(known.constant));
      } else if (*known.place != length && known.scale == 1) {
        state.relations.learn(length, core::Relation::Equal, *known.place,
                              known.constant);
        state.scalars.set(length,
                          core::ValueFact::of(
                              {core::Outcome::Zero, core::Outcome::Positive}));
      } else if (*known.place != length) {
        state.scalars.set(length,
                          core::ValueFact::of(
                              {core::Outcome::Zero, core::Outcome::Positive}));
      }
      return;
    }
    state.scalars.set(length, core::ValueFact::of({core::Outcome::Zero,
                                                   core::Outcome::Positive}));
    setLength(core::Affine::ofPlace(length));
    return;
  }
}

std::optional<std::pair<core::Affine, core::StringFact>>
FunctionDataflow::duplicatedStringOf(const CallExpr &call,
                                     const core::AnalysisState &state) {
  const std::string libraryName = resolvedLibraryName(call);
  const auto callee = stringCalleeOf(libraryName);
  if (!callee || callee->name != "strdup" || call.getNumArgs() != 1)
    return std::nullopt;
  const auto length = stringLengthOf(*call.getArg(0), state);
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
  const std::string libraryName = resolvedLibraryName(call);
  const auto callee = stringCalleeOf(libraryName);
  if (!callee)
    return;
  const llvm::StringRef name = callee->name;
  const auto argument = [&call](unsigned index) -> const Expr * {
    return index < call.getNumArgs() ? call.getArg(index) : nullptr;
  };

  // 1. Terminator-seeking reads of an object with no terminator.
  const auto checkSeekingRead = [&](unsigned index) -> bool {
    const Expr *arg = argument(index);
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
    report(std::move(diagnostic));
    return true;
  };
  for (const SeekingRead &entry : SeekingReads) {
    if (entry.name != name)
      continue;
    for (unsigned index = 0; index < call.getNumArgs() && index < 32; ++index) {
      if ((entry.arguments & (1U << index)) != 0 && checkSeekingRead(index))
        return;
    }
  }
  std::optional<FormatNeed> format;
  if (const auto formatIndex = formatIndexOf(*callee)) {
    format = formatNeedOf(call, *formatIndex, state);
    if (format) {
      for (const unsigned index : format->stringArguments) {
        if (checkSeekingRead(index))
          return;
      }
    }
  }

  // 2. The needs of the copying functions against the destination.
  const Expr *dest = argument(0);
  if (dest == nullptr)
    return;
  std::optional<core::Affine> need;
  bool lowerBound = false;
  if (name == "strcpy" || name == "stpcpy") {
    if (const Expr *source = argument(1)) {
      if (const auto length = stringLengthOf(*source, state))
        need = length->shifted(1);
    }
  } else if (name == "strcat") {
    const Expr *source = argument(1);
    const auto before = stringLengthOf(*dest, state);
    const auto added =
        source != nullptr ? stringLengthOf(*source, state) : std::nullopt;
    if (before && added) {
      if (const auto sum = sumOf(*before, *added))
        need = sum->shifted(1);
    }
  } else if (name == "sprintf" && format) {
    need = format->lower.shifted(1);
    lowerBound = !format->exact;
  } else {
    return;
  }
  if (!need)
    return;
  const auto subject = stringSubjectOf(*dest, state);
  if (!subject)
    return;
  const auto total = need->shifted(subject->offset);
  if (!total)
    return;
  if (!subject->extent) {
    // RFC 0012, *String checks*: a constant need on a parameter of unknown
    // extent is what this function requires of its caller.
    if (total->isConstant() && !lowerBound)
      noteExtentRequirement(subject->key, *total, state);
    return;
  }
  reportBounds(*total, *subject->extent, *dest, {}, subject->name, nullptr,
               &call, state, lowerBound);
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
