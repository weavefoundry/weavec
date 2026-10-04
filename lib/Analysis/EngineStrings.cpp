//===- EngineStrings.cpp - String facts in the object engine --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0012 *String facts* over the object domain (RFC 0031 §4.3): what is
// known of the NUL-terminated string a pointer points to comes from the
// object's bytes (a literal's contents, the byte cells stores and fills
// wrote, a zeroed object) and from the object's string fact (`nulWithin`: a
// NUL at an offset, and none from `nulFrom` up to it), which the rows'
// `writes-str` and the measuring calls (`strlen`) set. A length known
// exactly decides a need both ways; one known only as a bound proves.
//
//===----------------------------------------------------------------------===//

#include "Engine.h"
#include "weavec/Analysis/ClangLocation.h"

#include "clang/AST/RecordLayout.h"
#include "clang/Basic/SourceManager.h"

#include <utility>

using namespace clang;

namespace weavec::analysis::engine {

/// The bytes of a string literal object, without its terminating NUL.
static std::optional<llvm::StringRef>
literalBytes(const core::ObjectInfo &info) {
  if (info.key.kind != core::ObjectKind::Literal)
    return std::nullopt;
  const auto *expr = fromHandle<Expr>(info.key.handle);
  const auto *literal = dyn_cast_or_null<StringLiteral>(expr);
  if (const auto *predefined = dyn_cast_or_null<PredefinedExpr>(expr))
    literal = predefined->getFunctionName();
  if (literal == nullptr || literal->getCharByteWidth() != 1)
    return std::nullopt;
  return literal->getBytes();
}

/// The width of the scalar at `offset` in an object of `type`, when the
/// type says (a value keeps the type it was made with, not its cell's).
static std::optional<std::int64_t>
scalarWidth(const ASTContext &context, QualType type, std::int64_t offset) {
  for (int depth = 0; depth < 16 && !type.isNull(); ++depth) {
    type = type.getCanonicalType();
    if (type->isIncompleteType() && !type->isIncompleteArrayType())
      return std::nullopt;
    if (const ArrayType *array = context.getAsArrayType(type)) {
      QualType element = array->getElementType();
      if (element->isIncompleteType())
        return std::nullopt;
      auto size = static_cast<std::int64_t>(
          context.getTypeSizeInChars(element).getQuantity());
      if (size <= 0)
        return std::nullopt;
      offset %= size;
      type = element;
      continue;
    }
    if (const RecordDecl *record = type->getAsRecordDecl()) {
      if (!record->isCompleteDefinition() || record->isUnion())
        return std::nullopt;
      const ASTRecordLayout &layout = context.getASTRecordLayout(record);
      QualType next;
      for (const FieldDecl *field : record->fields()) {
        if (field->isBitField() || field->getType()->isIncompleteType())
          continue;
        auto start = static_cast<std::int64_t>(
            layout.getFieldOffset(field->getFieldIndex()) /
            context.getCharWidth());
        auto size = static_cast<std::int64_t>(
            context.getTypeSizeInChars(field->getType()).getQuantity());
        if (offset >= start && offset < start + size) {
          next = field->getType();
          offset -= start;
          break;
        }
      }
      if (next.isNull())
        return std::nullopt;
      type = next;
      continue;
    }
    if (!type->isScalarType() || offset != 0)
      return std::nullopt;
    return static_cast<std::int64_t>(
        context.getTypeSizeInChars(type).getQuantity());
  }
  return std::nullopt;
}

/// The bytes a value of `info` stored in memory occupies.
static std::optional<std::int64_t> valueWidth(const core::SymInfo &info) {
  if (info.type == core::SymInfo::Type::Int && info.intType)
    return info.intType->isBoolean ? 1 : std::max(1U, info.intType->width / 8);
  if (info.type == core::SymInfo::Type::Pointer)
    return 8;
  return std::nullopt;
}

Transfer::Byte Transfer::valueByte(core::Sym sym,
                                   std::optional<std::int64_t> width) const {
  const core::SymInfo &info = heap.info(state, sym);
  if (info.type == core::SymInfo::Type::Pointer)
    return info.null == core::PointerNull::Null ? Byte::Zero : Byte::Unknown;
  if (info.type != core::SymInfo::Type::Int)
    return Byte::Unknown;
  if (auto c = state.zone.constant(sym); c && *c == 0)
    return Byte::Zero;
  // A non-zero value is a non-zero byte only when it is one byte wide (a
  // value converted without change keeps a wider type, `char c = 'a'`).
  if (width.value_or(valueWidth(info).value_or(0)) != 1)
    return Byte::Unknown;
  if (auto c = state.zone.constant(sym); c && (*c < -128 || *c > 255))
    return Byte::Unknown;
  auto lo = state.zone.lower(sym);
  auto hi = state.zone.upper(sym);
  if ((lo && *lo > 0) || (hi && *hi < 0))
    return Byte::NonZero;
  return Byte::Unknown;
}

Transfer::Byte Transfer::byteAt(core::ObjectId id, std::int64_t offset) const {
  const core::ObjectState *object = heap.findObject(state, id);
  if (object == nullptr || offset < 0)
    return Byte::Unknown;
  const core::ObjectInfo &info = run.table().info(id);
  if (auto bytes = literalBytes(info)) {
    if (std::cmp_greater(offset, bytes->size()))
      return Byte::Unknown;
    if (std::cmp_equal(offset, bytes->size()))
      return Byte::Zero;
    return (*bytes)[static_cast<std::size_t>(offset)] == '\0' ? Byte::Zero
                                                              : Byte::NonZero;
  }
  auto combine = [](std::optional<Byte> a, Byte b) {
    return !a || *a == b ? b : Byte::Unknown;
  };
  std::optional<Byte> byte;
  bool concrete = false;
  QualType objectType = info.type != 0 ? typeOfHandle(info.type) : QualType();
  for (const auto &[key, sym] : object->cells) {
    const core::SymInfo &value = heap.info(state, sym);
    std::optional<std::int64_t> width =
        objectType.isNull() ? std::nullopt
                            : scalarWidth(context, objectType, key.offset);
    if (!width)
      width = valueWidth(value);
    if (key.isSummary()) {
      // Every element position of the summary may hold its value.
      if (!width || offset < key.offset ||
          (offset - key.offset) % key.stride >= *width)
        continue;
      byte = combine(byte, valueByte(sym, width));
      continue;
    }
    if (offset < key.offset || !width || offset >= key.offset + *width)
      continue;
    Byte here = valueByte(sym, width);
    if (here == Byte::NonZero && key.offset != offset)
      here = Byte::Unknown;
    byte = combine(byte, here);
    concrete = true;
  }
  if (!concrete) {
    // A byte no cell holds: what the object's creation left there.
    bool zero = object->zeroed && !object->forgetsAny() &&
                !object->uninitialised &&
                (info.key.kind == core::ObjectKind::Local ||
                 info.key.kind == core::ObjectKind::HeapRecent ||
                 info.key.kind == core::ObjectKind::HeapOld);
    byte = combine(byte, zero ? Byte::Zero : Byte::Unknown);
  }
  return byte.value_or(Byte::Unknown);
}

/// `-term`.
static core::Term negated(const core::Term &term) {
  core::Term out = term;
  out.scale = -out.scale;
  out.constant = -out.constant;
  if (out.isConstant())
    out.scale = 0;
  return out;
}

Transfer::StringFacts Transfer::stringFacts(core::Sym pointer) const {
  StringFacts out;
  const core::SymInfo &value = heap.info(state, pointer);
  if (value.type != core::SymInfo::Type::Pointer || value.top ||
      value.targets.size() != 1 || !value.targets[0].offset.known)
    return out;
  const core::Target &target = value.targets[0];
  const core::ObjectState *object = heap.findObject(state, target.object);
  if (object == nullptr || object->life != core::Life::Live)
    return out;
  const core::Term &start = target.offset;
  // The object's string fact: a NUL at `nulWithin`, from any start up to it.
  if (object->nulWithin)
    if (auto rest = object->nulWithin->plus(negated(start));
        rest && rest->known &&
        heap.lessEqual(state, core::Term::of(0), *rest).value_or(false) &&
        heap.lessEqual(state, core::Term::of(0), start).value_or(false)) {
      // Exactly when no NUL lies from the start to it.
      bool exact = false;
      if (object->nulFrom) {
        exact = heap.lessEqual(state, *object->nulFrom, start).value_or(false);
        if (!exact && start.isConstant() && object->nulFrom->isConstant() &&
            object->nulFrom->constant - start.constant <= 256) {
          exact = true;
          for (std::int64_t at = start.constant; at < object->nulFrom->constant;
               ++at)
            exact = exact && byteAt(target.object, at) == Byte::NonZero;
        }
      }
      out.length =
          exact ? StringFacts::Length::Exact : StringFacts::Length::AtMost;
      out.term = *rest;
      out.nulAt = *object->nulWithin;
      return out;
    }
  if (!start.isConstant() || start.constant < 0)
    return out;
  // The bytes from the start: the first known NUL ends the string, exactly
  // when every byte before it is known not to be one.
  std::optional<std::int64_t> end;
  if (object->extent && object->extent->bytes.isConstant())
    end = object->extent->bytes.constant;
  if (auto bytes = literalBytes(run.table().info(target.object)))
    end = static_cast<std::int64_t>(bytes->size()) + 1;
  std::int64_t limit = start.constant + 256;
  if (end && *end < limit)
    limit = *end;
  bool allNonZero = true;
  for (std::int64_t at = start.constant; at < limit; ++at) {
    Byte byte = byteAt(target.object, at);
    if (byte == Byte::Zero) {
      out.length =
          allNonZero ? StringFacts::Length::Exact : StringFacts::Length::AtMost;
      out.term = core::Term::of(at - start.constant);
      out.nulAt = core::Term::of(at);
      return out;
    }
    if (byte != Byte::NonZero)
      allNonZero = false;
  }
  // No byte to the object's end is NUL.
  if (allNonZero && end && limit == *end && start.constant < *end)
    out.unterminated = true;
  return out;
}

core::Term Transfer::measureString(core::Sym pointer, const Expr *argument) {
  StringFacts facts = stringFacts(pointer);
  if (facts.length == StringFacts::Length::Exact)
    return facts.term;
  const core::SymInfo &value = heap.info(state, pointer);
  if (value.type != core::SymInfo::Type::Pointer || value.top ||
      value.targets.size() != 1 || !value.targets[0].offset.known ||
      facts.unterminated)
    return core::Term::unknown();
  const core::Target target = value.targets[0];
  const core::ObjectInfo &info = run.table().info(target.object);
  if (!info.singular || info.key.kind == core::ObjectKind::Literal ||
      !heap.lessEqual(state, core::Term::of(0), target.offset).value_or(false))
    return core::Term::unknown();
  core::ObjectState *object = state.objects.find(target.object) != nullptr
                                  ? &state.objects.at(target.object)
                                  : nullptr;
  if (object == nullptr || object->life != core::Life::Live)
    return core::Term::unknown();
  // A returned `strlen(p)` is the string's length from here on: the NUL
  // lies that far from the pointer, and none before it (RFC 0012 *Length
  // places*).
  core::Sym length = unknownValue(context.getSizeType());
  // No object is larger than PTRDIFF_MAX bytes, so `strlen(s) + 1` does not
  // wrap.
  state.zone.addRange(length, 0, INT64_MAX - 1);
  core::SymInfo &lengthInfo = heap.infoMut(state, length);
  if (argument != nullptr)
    lengthInfo.name = "strlen(" + spell(*argument) + ")";
  auto nul = target.offset.plus(core::Term::ofSym(length));
  if (!nul)
    return core::Term::ofSym(length);
  object = &state.objects.at(target.object);
  object->nulWithin = *nul;
  object->nulFrom = target.offset;
  // The string ends inside an object whose extent is known.
  if (object->extent && object->extent->bytes.known &&
      (object->extent->cls == core::ExtentClass::Exact ||
       object->extent->cls == core::ExtentClass::Declared)) {
    const core::Term &bytes = object->extent->bytes;
    if (bytes.isConstant() && nul->scale == 1)
      state.zone.addLE(length, core::ZeroSym,
                       bytes.constant - 1 - nul->constant);
    else if (bytes.var != core::ZeroSym && bytes.scale == 1 && nul->scale == 1)
      state.zone.addLE(length, bytes.var, bytes.constant - 1 - nul->constant);
  }
  return core::Term::ofSym(length);
}

void Transfer::noteStringWritten(core::Sym pointer, const core::Term &length) {
  const core::SymInfo &value = heap.info(state, pointer);
  if (value.type != core::SymInfo::Type::Pointer || value.top ||
      value.targets.size() != 1 || !length.known)
    return;
  const core::Target target = value.targets[0];
  if (!state.objects.contains(target.object))
    return;
  auto nul = target.offset.plus(length);
  if (!nul || !nul->known)
    return;
  core::ObjectState &object = state.objects.at(target.object);
  // A terminator written past the end of the object (the write was out of
  // bounds, and reported so) is no fact about the object's bytes.
  if (object.extent && object.extent->cls == core::ExtentClass::Exact &&
      heap.lessEqual(state, object.extent->bytes, *nul).value_or(false))
    return;
  object.nulWithin = *nul;
  object.nulFrom = target.offset;
}

void Transfer::stringStored(core::ObjectId id, const core::Term &offset,
                            std::int64_t width, core::Sym value) {
  core::ObjectState *object =
      state.objects.find(id) != nullptr ? &state.objects.at(id) : nullptr;
  if (object == nullptr || !object->nulWithin)
    return;
  const core::Term nul = *object->nulWithin;
  Byte byte = width == 1 ? valueByte(value, width) : Byte::Unknown;
  if (width > 1 && state.zone.constant(value) == 0)
    byte = Byte::Zero;
  // Wholly before the NUL: it stays; a possible NUL is excluded only after
  // the store.
  if (offset.known &&
      heap.lessEqual(state, offset.plusConstant(width), nul).value_or(false)) {
    if (byte != Byte::NonZero && object->nulFrom &&
        !heap.lessEqual(state, offset.plusConstant(width), *object->nulFrom)
             .value_or(false))
      object->nulFrom = offset.plusConstant(width);
    return;
  }
  // Wholly after it: nothing changes.
  if (offset.known &&
      heap.lessEqual(state, nul.plusConstant(1), offset).value_or(false))
    return;
  // Over it: a NUL written exactly there keeps it.
  if (byte == Byte::Zero && width == 1 && offset == nul)
    return;
  object->nulWithin.reset();
  object->nulFrom.reset();
}

//===----------------------------------------------------------------------===//
// Formats (RFC 0012, RFC 0030 §8.2)
//===----------------------------------------------------------------------===//

// NOLINTBEGIN(readability-convert-member-functions-to-static): the other
// engine files call it through the instance.
Transfer::FormatFacts
Transfer::formatFacts(const CallExpr &call,
                      const core::LibraryMatch &match) const {
  // NOLINTEND(readability-convert-member-functions-to-static)
  FormatFacts out;
  const auto &format = match.entry->format;
  if (!format || format->kind != core::LibFormat::Kind::Printf)
    return out;
  int index = match.callArgument(format->format);
  int first = match.callArgument(format->first);
  if (index < 0 || static_cast<unsigned>(index) >= call.getNumArgs())
    return out;
  const auto *text = dyn_cast<StringLiteral>(
      call.getArg(static_cast<unsigned>(index))->IgnoreParenImpCasts());
  if (text == nullptr || text->getCharByteWidth() != 1)
    return out;
  out.literal = true;
  if (format->vaList)
    return out;
  out.passed = first >= 0 && call.getNumArgs() > static_cast<unsigned>(first)
                   ? call.getNumArgs() - static_cast<unsigned>(first)
                   : 0U;
  out.reads = core::formatArgumentCount(text->getString(), format->kind);
  // The least output: every literal byte, one per conversion, and the
  // known length of a `%s` argument; exact when nothing varies.
  llvm::StringRef spec = text->getString();
  unsigned argument =
      first >= 0 ? static_cast<unsigned>(first) : call.getNumArgs();
  std::int64_t lower = 0;
  bool exact = true;
  for (std::size_t i = 0; i < spec.size(); ++i) {
    if (spec[i] != '%') {
      ++lower;
      continue;
    }
    if (++i >= spec.size())
      break;
    if (spec[i] == '%') {
      ++lower;
      continue;
    }
    bool sized = false;
    bool precision = false;
    while (i < spec.size() && llvm::StringRef("-+ #0").contains(spec[i]))
      ++i;
    if (i < spec.size() && spec[i] == '*') {
      ++argument;
      ++i;
      sized = true;
    } else {
      while (i < spec.size() && llvm::isDigit(spec[i])) {
        ++i;
        sized = true;
      }
    }
    if (i < spec.size() && spec[i] == '.') {
      ++i;
      sized = true;
      precision = true;
      if (i < spec.size() && spec[i] == '*') {
        ++argument;
        ++i;
      } else {
        while (i < spec.size() && llvm::isDigit(spec[i]))
          ++i;
      }
    }
    while (i < spec.size() && llvm::StringRef("hljztLq").contains(spec[i]))
      ++i;
    if (i >= spec.size())
      break;
    switch (spec[i]) {
    case 'c':
      ++lower;
      exact = exact && !sized;
      ++argument;
      break;
    case 's': {
      (precision ? out.bounded : out.strings).push_back(argument);
      std::optional<std::int64_t> length;
      if (argument < call.getNumArgs())
        if (const auto *literal = dyn_cast<StringLiteral>(
                call.getArg(argument)->IgnoreParenImpCasts());
            literal != nullptr && literal->getCharByteWidth() == 1)
          length = static_cast<std::int64_t>(
              literal->getBytes()
                  .take_until([](char c) { return c == '\0'; })
                  .size());
      if (length && !sized)
        lower += *length;
      else
        exact = false;
      ++argument;
      break;
    }
    case 'n':
      ++argument;
      break;
    default:
      ++lower;
      exact = false;
      ++argument;
      break;
    }
  }
  out.lower = lower;
  out.exact = exact;
  return out;
}

} // namespace weavec::analysis::engine
