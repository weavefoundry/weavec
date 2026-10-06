//===- EngineSummary.cpp - Format-30 summaries in the object engine -------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0031 §6: a summary is read off the exit states against the entry heap
// (§6.2) and applied at a call by walking the callee's paths through the
// caller's memory (§6.3). Effects keyed on a result class wait on the
// result symbol until a test of it selects the class (RFC 0030 §9.1).
//
//===----------------------------------------------------------------------===//

#include "Engine.h"
#include "weavec/Analysis/ClangLocation.h"

#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

using namespace clang;

namespace weavec::analysis::engine {

//===----------------------------------------------------------------------===//
// Derivation (§6.2)
//===----------------------------------------------------------------------===//

/// The result classes of one exit's returned value.
static std::vector<core::ResultClass> classesOf(const core::Heap &heap,
                                                const core::HeapState &state,
                                                QualType returnType) {
  using core::ResultClass;
  if (state.result == core::ZeroSym || returnType->isVoidType())
    return {};
  const core::SymInfo &value = heap.info(state, state.result);
  if (value.type == core::SymInfo::Type::Pointer) {
    switch (value.null) {
    case core::PointerNull::Null:
      return {ResultClass::Null};
    case core::PointerNull::NonNull:
      return {ResultClass::NonNull};
    case core::PointerNull::Maybe:
      return {ResultClass::Null, ResultClass::NonNull};
    }
  }
  if (value.type == core::SymInfo::Type::Int) {
    std::vector<ResultClass> classes;
    auto lo = state.zone.lower(state.result);
    auto hi = state.zone.upper(state.result);
    if (!lo || *lo < 0)
      classes.push_back(ResultClass::Negative);
    if ((!lo || *lo <= 0) && (!hi || *hi >= 0))
      classes.push_back(ResultClass::Zero);
    if (!hi || *hi > 0)
      classes.push_back(ResultClass::Positive);
    if (hi && *hi < 0)
      std::erase(classes, ResultClass::Zero);
    if (lo && *lo >= 0)
      std::erase(classes, ResultClass::Negative);
    return classes;
  }
  return {};
}

namespace {
/// The effect a path's object got on one exit.
struct ExitEffect {
  core::PathEffect::Kind kind = {};
  bool may = false;
  std::string family;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<std::pair<std::uint32_t, bool>> guard = {};
  /// A release's offset into the object; none when not a constant.
  std::optional<std::int64_t> offset = 0;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<core::ParamPairTest> pairs = {};
};
} // namespace

/// The field name at `offset` of `type`, or `#<offset>`.
static std::string fieldNameAt(const ASTContext &context, QualType type,
                               std::int64_t offset) {
  if (!type.isNull())
    if (const RecordDecl *record = type->getAsRecordDecl();
        record != nullptr && record->isCompleteDefinition()) {
      const ASTRecordLayout &layout = context.getASTRecordLayout(record);
      for (const FieldDecl *field : record->fields())
        if (std::cmp_equal(layout.getFieldOffset(field->getFieldIndex()) /
                               context.getCharWidth(),
                           offset) &&
            !field->getName().empty())
          return field->getNameAsString();
    }
  return "#" + std::to_string(offset);
}

core::SummaryPath cellPath(const ASTContext &context, QualType type,
                           core::SummaryPath path, std::int64_t offset,
                           bool named) {
  type = type.isNull() ? type : type.getCanonicalType();
  bool stepped = false;
  for (int depth = 0; depth < 16 && !type.isNull(); ++depth) {
    const RecordDecl *record = type->getAsRecordDecl();
    if (record == nullptr || !record->isCompleteDefinition())
      break;
    const ASTRecordLayout &layout = context.getASTRecordLayout(record);
    const FieldDecl *found = nullptr;
    std::int64_t at = 0;
    for (const FieldDecl *field : record->fields()) {
      auto start = static_cast<std::int64_t>(
          layout.getFieldOffset(field->getFieldIndex()) /
          context.getCharWidth());
      QualType fieldType = field->getType();
      std::int64_t size =
          fieldType->isIncompleteType()
              ? 0
              : static_cast<std::int64_t>(
                    context.getTypeSizeInChars(fieldType).getQuantity());
      if (offset < start ||
          (offset >= start + size && (size != 0 || offset != start)))
        continue;
      if (found == nullptr || fieldType->isPointerType()) {
        found = field;
        at = start;
      }
      // A union: prefer a pointer member (as the loads do).
      if (!record->isUnion() || fieldType->isPointerType())
        break;
    }
    if (found == nullptr || found->isBitField())
      break;
    // RFC 0031 §7: an anonymous member is spelled by where it starts.
    path = path.field(found->getName().empty() ? "#" + std::to_string(at)
                                               : found->getNameAsString());
    stepped = true;
    offset -= at;
    type = found->getType().getCanonicalType();
    if (offset == 0 && !type->isRecordType())
      return path;
  }
  if (offset == 0 && (stepped || !named))
    return path;
  return path.field("#" + std::to_string(offset));
}

/// Calls `visit(offset, type)` for each scalar member of `type` from byte
/// `base` (a union's members, which share their bytes, once per offset; a
/// member array or bit-field is skipped: its cells are not named here).
template <typename Visit>
static void forEachScalar(const ASTContext &context, QualType type,
                          std::int64_t base, std::set<std::int64_t> &seen,
                          Visit visit, int depth = 0) {
  if (type.isNull() || depth > 8)
    return;
  type = type.getCanonicalType();
  if (type->isPointerType() || type->isIntegralOrEnumerationType()) {
    if (seen.insert(base).second)
      visit(base, type);
    return;
  }
  const RecordDecl *record = type->getAsRecordDecl();
  if (record == nullptr || !record->isCompleteDefinition())
    return;
  const ASTRecordLayout &layout = context.getASTRecordLayout(record);
  for (const FieldDecl *field : record->fields()) {
    if (field->isBitField())
      continue;
    forEachScalar(context, field->getType(),
                  base + static_cast<std::int64_t>(
                             layout.getFieldOffset(field->getFieldIndex()) /
                             context.getCharWidth()),
                  seen, visit, depth + 1);
  }
}

template <typename Describe>
void FunctionRun::describeContents(
    const core::HeapState &state, core::ObjectId object,
    const core::SummaryPath &path,
    std::map<core::SummaryPath, core::ValueDesc> &stores, int depth,
    Describe &describe) {
  if (depth > 2)
    return;
  if (depth == 0)
    visitedFresh = {object};
  const core::ObjectState *found = heap.findObject(state, object);
  if (found == nullptr)
    return;
  const core::ObjectInfo &info = objects.info(object);
  QualType type = info.type != 0 ? typeOfHandle(info.type) : QualType();
  for (const auto &[key, sym] : found->cells) {
    if (!key.isConcrete())
      continue;
    const core::SymInfo &value = heap.info(state, sym);
    core::SummaryPath dest = cellPath(context, type, path, key.offset, true);
    // An unknown value too (and one of another type): the caller's new
    // object would otherwise read its unwritten (zero) value there.
    core::ValueDesc desc = value.type == core::SymInfo::Type::Pointer ||
                                   value.type == core::SymInfo::Type::Int
                               ? describe(state, sym)
                               : core::ValueDesc{};
    bool below = desc.kind == core::ValueDesc::Kind::Fresh && !desc.many &&
                 !desc.offset && !desc.interior && value.targets.size() == 1;
    // A new object whose contents are not described below reads its cells
    // as unknown in the caller, not as zeros.
    if (desc.kind == core::ValueDesc::Kind::Fresh && (!below || depth >= 2))
      desc.zeroed = false;
    stores[dest] = desc;
    if (below && depth < 2 &&
        visitedFresh.insert(value.targets[0].object).second)
      describeContents(state, value.targets[0].object, dest.deref(), stores,
                       depth + 1, describe);
  }
  // Its elements (RFC 0015 §5): what some elements hold, a weak store
  // (`p[*]`, `p[*].f`).
  QualType element = type;
  if (!element.isNull())
    if (const auto *array = context.getAsArrayType(element))
      element = array->getElementType();
  auto put = [&](const core::CellKey &position, core::Sym sym) {
    core::SummaryPath dest = elementsOf(path);
    if (!element.isNull() && element->isRecordType())
      dest = dest.field(fieldNameAt(context, element, position.offset));
    else if (position.offset != 0)
      dest = dest.field("#" + std::to_string(position.offset));
    core::ValueDesc desc = describe(state, sym);
    auto [it, inserted] = stores.emplace(dest, desc);
    if (!inserted && !(it->second == desc))
      it->second = core::ValueDesc{};
  };
  for (const auto &[key, sym] : found->cells)
    if (!key.isConcrete())
      put(key.isSummary() ? key : key.position(), sym);
  for (const core::Segment &segment : found->segments)
    put(segment.position, segment.value);
}

namespace {
/// Finds the parameters a body assigns or takes the address of.
class ParameterWrites : public RecursiveASTVisitor<ParameterWrites> {
public:
  std::set<const VarDecl *> written;
  // RecursiveASTVisitor's CRTP hooks are found by name.
  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool VisitBinaryOperator(BinaryOperator *op) {
    if (op->isAssignmentOp())
      note(op->getLHS());
    return true;
  }
  bool VisitUnaryOperator(UnaryOperator *op) {
    if (op->isIncrementDecrementOp() || op->getOpcode() == UO_AddrOf)
      note(op->getSubExpr());
    return true;
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)

private:
  void note(const Expr *expr) {
    if (const auto *ref = dyn_cast<DeclRefExpr>(expr->IgnoreParenImpCasts()))
      if (const auto *param = dyn_cast<ParmVarDecl>(ref->getDecl()))
        written.insert(param->getCanonicalDecl());
  }
};
} // namespace

std::optional<core::PathTerm>
FunctionRun::parameterTerm(const core::HeapState &state,
                           const core::Term &term) const {
  if (!term.known)
    return std::nullopt;
  auto fits = [](__int128 value) {
    return value >= INT64_MIN && value <= INT64_MAX;
  };
  if (term.isConstant())
    return core::PathTerm{
        .path = std::nullopt, .scale = 1, .constant = term.constant};
  if (auto value = state.zone.constant(term.var)) {
    __int128 folded =
        (static_cast<__int128>(term.scale) * *value) + term.constant;
    if (!fits(folded))
      return std::nullopt;
    return core::PathTerm{.path = std::nullopt,
                          .scale = 1,
                          .constant = static_cast<std::int64_t>(folded)};
  }
  if (!unchangedParams) {
    ParameterWrites writes;
    if (const Stmt *body = function.getBody())
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): Clang's API
      writes.TraverseStmt(const_cast<Stmt *>(body));
    std::vector<bool> unchanged;
    for (const ParmVarDecl *param : function.parameters())
      unchanged.push_back(!writes.written.contains(param->getCanonicalDecl()));
    unchangedParams = std::move(unchanged);
  }
  // A parameter the body never changes still holds its entry value.
  for (unsigned i = 0; i < function.getNumParams(); ++i) {
    const ParmVarDecl *param = function.getParamDecl(i);
    if (!(*unchangedParams)[i] ||
        !param->getType()->isIntegralOrEnumerationType())
      continue;
    auto held = heap.read(state, variableObject(*param), core::CellKey{});
    if (!held)
      continue;
    std::optional<std::int64_t> offset;
    if (*held == term.var) {
      offset = 0;
    } else {
      auto up = state.zone.bound(term.var, *held);
      auto down = state.zone.bound(*held, term.var);
      if (up && down && *down != INT64_MIN && *up == -*down)
        offset = up;
    }
    if (!offset)
      continue;
    __int128 constant = static_cast<__int128>(term.constant) +
                        (static_cast<__int128>(term.scale) * *offset);
    if (!fits(constant))
      return std::nullopt;
    return core::PathTerm{.path = core::SummaryPath::param(i),
                          .scale = term.scale,
                          .constant = static_cast<std::int64_t>(constant)};
  }
  return std::nullopt;
}

template <typename Describe>
void FunctionRun::describeElements(
    const core::HeapState &state, core::ObjectId id,
    const core::ObjectState &contents, const core::SummaryPath &objectPath,
    Describe &describe, std::map<ElementKey, std::string> &releases,
    std::map<ElementKey, core::ValueDesc> &stores) {
  const core::ObjectInfo &info = objects.info(id);
  QualType type = info.type != 0 ? typeOfHandle(info.type) : QualType();
  QualType element = type;
  if (!element.isNull())
    if (const auto *array = context.getAsArrayType(element))
      element = array->getElementType();
  // The path of the element cell at `position`: `p[*]`, or `p[*].f` for a
  // record element.
  // (For a member array, the elements a position's range counts are the
  // member's, `shift` of them further on: a position is kept modulo its
  // stride, so the second field of `sub[i]` may be counted from `sub[i+1]`.)
  std::int64_t shift = 0;
  auto elementPath =
      [&](const core::CellKey &position) -> std::optional<core::SummaryPath> {
    shift = 0;
    // Elements of a member array (`n->b[i]` in one record): the stride is
    // the member's element size, not the record's.
    if (!element.isNull() && element->isRecordType() &&
        !element->isIncompleteType() &&
        static_cast<std::int64_t>(
            context.getTypeSizeInChars(element).getQuantity()) !=
            static_cast<std::int64_t>(position.stride))
      if (const RecordDecl *record = element->getAsRecordDecl();
          record != nullptr && record->isCompleteDefinition() &&
          !objectPath.steps.empty() &&
          objectPath.steps.back().step == core::PathStep::Deref &&
          element == type) {
        const ASTRecordLayout &layout = context.getASTRecordLayout(record);
        for (const FieldDecl *field : record->fields()) {
          const auto *array = context.getAsConstantArrayType(field->getType());
          if (array == nullptr || field->getName().empty())
            continue;
          QualType member = array->getElementType();
          auto start = static_cast<std::int64_t>(
              layout.getFieldOffset(field->getFieldIndex()) /
              context.getCharWidth());
          auto size = static_cast<std::int64_t>(
              context.getTypeSizeInChars(field->getType()).getQuantity());
          auto stride = static_cast<std::int64_t>(
              context.getTypeSizeInChars(member).getQuantity());
          if (std::cmp_not_equal(stride, position.stride) || size <= 0 ||
              stride <= 0)
            continue;
          // The element of the member the position's first byte falls in.
          std::int64_t within = position.offset - start;
          std::int64_t back = within >= 0 ? 0 : (-within + stride - 1) / stride;
          within += back * stride;
          if (within < 0 || within >= stride)
            continue;
          shift = -back;
          core::SummaryPath elements =
              elementsOf(objectPath.field(field->getName()));
          // (A record element: the member of it at that offset.)
          if (member->isRecordType()) {
            std::string name = fieldNameAt(context, member, within);
            if (name.front() == '#')
              return std::nullopt;
            return elements.field(name);
          }
          if (within != 0)
            continue;
          return elements;
        }
        return std::nullopt;
      }
    core::SummaryPath path = elementsOf(objectPath);
    if (!element.isNull() && element->isRecordType()) {
      std::string name = fieldNameAt(context, element, position.offset);
      if (name.front() == '#')
        return std::nullopt;
      return path.field(name);
    }
    if (position.offset != 0)
      return std::nullopt;
    return path;
  };
  // A value some element of this object held at entry.
  auto entryElement = [&](const core::SymInfo &value) {
    if (value.type != core::SymInfo::Type::Pointer || value.top ||
        value.targets.empty())
      return false;
    return std::ranges::all_of(value.targets, [&](const core::Target &target) {
      const core::ObjectInfo &pointee =
          objects.info(objects.liveVersion(target.object));
      return pointee.key.kind == core::ObjectKind::Entry &&
             objectPath.isProperPrefixOf(pointee.key.path) &&
             pointee.key.path.steps.size() <= objectPath.steps.size() + 3;
    });
  };
  auto note = [&](const core::CellKey &position,
                  std::optional<std::pair<core::Term, core::Term>> bounds,
                  core::Sym value) {
    auto path = elementPath(position);
    if (!path)
      return;
    if (bounds && shift != 0) {
      bounds->first = bounds->first.plusConstant(shift);
      bounds->second = bounds->second.plusConstant(shift);
    }
    std::optional<core::ElementRange> range;
    if (bounds) {
      auto from = parameterTerm(state, bounds->first);
      auto to = parameterTerm(state, bounds->second);
      if (from && to)
        range = core::ElementRange{.from = *from, .to = *to};
    }
    const core::SymInfo &held = heap.info(state, value);
    if (entryElement(held)) {
      // The elements' own entry values, released or only read; when the
      // function stored into the object, possibly another element's: some
      // elements may now hold any element's value.
      if (held.release && held.release->definite())
        releases[{path->deref(), range}] = held.release->family;
      // (Or null, where the function joined in a null it stored.)
      else if (contents.stored)
        stores[{*path, std::nullopt}] =
            core::ValueDesc{.kind = core::ValueDesc::Kind::Path,
                            .path = path,
                            .maybeNull = held.nullJoined};
      return;
    }
    // Only a store changes what an element holds.
    if (contents.stored)
      stores[{*path, range}] = describe(state, value);
  };
  for (const core::Segment &segment : contents.segments)
    note(segment.position, std::make_pair(segment.from, segment.to),
         segment.value);
  for (const auto &[key, sym] : contents.cells) {
    if (key.isSummary()) {
      note(key, std::nullopt, sym);
      continue;
    }
    if (!key.isSelected())
      continue;
    core::CellKey position = key.position();
    core::Term index = core::Term::ofSym(
        key.index, 1,
        (key.offset - position.offset) / static_cast<std::int64_t>(key.stride));
    note(position, std::make_pair(index, index.plusConstant(1)), sym);
  }
}

// NOLINTNEXTLINE(readability-function-size): one pass over the exits' views
core::FunctionEffects FunctionRun::deriveEffects() {
  core::FunctionEffects effects;
  if (exits.empty()) {
    effects.returns = core::FunctionEffects::Returns::Never;
    return effects;
  }
  QualType returnType = function.getReturnType();
  // RFC 0030 §9.1: an exit whose result carries pending cases is one exit
  // per class of the result, each with the cases it selects applied.
  // So is one that returns a fixed local a release was guarded by (the
  // release happened where it was non-null: not on the null class).
  auto guardingLocals = [&](const core::HeapState &exit) {
    std::vector<core::Handle> found;
    for (const clang::VarDecl *var : fixedLocals) {
      const core::ObjectState *holder =
          heap.findObject(exit, variableObject(*var));
      const core::Sym *value =
          holder != nullptr ? holder->cells.find(core::CellKey{}) : nullptr;
      if (value == nullptr || *value != exit.result)
        continue;
      for (const auto &[id, object] : exit.objects)
        if (object.record &&
            std::binary_search(object.record->nonNullLocals.begin(),
                               object.record->nonNullLocals.end(),
                               handleOf(var))) {
          found.push_back(handleOf(var));
          break;
        }
    }
    return found;
  };
  std::vector<core::HeapState> split;
  for (const core::HeapState &exit : exits) {
    const core::SymInfo *result =
        exit.result != core::ZeroSym ? exit.syms.find(exit.result) : nullptr;
    const std::vector<core::Handle> guarding =
        result != nullptr && result->type == core::SymInfo::Type::Pointer
            ? guardingLocals(exit)
            : std::vector<core::Handle>{};
    if (result == nullptr ||
        (result->pending.empty() && guarding.empty() && !result->condition)) {
      split.push_back(exit);
      continue;
    }
    std::vector<core::ResultClass> classes = classesOf(heap, exit, returnType);
    for (core::ResultClass c : classes) {
      core::HeapState refined = exit;
      if (!Transfer::selectClass(*this, refined, exit.result, c))
        continue;
      if (c == core::ResultClass::Null) {
        std::vector<core::ObjectId> undone;
        for (const auto &[id, object] : refined.objects)
          if (object.record &&
              std::ranges::any_of(guarding, [&](core::Handle var) {
                return std::ranges::binary_search(object.record->nonNullLocals,
                                                  var);
              }))
            undone.push_back(id);
        for (core::ObjectId id : undone) {
          core::ObjectState &object = refined.objects.at(id);
          object.life = core::Life::Live;
          object.record.reset();
          object.effectReleased = false;
          object.effectMayReleased = false;
        }
      }
      split.push_back(std::move(refined));
    }
  }
  // Per exit: its classes, and the effect per entry path.
  struct ExitView {
    std::vector<core::ResultClass> classes;
    std::map<core::SummaryPath, ExitEffect> byPath;
    std::map<core::SummaryPath, core::ValueDesc> stores;
    /// Byte ranges of entry objects whose bytes the function rewrote with
    /// values it cannot describe (`StoreEffect::bytes`), by object path,
    /// each with whether it may only have (`mayForgotten`).
    std::map<core::SummaryPath,
             std::set<std::tuple<std::int64_t, std::int64_t, bool>>>
        forgotten;
    /// RFC 0013 heap outputs, §6.3: the contents of a new object the
    /// function left in an entry cell, below that cell's dereference, by
    /// the cell (a caller reads them through the new value it stores).
    std::map<core::SummaryPath, core::ValueDesc> contents;
    std::map<core::SummaryPath, core::SummaryPath> contentOf;
    /// RFC 0012 *String facts* of the objects the summary names, by path
    /// and contents prefix (`StringEffect`).
    std::map<std::pair<core::SummaryPath, std::optional<std::uint32_t>>,
             std::pair<core::PathTerm, std::optional<core::PathTerm>>>
        strings;
    /// Cells that hold their entry value or the stored one (a join of a
    /// path that stored and one that did not): a possible store.
    std::set<core::SummaryPath> keepsEntry;
    /// Stores made on exactly the paths of an entry test (the cell's
    /// `storedIff`).
    std::map<core::SummaryPath, core::EntryTest> storeGuards;
    std::optional<core::ValueDesc> result;
    std::map<core::SummaryPath, core::ValueDesc> resultStores;
    /// The exit returns a record whose fields `resultStores` describe.
    bool recordResult = false;
    /// RFC 0015 §5: releases of the elements a range selects, and what a
    /// range of elements (or some elements, without a range) now holds.
    std::map<ElementKey, std::string> elementReleases;
    std::map<ElementKey, core::ValueDesc> elementStores;
  };
  std::vector<ExitView> views;
  auto pathOf = [&](core::ObjectId id) -> std::optional<core::SummaryPath> {
    const core::ObjectInfo &info = objects.info(objects.liveVersion(id));
    if (info.key.kind == core::ObjectKind::Entry ||
        info.key.kind == core::ObjectKind::EntrySummary)
      return info.key.path;
    return std::nullopt;
  };
  // Per exit: the index of each new object described so far.
  std::map<core::ObjectId, std::uint32_t> freshIndex;
  auto describe = [&](const core::HeapState &state,
                      core::Sym sym) -> core::ValueDesc {
    core::ValueDesc desc;
    const core::SymInfo &value = heap.info(state, sym);
    if (value.type == core::SymInfo::Type::Pointer) {
      if (value.null == core::PointerNull::Null) {
        desc.kind = core::ValueDesc::Kind::Null;
        return desc;
      }
      desc.maybeNull = value.null == core::PointerNull::Maybe;
      if (value.targets.size() == 1 && !value.top) {
        const core::Target &target = value.targets[0];
        const core::ObjectInfo &info = objects.info(target.object);
        const core::ObjectState *object = heap.findObject(state, target.object);
        switch (info.key.kind) {
        case core::ObjectKind::HeapRecent:
        case core::ObjectKind::HeapOld:
          // A pointer into it at a constant offset (a cursor after its
          // base, RFC 0011), or somewhere inside it (past a header whose
          // size the path chose, `(char *)block + header`).
          if (object != nullptr && object->owned &&
              object->life == core::Life::Live) {
            desc.kind = core::ValueDesc::Kind::Fresh;
            if (!target.offset.isConstant())
              desc.interior = true;
            else if (target.offset.constant != 0)
              desc.offset = target.offset.constant;
            desc.family = object->family;
            desc.object =
                freshIndex
                    .try_emplace(target.object,
                                 static_cast<std::uint32_t>(freshIndex.size()))
                    .first->second;
            // (Bytes the callee wrote are no longer its zeros.)
            desc.zeroed = object->zeroed && !object->forgetsAny();
            // The extent over the parameters: a constant, or a symbol a
            // parameter held at entry.
            std::optional<std::int64_t> folded;
            if (object->extent && object->extent->bytes.known) {
              const core::Term &bytes = object->extent->bytes;
              if (bytes.isConstant())
                folded = bytes.constant;
              else if (auto c = state.zone.constant(bytes.var))
                folded = (bytes.scale * *c) + bytes.constant;
            }
            if (folded)
              desc.extent = core::PathTerm{
                  .path = std::nullopt, .scale = 1, .constant = *folded};
            // (Read in this state: symbols are numbered per state, and an
            // unchanged integer parameter still holds its entry value.)
            else if (object->extent && object->extent->bytes.known)
              desc.extent = parameterTerm(state, object->extent->bytes);
            return desc;
          }
          break;
        case core::ObjectKind::Entry:
          if (target.offset.isConstant()) {
            desc.kind = core::ValueDesc::Kind::Path;
            // (Null too only where the function joined in a null of its
            // own, `nullJoined`: the entry value's own nullness is the
            // caller's.)
            desc.maybeNull = desc.maybeNull && value.nullJoined;
            // The object's path names the pointee; the value is the pointer
            // that was stored above it.
            core::SummaryPath path = info.key.path;
            if (!path.steps.empty() &&
                path.steps.back().step == core::PathStep::Deref)
              path.steps.popBack();
            desc.path = path;
            desc.offset = target.offset.constant;
            return desc;
          }
          break;
        case core::ObjectKind::Literal:
        case core::ObjectKind::Global:
          desc.kind = core::ValueDesc::Kind::Static;
          return desc;
        default:
          break;
        }
      }
      // Several new objects of one family (the elements a loop filled):
      // each element its own (RFC 0015 §5).
      if (!value.top && value.targets.size() > 1) {
        bool fresh = true;
        std::string family;
        for (const core::Target &target : value.targets) {
          core::ObjectKind kind = objects.info(target.object).key.kind;
          const core::ObjectState *object =
              heap.findObject(state, target.object);
          if ((kind != core::ObjectKind::HeapRecent &&
               kind != core::ObjectKind::HeapOld) ||
              target.offset != core::Term::of(0) || object == nullptr ||
              !object->owned || object->life != core::Life::Live ||
              (!family.empty() && object->family != family)) {
            fresh = false;
            break;
          }
          family = object->family;
        }
        if (fresh) {
          desc.kind = core::ValueDesc::Kind::Fresh;
          desc.family = family;
          desc.many = true;
          // (Into the objects at one constant offset, or at some offset.)
          const core::Term &first = value.targets.front().offset;
          bool same = first.isConstant();
          for (const core::Target &target : value.targets)
            same = same && target.offset == first;
          if (!same)
            desc.interior = true;
          else if (first.constant != 0)
            desc.offset = first.constant;
          desc.object =
              freshIndex
                  .try_emplace(value.targets[0].object,
                               static_cast<std::uint32_t>(freshIndex.size()))
                  .first->second;
          return desc;
        }
      }
      // §5.7: storage of this frame outlives nothing the caller holds.
      bool frame = !value.targets.empty() && !value.top;
      for (const core::Target &target : value.targets) {
        const core::ObjectState *object = heap.findObject(state, target.object);
        frame = frame &&
                (isFrameObject(state, target.object) ||
                 (object != nullptr && (object->life == core::Life::Ended ||
                                        object->life == core::Life::MayEnded)));
      }
      desc.kind = frame ? core::ValueDesc::Kind::Dangling
                        : core::ValueDesc::Kind::Unknown;
      desc.raw = !frame && value.raw;
      desc.rawSome = desc.raw && value.rawSome;
      return desc;
    }
    if (value.type == core::SymInfo::Type::Int) {
      desc.kind = core::ValueDesc::Kind::Int;
      desc.lo = state.zone.lower(sym);
      desc.hi = state.zone.upper(sym);
      // (Unsigned 64-bit values above `INT64_MAX`, which only the interval
      // holds.)
      if (value.values && !value.values->empty() && !value.values->isFull() &&
          !value.values->type.isSigned && value.values->type.width == 64 &&
          value.values->maximum()->bits > static_cast<std::uint64_t>(INT64_MAX))
        desc.range = *value.values;
      return desc;
    }
    // Functions the value is known to be one of, by portable name (§7
    // *Amendment (cross-unit contexts)*: a callback handed out).
    if (value.type == core::SymInfo::Type::Function && value.functionsKnown &&
        (!value.functions.empty() || !value.foreignFunctions.empty())) {
      desc.kind = core::ValueDesc::Kind::Function;
      desc.functions = value.foreignFunctions;
      for (core::Handle handle : value.functions)
        if (const auto *fn = fromHandle<FunctionDecl>(handle))
          desc.functions.push_back(unit.portableName(*fn));
      std::ranges::sort(desc.functions);
      auto repeated = std::ranges::unique(desc.functions);
      desc.functions.erase(repeated.begin(), repeated.end());
      return desc;
    }
    // A function pointer a parameter held at entry (a setter's `h->fn =
    // fn`); any other function value is unknown to the caller, whose calls
    // through the cell then resolve by the slot solution (RFC 0030 §9.3).
    // (Read in this state, symbols being numbered per state: an unmodified
    // parameter still holds its entry value.)
    if (value.type == core::SymInfo::Type::Function)
      for (unsigned i = 0; i < function.getNumParams(); ++i) {
        if (!isUnmodifiedParam(i))
          continue;
        auto held = heap.read(state, variableObject(*function.getParamDecl(i)),
                              core::CellKey{});
        if (held && *held == sym) {
          desc.kind = core::ValueDesc::Kind::Path;
          desc.path = core::SummaryPath::param(i);
          return desc;
        }
      }
    desc.kind = core::ValueDesc::Kind::Unknown;
    return desc;
  };
  // The value an entry cell `dest` holds at an exit, and the new object it
  // points to when it is one. A join of the cell's entry value (a pointer
  // to the start of the entry object below it) and another value is that
  // value, stored on some paths only.
  auto describeCell = [&](const core::HeapState &exit, core::Sym sym,
                          const core::SummaryPath &dest, ExitView &view,
                          core::ObjectId &fresh) -> core::ValueDesc {
    const core::SymInfo &value = heap.info(exit, sym);
    fresh = 0;
    if (value.type != core::SymInfo::Type::Pointer || value.top ||
        value.targets.size() < 2) {
      core::ValueDesc desc = describe(exit, sym);
      if (desc.kind == core::ValueDesc::Kind::Fresh &&
          value.targets.size() == 1)
        fresh = value.targets[0].object;
      return desc;
    }
    core::SummaryPath below = dest.deref();
    core::SymInfo narrowed = value;
    narrowed.targets.clear();
    bool keeps = false;
    for (const core::Target &target : value.targets) {
      const core::ObjectInfo &info = objects.info(target.object);
      if (info.key.kind == core::ObjectKind::Entry && !info.key.dead &&
          info.key.path == below && target.offset == core::Term::of(0)) {
        keeps = true;
        continue;
      }
      narrowed.targets.push_back(target);
    }
    if (!keeps)
      return describe(exit, sym);
    core::HeapState scratch = exit;
    core::Sym other = heap.fresh(scratch, narrowed);
    core::ValueDesc desc = describe(scratch, other);
    if (desc.kind == core::ValueDesc::Kind::Unknown ||
        (desc.kind == core::ValueDesc::Kind::Path && desc.path &&
         *desc.path == dest))
      return describe(exit, sym);
    if (desc.kind == core::ValueDesc::Kind::Fresh &&
        narrowed.targets.size() == 1)
      fresh = narrowed.targets[0].object;
    view.keepsEntry.insert(dest);
    return desc;
  };
  // A new object stored into an entry cell: its contents, kept apart from
  // the stores into entry objects (which name the same paths below the
  // cell's old value).
  auto describeStored = [&](const core::HeapState &exit, ExitView &view,
                            const core::SummaryPath &dest,
                            core::ObjectId object) {
    std::map<core::SummaryPath, core::ValueDesc> contents;
    describeContents(exit, object, dest.deref(), contents, 0, describe);
    for (auto &[path, desc] : contents) {
      view.contents[path] = desc;
      view.contentOf[path] = dest;
    }
  };
  const char *dumpLevel = std::getenv("WEAVEC_ENGINE_DUMP");
  bool dumpExits = dumpLevel != nullptr && std::string_view(dumpLevel) == "2";
  for (const core::HeapState &exit : split) {
    freshIndex.clear();
    if (dumpExits)
      llvm::errs() << "exit of " << function.getNameAsString() << " result s"
                   << exit.result << "\n"
                   << heap.dump(exit);
    ExitView view;
    view.classes = classesOf(heap, exit, returnType);
    if (exit.result != core::ZeroSym)
      view.result = describe(exit, exit.result);
    // Stores into globals (RFC 0005 `global(g)`): what the function left in
    // a global's cells.
    for (const auto &[id, object] : exit.objects) {
      const core::ObjectInfo &info = objects.info(id);
      if (info.key.kind != core::ObjectKind::Global)
        continue;
      const auto *var = fromHandle<VarDecl>(info.key.handle);
      if (var == nullptr)
        continue;
      core::SummaryPath root = core::SummaryPath::global(unit.globalId(*var));
      for (const auto &[key, sym] : object.cells) {
        const core::SymInfo &value = heap.info(exit, sym);
        if (!key.isConcrete())
          continue;
        // A cell still holding its entry value is no store; nor is one of
        // an object no store reached (only read).
        if (!object.stored || value.entryOf == std::make_pair(id, key))
          continue;
        core::SummaryPath dest =
            cellPath(context, var->getType(), root, key.offset, false);
        core::ObjectId fresh = 0;
        core::ValueDesc desc = describeCell(exit, sym, dest, view, fresh);
        // (Unless null was joined in: `if (p) *p = NULL`.)
        if (desc.kind == core::ValueDesc::Kind::Path && desc.path &&
            *desc.path == dest && !desc.maybeNull)
          continue;
        view.stores[dest] = desc;
        for (const auto &[at, test] : object.storedIff)
          if (at == key)
            view.storeGuards[dest] = test;
        if (fresh != 0 && desc.kind == core::ValueDesc::Kind::Fresh &&
            !desc.many && !desc.offset && !desc.interior)
          describeStored(exit, view, dest, fresh);
      }
      // Its elements (RFC 0015 §5).
      describeElements(exit, id, object, root, describe, view.elementReleases,
                       view.elementStores);
    }
    for (const auto &[id, object] : exit.objects) {
      auto path = pathOf(id);
      if (!path)
        continue;
      const core::ObjectInfo &info = objects.info(id);
      bool released =
          object.life == core::Life::Released || object.effectReleased;
      bool mayReleased =
          object.life == core::Life::MayReleased || object.effectMayReleased;
      bool unknown = object.life == core::Life::UnknownReleased;
      std::string family = object.record ? object.record->family : "";
      if (unknown) {
        view.byPath[*path] = ExitEffect{
            .kind = core::PathEffect::Kind::Unknown, .may = true, .family = ""};
      } else if (released || mayReleased) {
        bool moved = object.record && object.record->reason ==
                                          core::ReleaseRecord::Reason::Moved;
        view.byPath[*path] = ExitEffect{
            .kind = moved ? core::PathEffect::Kind::Move
                          : core::PathEffect::Kind::Release,
            .may = !released || !info.singular ||
                   (object.record && object.record->conditional),
            .family = family,
            .guard = object.record
                         ? object.record->paramGuard
                         : std::vector<std::pair<std::uint32_t, bool>>{},
            .offset = object.releaseOffset,
            .pairs = object.record ? object.record->pairGuard
                                   : std::vector<core::ParamPairTest>{}};
      } else if (object.escaped) {
        view.byPath[*path] = ExitEffect{
            .kind = core::PathEffect::Kind::Escape, .may = false, .family = ""};
      }
      // Bytes written without cells to show for them (a `memcpy` into the
      // object, a store at an unknown offset): an unknown value stored at
      // the object's own path, which the caller takes as every byte of the
      // object rewritten (instantiate()).
      // (A dead copy an unknown callee may have released, §4.6, still names
      // the entry object when no live object does: its stores stand.)
      bool dead = info.key.dead;
      if (dead && object.life == core::Life::UnknownReleased &&
          !object.effectReleased) {
        core::ObjectKey live = info.key;
        live.dead = false;
        std::optional<core::ObjectId> twin = objects.lookup(live);
        dead = twin && exit.objects.contains(*twin);
      }
      bool bytesWritten = false;
      if (info.key.kind == core::ObjectKind::Entry && !dead &&
          object.havocked && !info.key.path.steps.empty() &&
          info.key.path.steps.back().step == core::PathStep::Deref) {
        view.stores[info.key.path] = core::ValueDesc{};
        bytesWritten = true;
      }
      // An entry object the function zero-filled (`memset(p, 0, sizeof *p)`):
      // each scalar member it wrote no other value into holds zero.
      if (info.key.kind == core::ObjectKind::Entry && !dead && object.zeroed &&
          !object.havocked && info.type != 0) {
        std::set<std::int64_t> seen;
        forEachScalar(
            context, typeOfHandle(info.type), 0, seen,
            [&](std::int64_t offset, QualType type) {
              if (object.cells.contains(core::CellKey{.offset = offset}))
                return;
              core::ValueDesc zero;
              if (type->isPointerType()) {
                zero.kind = core::ValueDesc::Kind::Null;
              } else {
                zero.kind = core::ValueDesc::Kind::Int;
                zero.lo = 0;
                zero.hi = 0;
              }
              view.stores[cellPath(context, typeOfHandle(info.type),
                                   info.key.path, offset, false)] = zero;
            });
      }
      if (!bytesWritten && info.key.kind == core::ObjectKind::Entry && !dead &&
          (!object.forgotten.empty() || !object.mayForgotten.empty()) &&
          !info.key.path.steps.empty() &&
          info.key.path.steps.back().step == core::PathStep::Deref) {
        auto &ranges = view.forgotten[info.key.path];
        for (const auto &[from, to] : object.forgotten)
          ranges.emplace(from, to, false);
        for (const auto &[from, to] : object.mayForgotten)
          ranges.emplace(from, to, true);
      }
      // Stores into the entry object's cells.
      if (info.key.kind == core::ObjectKind::Entry && !dead)
        for (const auto &[key, sym] : object.cells) {
          const core::SymInfo &value = heap.info(exit, sym);
          // (A function pointer the callee stored is a store too: the
          // caller's indirect calls through the cell must see it. So is a
          // value of no known type, read through a `void *` (a field read
          // by an accessor that takes `void *`): the caller forgets what the
          // cell held.)
          if (!key.isConcrete())
            continue;
          // The callee rewrote the object's bytes: an unknown store at its
          // own path says so (below), not its cells.
          if (bytesWritten && key.offset == 0)
            continue;
          // A cell still holding its entry value is no store; nor is one
          // of an object no store reached (only read).
          if (!object.stored || value.entryOf == std::make_pair(id, key))
            continue;
          QualType type = info.type != 0 ? typeOfHandle(info.type) : QualType();
          core::SummaryPath dest =
              cellPath(context, type, info.key.path, key.offset, false);
          core::ObjectId fresh = 0;
          core::ValueDesc desc = describeCell(exit, sym, dest, view, fresh);
          // A cell still holding what it held at entry is no store (one
          // that may hold a null stored instead is).
          if (desc.kind == core::ValueDesc::Kind::Path && desc.path &&
              *desc.path == dest && !desc.maybeNull)
            continue;
          // An unknown value in a scalar object's only cell: spelled as its
          // cell (`*p.#0`), which an unknown store at the object's own path
          // (every byte rewritten, above) is not.
          if (desc.kind == core::ValueDesc::Kind::Unknown &&
              dest == info.key.path)
            dest = dest.field("#0");
          view.stores[dest] = desc;
          for (const auto &[at, test] : object.storedIff)
            if (at == key)
              view.storeGuards[dest] = test;
          if (fresh != 0 && desc.kind == core::ValueDesc::Kind::Fresh &&
              !desc.many && !desc.offset && !desc.interior)
            describeStored(exit, view, dest, fresh);
        }
      if (info.key.kind == core::ObjectKind::Entry && !info.key.dead)
        describeElements(exit, id, object, info.key.path, describe,
                         view.elementReleases, view.elementStores);
    }
    // A record returned by value: its fields, `result.f` (RFC 0013), when
    // it is storage of this frame whose every cell the exit knows.
    if (returnType->isRecordType())
      if (const core::Sym *storage = exit.exprs.find(handleOf(&function))) {
        const core::SymInfo &pointer = heap.info(exit, *storage);
        if (pointer.type == core::SymInfo::Type::Pointer && !pointer.top &&
            pointer.targets.size() == 1 &&
            pointer.targets[0].offset == core::Term::of(0)) {
          core::ObjectId record = pointer.targets[0].object;
          const core::ObjectState *object = heap.findObject(exit, record);
          const VarDecl *var = localVariable(record);
          if (objects.info(record).key.kind == core::ObjectKind::Local &&
              object != nullptr && !object->forgetsAny() && !object->zeroed &&
              object->segments.empty() &&
              (var == nullptr || !isa<ParmVarDecl>(var))) {
            view.recordResult = true;
            describeContents(exit, record, core::SummaryPath::result(),
                             view.resultStores, 0, describe);
          }
        }
      }
    if (exit.result != core::ZeroSym) {
      // A fresh result's contents: `result*.f` (RFC 0013's heap outputs).
      // Those of one the result points into are not described, so its
      // cells read as unknown in the caller, not as zeros (a string
      // header before where it points).
      if (view.result->kind == core::ValueDesc::Kind::Fresh &&
          !view.result->many && !view.result->offset &&
          !view.result->interior) {
        const core::SymInfo &value = heap.info(exit, exit.result);
        describeContents(exit, value.targets[0].object,
                         core::SummaryPath::result().deref(), view.resultStores,
                         0, describe);
      } else if (view.result->kind == core::ValueDesc::Kind::Fresh) {
        view.result->zeroed = false;
      }
    }
    // RFC 0012 *String facts* (§6.1 `string`): of the new objects the exit
    // describes and of the entry objects, relative to where their pointer
    // points.
    std::map<std::uint32_t, core::ObjectId> byIndex;
    for (const auto &[id, index] : freshIndex)
      byIndex.emplace(index, id);
    auto noteString = [&](core::ObjectId id, const core::SummaryPath &path,
                          std::optional<std::uint32_t> contents,
                          std::int64_t pointerOffset) {
      const core::ObjectState *object = heap.findObject(exit, id);
      if (object == nullptr || !object->nulWithin)
        return;
      auto within =
          parameterTerm(exit, object->nulWithin->plusConstant(-pointerOffset));
      if (!within)
        return;
      std::optional<core::PathTerm> from;
      if (object->nulFrom)
        from =
            parameterTerm(exit, object->nulFrom->plusConstant(-pointerOffset));
      view.strings[{path, contents}] = {*within, from};
    };
    auto freshString = [&](const core::ValueDesc &desc,
                           const core::SummaryPath &path,
                           std::optional<std::uint32_t> contents) {
      if (desc.kind != core::ValueDesc::Kind::Fresh || desc.many)
        return;
      if (auto it = byIndex.find(desc.object); it != byIndex.end())
        noteString(it->second, path, contents, desc.offset.value_or(0));
    };
    if (view.result)
      freshString(*view.result, core::SummaryPath::result().deref(),
                  std::nullopt);
    for (const auto &[dest, desc] : view.resultStores)
      freshString(desc, dest.deref(), std::nullopt);
    for (const auto &[dest, desc] : view.stores)
      freshString(desc, dest.deref(),
                  static_cast<std::uint32_t>(dest.steps.size()));
    for (const auto &[dest, desc] : view.contents)
      freshString(
          desc, dest.deref(),
          static_cast<std::uint32_t>(view.contentOf.at(dest).steps.size()));
    for (const auto &[id, object] : exit.objects) {
      const core::ObjectInfo &info = objects.info(id);
      if (info.key.kind == core::ObjectKind::Entry && !info.key.dead &&
          info.singular)
        noteString(id, info.key.path, std::nullopt, 0);
    }
    views.push_back(std::move(view));
  }
  // Join the exits, per path and kind of effect: one on every exit is
  // `always`; one on some is keyed by the result classes of the exits it is
  // on and, where those do not separate it from the others, by a parameter
  // test; otherwise it is a possible effect.
  std::set<std::pair<core::SummaryPath, core::PathEffect::Kind>> keys;
  for (const ExitView &view : views)
    for (const auto &[path, effect] : view.byPath)
      keys.emplace(path, effect.kind);
  // RFC 0030 §9.1: an unmodified integer parameter whose zero test holds on
  // every exit in `present` and fails on every other exit in `relevant`.
  auto paramCase = [&](const std::vector<bool> &present,
                       const std::vector<bool> &relevant)
      -> std::optional<std::pair<std::uint32_t, bool>> {
    for (unsigned i = 0; i < function.getNumParams(); ++i) {
      if (!unmodifiedParams.contains(i) ||
          (!function.getParamDecl(i)->getType()->isIntegerType() &&
           !function.getParamDecl(i)->getType()->isPointerType()))
        continue;
      core::ObjectId holder = variableObject(*function.getParamDecl(i));
      bool nonZeroWhere = true;
      bool zeroWhere = true;
      for (std::size_t e = 0; e < split.size(); ++e) {
        if (!present[e] && !relevant[e])
          continue;
        auto held = heap.read(split[e], holder, core::CellKey{});
        if (!held) {
          nonZeroWhere = zeroWhere = false;
          break;
        }
        std::optional<bool> zero = isZeroValue(heap, split[e], *held);
        bool isZero = zero == true;
        bool nonZero = zero == false;
        if (present[e]) {
          nonZeroWhere = nonZeroWhere && nonZero;
          zeroWhere = zeroWhere && isZero;
        } else {
          nonZeroWhere = nonZeroWhere && isZero;
          zeroWhere = zeroWhere && nonZero;
        }
      }
      if (nonZeroWhere)
        return std::make_pair(i, false);
      if (zeroWhere)
        return std::make_pair(i, true);
    }
    return std::nullopt;
  };
  // RFC 0014: a comparison of two unmodified pointer parameters that holds
  // on every exit in `present` and fails on every other exit in `relevant`.
  auto pairCase = [&](const std::vector<bool> &present,
                      const std::vector<bool> &relevant)
      -> std::optional<core::ParamPairTest> {
    std::vector<std::pair<std::uint32_t, core::ObjectId>> pointers;
    for (unsigned i = 0; i < function.getNumParams(); ++i)
      if (unmodifiedParams.contains(i) &&
          function.getParamDecl(i)->getType()->isPointerType())
        pointers.emplace_back(i, variableObject(*function.getParamDecl(i)));
    for (std::size_t a = 0; a < pointers.size(); ++a)
      for (std::size_t b = a + 1; b < pointers.size(); ++b)
        for (bool equal : {true, false}) {
          bool holds = true;
          for (std::size_t e = 0; e < split.size() && holds; ++e) {
            if (!present[e] && !relevant[e])
              continue;
            auto x = heap.read(split[e], pointers[a].second, core::CellKey{});
            auto y = heap.read(split[e], pointers[b].second, core::CellKey{});
            std::optional<bool> same =
                x && y ? core::Heap::pointersEqual(split[e], *x, *y)
                       : std::nullopt;
            holds = same && *same == (present[e] ? equal : !equal);
          }
          if (holds)
            return core::ParamPairTest{.first = pointers[a].first,
                                       .second = pointers[b].first,
                                       .equal = equal};
        }
    return std::nullopt;
  };
  // The path over the interface of the cell an entry test is of.
  auto entryCellPath =
      [&](const core::EntryTest &test) -> std::optional<core::SummaryPath> {
    if (!test.key.isConcrete())
      return std::nullopt;
    const core::ObjectInfo &info = objects.info(test.object);
    QualType type = info.type != 0 ? typeOfHandle(info.type) : QualType();
    if (info.key.kind == core::ObjectKind::Entry && !info.key.dead)
      return cellPath(context, type, info.key.path, test.key.offset, false);
    if (info.key.kind == core::ObjectKind::Global)
      if (const auto *var = fromHandle<VarDecl>(info.key.handle))
        return cellPath(context, var->getType(),
                        core::SummaryPath::global(unit.globalId(*var)),
                        test.key.offset, false);
    return std::nullopt;
  };
  // A zero test of a value a cell held at entry that holds on every exit in
  // `present` and fails on every other exit in `relevant`: a lazy
  // initialisation (`if (!g) g = malloc(…)`), keyed by the entry path.
  auto entryCase = [&](const std::vector<bool> &present,
                       const std::vector<bool> &relevant)
      -> std::optional<std::pair<core::SummaryPath, bool>> {
    std::optional<std::size_t> first;
    for (std::size_t e = 0; e < split.size() && !first; ++e)
      if (present[e])
        first = e;
    if (!first)
      return std::nullopt;
    for (const core::EntryTest &candidate : split[*first].entryTests) {
      bool holds = true;
      for (std::size_t e = 0; e < split.size() && holds; ++e) {
        if (!present[e] && !relevant[e])
          continue;
        const auto &tests = split[e].entryTests;
        auto it = std::ranges::find_if(tests, [&](const core::EntryTest &test) {
          return test.object == candidate.object && test.key == candidate.key;
        });
        holds = it != tests.end() &&
                it->zero == (present[e] ? candidate.zero : !candidate.zero);
      }
      if (!holds)
        continue;
      if (auto path = entryCellPath(candidate))
        return std::make_pair(*path, candidate.zero);
    }
    return std::nullopt;
  };
  std::set<core::ResultClass> allClasses;
  for (const ExitView &view : views)
    allClasses.insert(view.classes.begin(), view.classes.end());
  for (const auto &[path, kind] : keys) {
    std::set<core::ResultClass> with;
    std::vector<bool> present;
    bool onAll = true;
    bool anyMay = false;
    // (Exits that released the value at different offsets: at one the
    // caller cannot tell.)
    bool anyOffset = false;
    bool mixedFamily = false;
    const ExitEffect *first = nullptr;
    // RFC 0014: the pointer parameter comparisons every such exit's
    // release was made under.
    std::vector<core::ParamPairTest> pairs;
    for (const ExitView &view : views) {
      auto it = view.byPath.find(path);
      bool here = it != view.byPath.end() && it->second.kind == kind;
      present.push_back(here);
      if (!here) {
        onAll = false;
        continue;
      }
      if (first == nullptr)
        pairs = it->second.pairs;
      else
        std::erase_if(pairs, [&](const core::ParamPairTest &test) {
          return std::find(it->second.pairs.begin(), it->second.pairs.end(),
                           test) == it->second.pairs.end();
        });
      if (first == nullptr)
        first = &it->second;
      // RFC 0034 §6.3: a family two exits disagree on is unknown.
      if (first->family != it->second.family)
        mixedFamily = true;
      anyOffset =
          anyOffset || !it->second.offset || it->second.offset != first->offset;
      anyMay = anyMay || it->second.may;
      with.insert(view.classes.begin(), view.classes.end());
    }
    core::PathEffect effect;
    effect.kind = kind;
    effect.path = path;
    effect.family = mixedFamily ? std::string() : first->family;
    effect.anyOffset = anyOffset;
    effect.offset = anyOffset ? 0 : *first->offset;
    effect.may = anyMay;
    // Made only where the comparison held: an activation where it fails
    // makes none.
    if (!pairs.empty())
      effect.when.paramsEqual = pairs.front();
    if (onAll) {
      // A possible release made under a parameter test is keyed by it
      // (lossy: other conditions may have been dropped).
      if (anyMay && !first->guard.empty()) {
        effect.when.paramZero = first->guard.front();
        effect.lossy = true;
      }
      effects.effects.push_back(effect);
      continue;
    }
    // The exits without the effect whose classes the classes of `with` do
    // not rule out.
    std::vector<bool> relevant;
    bool separated = !with.empty();
    for (std::size_t e = 0; e < views.size(); ++e) {
      bool overlaps = false;
      if (!present[e])
        for (core::ResultClass c : views[e].classes)
          overlaps = overlaps || with.contains(c);
      relevant.push_back(overlaps);
      separated = separated && !overlaps;
    }
    // Without result classes (a `void` function) no class tells the exits
    // apart: a parameter test must, against every exit without the effect.
    if (with.empty())
      for (std::size_t e = 0; e < views.size(); ++e)
        relevant[e] = !present[e];
    bool narrowing = with.size() < allClasses.size();
    if (separated) {
      effect.when.classes.assign(with.begin(), with.end());
    } else if (auto keyed = paramCase(present, relevant)) {
      effect.when.paramZero = keyed;
      if (narrowing)
        effect.when.classes.assign(with.begin(), with.end());
    } else if (auto pair = pairCase(present, relevant)) {
      effect.when.paramsEqual = pair;
      if (narrowing)
        effect.when.classes.assign(with.begin(), with.end());
    } else if (auto entry = entryCase(present, relevant)) {
      effect.when.entryZero = std::move(entry);
      if (narrowing)
        effect.when.classes.assign(with.begin(), with.end());
    } else {
      // Possible, and only on the classes of the exits that have it.
      effect.may = true;
      if (narrowing)
        effect.when.classes.assign(with.begin(), with.end());
    }
    effects.effects.push_back(effect);
  }
  // Two exits' values of one cell: the same value, or a new object on some
  // exits and null on the others (the cell holds either).
  auto joinValue = [](std::optional<core::ValueDesc> &into,
                      const core::ValueDesc &desc, bool &same) {
    if (!into) {
      into = desc;
      return;
    }
    core::ValueDesc a = *into;
    core::ValueDesc b = desc;
    bool maybeNull = a.maybeNull || b.maybeNull;
    a.maybeNull = b.maybeNull = false;
    if (a == b) {
      into->maybeNull = maybeNull;
      return;
    }
    if (a.kind == core::ValueDesc::Kind::Null &&
        b.kind == core::ValueDesc::Kind::Fresh) {
      into = desc;
      into->maybeNull = true;
      return;
    }
    if (a.kind == core::ValueDesc::Kind::Fresh &&
        b.kind == core::ValueDesc::Kind::Null) {
      into->maybeNull = true;
      return;
    }
    // Integers: the hull of the two.
    if (a.kind == core::ValueDesc::Kind::Int &&
        b.kind == core::ValueDesc::Kind::Int && !a.range && !b.range &&
        !a.path && !b.path) {
      into->lo =
          a.lo && b.lo ? std::optional(std::min(*a.lo, *b.lo)) : std::nullopt;
      into->hi =
          a.hi && b.hi ? std::optional(std::max(*a.hi, *b.hi)) : std::nullopt;
      into->maybeNull = maybeNull;
      return;
    }
    same = false;
  };
  // Stores: on every exit, keyed by the result classes of the exits they
  // are on when those separate them from the others, or possible.
  std::set<core::SummaryPath> stored;
  for (const ExitView &view : views)
    for (const auto &[path, desc] : view.stores)
      stored.insert(path);
  for (const core::SummaryPath &path : stored) {
    bool onAll = true;
    std::optional<core::ValueDesc> value;
    bool same = true;
    std::set<core::ResultClass> with;
    for (const ExitView &view : views) {
      auto it = view.stores.find(path);
      if (it == view.stores.end() || view.keepsEntry.contains(path))
        onAll = false;
      if (it == view.stores.end())
        continue;
      with.insert(view.classes.begin(), view.classes.end());
      joinValue(value, it->second, same);
    }
    bool separated = !onAll && !with.empty();
    std::vector<bool> present;
    std::vector<bool> relevant;
    // (A cell that may still hold its entry value is a possible store.)
    bool keeps = false;
    for (const ExitView &view : views) {
      bool here = view.stores.contains(path);
      present.push_back(here);
      relevant.push_back(!here);
      if (!here)
        for (core::ResultClass c : view.classes)
          separated = separated && !with.contains(c);
      keeps = keeps || view.keepsEntry.contains(path);
    }
    separated = separated && !keeps;
    core::StoreEffect store;
    store.dest = path;
    store.value = same && value ? *value : core::ValueDesc{};
    // A new object on some classes and null on the others (its allocation
    // failed there): the object was never made on those (§6.3).
    if (onAll && same && value && value->kind == core::ValueDesc::Kind::Fresh) {
      std::set<core::ResultClass> nullClasses;
      std::set<core::ResultClass> freshClasses;
      for (const ExitView &view : views) {
        const core::ValueDesc &each = view.stores.at(path);
        (each.kind == core::ValueDesc::Kind::Null ? nullClasses : freshClasses)
            .insert(view.classes.begin(), view.classes.end());
      }
      bool disjoint =
          std::ranges::none_of(nullClasses, [&](core::ResultClass c) {
            return freshClasses.contains(c);
          });
      if (!nullClasses.empty() && !freshClasses.empty() && disjoint)
        store.absentOn.assign(nullClasses.begin(), nullClasses.end());
    }
    // Made on exactly the paths of one entry test, on every exit: keyed by
    // it (the exits without the store took the opposite test).
    std::optional<core::EntryTest> iff;
    bool consistent = true;
    for (std::size_t e = 0; e < views.size() && consistent; ++e) {
      const ExitView &view = views[e];
      if (view.stores.contains(path)) {
        auto guard = view.storeGuards.find(path);
        consistent =
            guard != view.storeGuards.end() && (!iff || *iff == guard->second);
        if (consistent)
          iff = guard->second;
      } else if (iff) {
        core::EntryTest opposite = *iff;
        opposite.zero = !opposite.zero;
        consistent = std::binary_search(split[e].entryTests.begin(),
                                        split[e].entryTests.end(), opposite);
      }
    }
    // (Exits before the first store's: checked against the guard found.)
    for (std::size_t e = 0; e < views.size() && consistent && iff; ++e)
      if (!views[e].stores.contains(path)) {
        core::EntryTest opposite = *iff;
        opposite.zero = !opposite.zero;
        consistent = std::binary_search(split[e].entryTests.begin(),
                                        split[e].entryTests.end(), opposite);
      }
    std::optional<core::SummaryPath> iffPath =
        consistent && iff ? entryCellPath(*iff) : std::nullopt;
    if (iffPath)
      store.when.entryZero = std::make_pair(*iffPath, iff->zero);
    else if (separated)
      store.when.classes.assign(with.begin(), with.end());
    else if (!onAll && same && !keeps)
      // RFC 0030 §9.1: a store an unmodified parameter's test selects, or
      // the test of a value at entry.
      if (auto keyed = paramCase(present, relevant))
        store.when.paramZero = keyed;
      else if (auto entry = entryCase(present, relevant))
        store.when.entryZero = std::move(entry);
      else
        store.may = true;
    else
      store.may = !onAll;
    effects.stores.push_back(store);
  }
  // Bytes rewritten with unknown values: every range some exit has, on all
  // of them when each exit has it.
  std::set<core::SummaryPath> rewritten;
  for (const ExitView &view : views)
    for (const auto &[path, ranges] : view.forgotten)
      rewritten.insert(path);
  for (const core::SummaryPath &path : rewritten) {
    std::set<std::pair<std::int64_t, std::int64_t>> ranges;
    for (const ExitView &view : views)
      if (auto it = view.forgotten.find(path); it != view.forgotten.end())
        for (const auto &[from, to, weak] : it->second)
          ranges.emplace(from, to);
    for (const auto &range : ranges) {
      core::StoreEffect store;
      store.dest = path;
      store.bytes = range;
      // (Definite when every exit rewrote them on every path.)
      store.may = !std::ranges::all_of(views, [&](const ExitView &view) {
        auto it = view.forgotten.find(path);
        return it != view.forgotten.end() &&
               it->second.contains(
                   std::make_tuple(range.first, range.second, false));
      });
      effects.stores.push_back(store);
    }
  }
  // The contents of the new objects left in entry cells, over the exits
  // that leave one there: the caller writes them into that object.
  std::set<std::pair<core::SummaryPath, core::SummaryPath>> contentPaths;
  for (const ExitView &view : views)
    for (const auto &[path, container] : view.contentOf)
      contentPaths.emplace(path, container);
  for (const auto &[path, container] : contentPaths) {
    bool onAll = true;
    std::optional<core::ValueDesc> value;
    bool same = true;
    for (const ExitView &view : views) {
      auto holder = view.stores.find(container);
      if (holder == view.stores.end() ||
          holder->second.kind != core::ValueDesc::Kind::Fresh)
        continue;
      auto it = view.contents.find(path);
      if (it == view.contents.end() || view.contentOf.at(path) != container) {
        onAll = false;
        continue;
      }
      joinValue(value, it->second, same);
    }
    core::StoreEffect store;
    store.dest = path;
    store.value = same && value ? *value : core::ValueDesc{};
    store.may = !onAll;
    store.contents = static_cast<std::uint32_t>(container.steps.size());
    effects.stores.push_back(store);
  }
  // String facts, on every exit where their object exists.
  std::set<std::pair<core::SummaryPath, std::optional<std::uint32_t>>>
      stringKeys;
  for (const ExitView &view : views)
    for (const auto &[key, fact] : view.strings)
      stringKeys.insert(key);
  for (const auto &key : stringKeys) {
    const auto &[path, contents] = key;
    auto relevant = [&](const ExitView &view) {
      if (path.isResult())
        return view.result && view.result->kind == core::ValueDesc::Kind::Fresh;
      if (contents) {
        core::SummaryPath container = path;
        container.steps.truncate(*contents);
        auto it = view.stores.find(container);
        return it != view.stores.end() &&
               it->second.kind == core::ValueDesc::Kind::Fresh;
      }
      return true;
    };
    std::optional<std::pair<core::PathTerm, std::optional<core::PathTerm>>>
        fact;
    bool holds = true;
    for (const ExitView &view : views) {
      if (!relevant(view))
        continue;
      auto it = view.strings.find(key);
      if (it == view.strings.end() || (fact && !(*fact == it->second))) {
        holds = false;
        break;
      }
      fact = it->second;
    }
    if (holds && fact)
      effects.strings.push_back(core::StringEffect{.path = path,
                                                   .contents = contents,
                                                   .nulWithin = fact->first,
                                                   .nulFrom = fact->second});
  }
  // Element ranges (RFC 0015 §5): on every exit, or possible.
  std::set<ElementKey> released;
  std::set<ElementKey> written;
  for (const ExitView &view : views) {
    for (const auto &[key, family] : view.elementReleases)
      released.insert(key);
    for (const auto &[key, desc] : view.elementStores)
      written.insert(key);
  }
  for (const ElementKey &key : released) {
    bool onAll = true;
    std::string family;
    for (const ExitView &view : views) {
      auto it = view.elementReleases.find(key);
      if (it == view.elementReleases.end())
        onAll = false;
      else
        family = it->second;
    }
    core::PathEffect effect;
    effect.kind = core::PathEffect::Kind::Release;
    effect.path = key.first;
    effect.family = family;
    effect.elements = key.second;
    effect.may = !onAll || !key.second;
    effects.effects.push_back(effect);
  }
  for (const ElementKey &key : written) {
    bool onAll = true;
    std::optional<core::ValueDesc> value;
    bool same = true;
    for (const ExitView &view : views) {
      auto it = view.elementStores.find(key);
      if (it == view.elementStores.end()) {
        onAll = false;
        continue;
      }
      if (!value)
        value = it->second;
      else if (!(*value == it->second))
        same = false;
    }
    core::StoreEffect store;
    store.dest = key.first;
    store.value = same && value ? *value : core::ValueDesc{};
    store.may = !onAll || !key.second;
    store.elements = key.second;
    effects.stores.push_back(store);
  }
  // A fresh result's contents, over the exits that return it.
  std::set<core::SummaryPath> resultPaths;
  std::size_t freshExits = 0;
  // A record result is described on every exit or not at all (a caller
  // reads the fields no store names as never written).
  bool records = returnType->isRecordType() &&
                 std::ranges::all_of(views, [](const ExitView &view) {
                   return view.recordResult;
                 });
  auto describesResult = [&](const ExitView &view) {
    return (records && view.recordResult) ||
           (view.result && view.result->kind == core::ValueDesc::Kind::Fresh);
  };
  for (const ExitView &view : views)
    if (describesResult(view)) {
      ++freshExits;
      for (const auto &[path, desc] : view.resultStores)
        resultPaths.insert(path);
    }
  for (const core::SummaryPath &path : resultPaths) {
    std::size_t count = 0;
    std::optional<core::ValueDesc> value;
    bool same = true;
    for (const ExitView &view : views) {
      if (!describesResult(view))
        continue;
      auto it = view.resultStores.find(path);
      if (it == view.resultStores.end())
        continue;
      ++count;
      if (!value)
        value = it->second;
      else if (!(*value == it->second))
        same = false;
    }
    core::StoreEffect store;
    store.dest = path;
    store.value = same && value ? *value : core::ValueDesc{};
    store.may = count != freshExits;
    effects.stores.push_back(store);
  }
  // Results: one alternative per distinct description, with a parameter
  // test every exit returning it passes.
  std::map<core::ValueDesc, std::set<core::ResultClass>> results;
  for (const ExitView &view : views)
    if (view.result)
      results[*view.result].insert(view.classes.begin(), view.classes.end());
  const std::vector<bool> none(views.size(), false);
  for (const auto &[desc, classes] : results) {
    std::vector<bool> present;
    bool onAll = true;
    for (const ExitView &view : views) {
      present.push_back(view.result && *view.result == desc);
      onAll = onAll && present.back();
    }
    effects.results.push_back(core::ResultEffect{
        .value = desc,
        .classes = {classes.begin(), classes.end()},
        .paramZero = onAll ? std::nullopt : paramCase(present, none)});
  }
  // RFC 0030 §9.2: a class on which a pointer parameter is always non-null.
  for (unsigned i = 0; i < function.getNumParams(); ++i) {
    if (!function.getParamDecl(i)->getType()->isPointerType())
      continue;
    core::ObjectId holder = variableObject(*function.getParamDecl(i));
    std::set<core::ResultClass> maybeNull;
    std::set<core::ResultClass> all;
    for (std::size_t e = 0; e < split.size(); ++e) {
      const core::HeapState &exit = split[e];
      all.insert(views[e].classes.begin(), views[e].classes.end());
      auto held = heap.read(exit, holder, core::CellKey{});
      // The parameter's entry value, if the variable still holds it.
      if (!held || heap.info(exit, *held).null != core::PointerNull::NonNull)
        maybeNull.insert(views[e].classes.begin(), views[e].classes.end());
    }
    for (core::ResultClass c : all)
      if (!maybeNull.contains(c))
        effects.nonNullOn[c].push_back(core::SummaryPath::param(i));
  }
  // §6.6: the entry objects the function read cells of or stored into (a
  // release alone, or handing the pointer on, is neither).
  std::set<core::SummaryPath> read;
  std::set<core::SummaryPath> wrote;
  for (const core::HeapState &exit : split)
    for (const auto &[id, object] : exit.objects) {
      const core::ObjectInfo &info = objects.info(id);
      if (info.key.kind != core::ObjectKind::Entry &&
          info.key.kind != core::ObjectKind::EntrySummary)
        continue;
      // (A cell still holding its entry value was loaded; a stored one
      // counts as written.)
      bool loaded = !object.segments.empty();
      for (const auto &[key, sym] : object.cells)
        if (const core::SymInfo *value = exit.syms.find(sym);
            value != nullptr && value->entryOf == std::make_pair(id, key))
          loaded = true;
      if (loaded)
        read.insert(info.key.path);
      if (object.stored || object.forgetsAny())
        wrote.insert(info.key.path);
    }
  effects.reads.assign(read.begin(), read.end());
  effects.writes.assign(wrote.begin(), wrote.end());
  effects.unknownGlobals = ranUnknownCode;
  return effects;
}

//===----------------------------------------------------------------------===//
// Instantiation (§6.3)
//===----------------------------------------------------------------------===//

/// The byte offset of the field named `name` in `type`, or none.
static std::optional<std::int64_t>
fieldOffset(const ASTContext &context, QualType type, llvm::StringRef name) {
  if (type.isNull())
    return std::nullopt;
  if (name.starts_with("#")) {
    std::int64_t value = 0;
    if (name.drop_front().getAsInteger(10, value))
      return std::nullopt;
    return value;
  }
  const RecordDecl *record = type->getAsRecordDecl();
  if (record == nullptr || !record->isCompleteDefinition())
    return std::nullopt;
  const ASTRecordLayout &layout = context.getASTRecordLayout(record);
  for (const FieldDecl *field : record->fields())
    if (field->getName() == name)
      return static_cast<std::int64_t>(
          layout.getFieldOffset(field->getFieldIndex()) /
          context.getCharWidth());
  return std::nullopt;
}

/// The type of the member a field step names: the record's field by name
/// or, for `#<offset>`, the field starting there; a step by offset into a
/// non-record keeps the element type.
static QualType fieldStepType(const ASTContext &context, QualType type,
                              llvm::StringRef name, std::int64_t offset) {
  const RecordDecl *record = type.isNull() ? nullptr : type->getAsRecordDecl();
  if (record == nullptr || !record->isCompleteDefinition()) {
    if (!name.starts_with("#") || type.isNull())
      return {};
    // A byte offset into an array: its element's scalar there. Inside a
    // scalar (the high word of a `double` a union also holds as two
    // integers), what the callee wrote there has no type here; past its end
    // it is another element of the same type.
    if (context.getAsArrayType(type) == nullptr && !type->isIncompleteType() &&
        offset > 0 &&
        offset < static_cast<std::int64_t>(
                     context.getTypeSizeInChars(type).getQuantity()))
      return {};
    QualType leaf = type;
    while (const auto *array = context.getAsArrayType(leaf))
      leaf = array->getElementType();
    if (leaf->isRecordType())
      return {};
    // RFC 0033 §1: an offset that is no element's start is a byte there
    // (`memset(p, 0, 16)` over `int *p` stores bytes `#0`..`#15`); taking
    // it for a whole element would reach past the bytes the callee wrote.
    if (!leaf->isIncompleteType()) {
      const std::int64_t width = context.getTypeSizeInChars(leaf).getQuantity();
      if (width > 1 && offset % width != 0)
        return context.UnsignedCharTy;
    }
    return leaf;
  }
  const ASTRecordLayout &layout = context.getASTRecordLayout(record);
  for (const FieldDecl *field : record->fields()) {
    if (name.starts_with("#")
            ? std::cmp_equal(layout.getFieldOffset(field->getFieldIndex()) /
                                 context.getCharWidth(),
                             offset)
            : field->getName() == name)
      return field->getType();
  }
  return {};
}

namespace {
/// Resolves a callee's paths in the caller.
class PathResolver {
public:
  PathResolver(Transfer &transfer, const CallExpr &call,
               const std::vector<core::Sym> &args)
      : transfer(transfer), heap(transfer.domain()),
        state(transfer.heapState()), call(call), args(args),
        context(transfer.functionRun().ast()) {}

  /// The pointer values at a path that ends in a cell (no trailing deref):
  /// `param(0)` is the argument; `param(0)*.buf` is the value in that
  /// field. None when the path leaves what the caller can follow.
  std::optional<core::Sym> valueAt(const core::SummaryPath &path,
                                   QualType *type = nullptr);
  /// The cells a path names (a path ending in a field).
  std::optional<Address> cellAt(const core::SummaryPath &path, QualType *type);
  /// RFC 0015 §5: the elements a path's last index step selects, as the
  /// cell of element 0 in each target (with the fields after the step), the
  /// element size and the cell's type.
  struct Elements {
    std::vector<core::Target> targets;
    std::int64_t stride = 0;
    QualType cellType;
  };
  std::optional<Elements> elementsAt(const core::SummaryPath &path);
  /// The value of "some element" at a path whose last index step has no
  /// range: every element's value, weakly.
  std::optional<core::Sym> anyElementAt(const core::SummaryPath &path);
  void setResult(core::Sym sym, QualType type) {
    result = sym;
    resultType = type;
  }
  /// A record result's storage in the caller (`result.f` names its cells).
  void setResultStorage(Address storage) { resultStorage = std::move(storage); }
  /// §6.3: the new object `store` puts in its cell. The contents stores
  /// below that cell (and a new result's) are read through it, not through
  /// the cell's old value.
  void setStored(const core::StoreEffect &store, core::Sym sym) {
    stored[{store.dest, store.contents.has_value()}] = sym;
  }
  /// `cellAt` and `elementsAt` for a store's destination: a contents store
  /// (or a new result's) reads its dereferences from the new objects of
  /// `setStored`, and fails where none was stored; any other path, and a
  /// value, is read in the entry state.
  std::optional<Address> destination(const core::StoreEffect &store,
                                     QualType *type) {
    through = &store;
    auto out = cellAt(store.dest, type);
    through = nullptr;
    return out;
  }
  std::optional<Elements> destinationElements(const core::StoreEffect &store) {
    through = &store;
    auto out = elementsAt(store.dest);
    through = nullptr;
    return out;
  }

private:
  /// The pointer a dereference of the cell `prefix` names reads.
  std::optional<core::Sym> loadThrough(const core::SummaryPath &prefix,
                                       const Address &cell, QualType type) {
    if (through != nullptr && (through->contents || through->dest.isResult())) {
      std::size_t from = through->contents.value_or(0);
      if (prefix.steps.size() >= from) {
        // The cell holding the new object itself is a store of the entry
        // heap; the cells below it are its contents.
        bool contents =
            through->contents && prefix.steps.size() > *through->contents;
        auto it = stored.find({prefix, contents});
        if (it == stored.end())
          return std::nullopt;
        return it->second;
      }
    }
    return transfer.load(cell, type, nullptr);
  }
  std::map<std::pair<core::SummaryPath, bool>, core::Sym> stored;
  const core::StoreEffect *through = nullptr;
  std::optional<Address> resultStorage;
  core::Sym result = core::ZeroSym;
  QualType resultType;
  Transfer &transfer;
  core::Heap &heap;
  core::HeapState &state;
  const CallExpr &call;
  const std::vector<core::Sym> &args;
  ASTContext &context;
};
} // namespace

std::optional<Address> PathResolver::cellAt(const core::SummaryPath &path,
                                            QualType *typeOut) {
  QualType type;
  core::Sym value = core::ZeroSym;
  if (path.root == core::SummaryRoot::Param) {
    if (path.index >= args.size() || path.index >= call.getNumArgs())
      return std::nullopt;
    value = args[path.index];
    type = call.getArg(path.index)->IgnoreParenImpCasts()->getType();
    // The path is the callee's: through its parameter's type when that is
    // a typed pointer, and an array argument is a pointer to its elements.
    if (const FunctionDecl *callee = call.getDirectCallee();
        callee != nullptr && path.index < callee->getNumParams()) {
      QualType declared = callee->getParamDecl(path.index)->getType();
      if (declared->isPointerType() &&
          !declared->getPointeeType()->isVoidType())
        type = declared;
    }
    if (type->isArrayType())
      type = context.getArrayDecayedType(type);
  } else if (path.root == core::SummaryRoot::Result && !resultStorage) {
    if (result == core::ZeroSym)
      return std::nullopt;
    value = result;
    type = resultType;
  } else if (path.root == core::SummaryRoot::Result ||
             path.root == core::SummaryRoot::Global) {
    // A global root names the variable's storage itself, and a record
    // result the caller's temporary (RFC 0013 `result.f`).
    Address address;
    if (path.root == core::SummaryRoot::Result) {
      address = *resultStorage;
      type = resultType;
    } else {
      const VarDecl *var =
          transfer.functionRun().unitRun().globalDecl(path.index);
      if (var == nullptr)
        return std::nullopt;
      address.targets = {
          core::Target{.object = transfer.functionRun().variableObject(*var)}};
      heap.ensure(state, address.targets[0].object);
      type = var->getType();
    }
    Address current = address;
    for (std::size_t i = 0; i < path.steps.size(); ++i) {
      const core::PathElem &elem = path.steps[i];
      if (elem.step == core::PathStep::Deref) {
        core::SummaryPath prefix = path;
        prefix.steps.truncate(i);
        std::optional<core::Sym> read = loadThrough(prefix, current, type);
        if (!read)
          return std::nullopt;
        core::Sym loaded = *read;
        const core::SymInfo &info = heap.info(state, loaded);
        if (info.type != core::SymInfo::Type::Pointer || info.top)
          return std::nullopt;
        current.targets = info.targets;
        type = type->isPointerType() ? type->getPointeeType() : QualType();
      } else if (elem.step == core::PathStep::Field) {
        auto offset = fieldOffset(context, type, elem.field);
        if (!offset)
          return std::nullopt;
        for (core::Target &target : current.targets)
          target.offset = target.offset.plusConstant(*offset);
        type = fieldStepType(context, type, elem.field, *offset);
      } else {
        return std::nullopt;
      }
    }
    if (typeOut != nullptr)
      *typeOut = type;
    return current;
  } else {
    return std::nullopt;
  }
  // A parameter root is a value; the first step must be a dereference.
  Address current;
  bool haveAddress = false;
  for (std::size_t i = 0; i < path.steps.size(); ++i) {
    const core::PathElem &elem = path.steps[i];
    if (elem.step == core::PathStep::Deref) {
      if (haveAddress) {
        core::SummaryPath prefix = path;
        prefix.steps.truncate(i);
        std::optional<core::Sym> read = loadThrough(prefix, current, type);
        if (!read)
          return std::nullopt;
        value = *read;
      }
      const core::SymInfo &info = heap.info(state, value);
      if (info.type != core::SymInfo::Type::Pointer || info.top)
        return std::nullopt;
      current.targets = info.targets;
      current.top = false;
      haveAddress = true;
      if (const auto *array =
              !type.isNull() ? context.getAsArrayType(type) : nullptr)
        type = array->getElementType(); // an array argument decays
      else
        type = type->isPointerType() ? type->getPointeeType() : QualType();
    } else if (elem.step == core::PathStep::Field) {
      if (!haveAddress)
        return std::nullopt;
      auto offset = fieldOffset(context, type, elem.field);
      if (!offset)
        return std::nullopt;
      for (core::Target &target : current.targets)
        target.offset = target.offset.plusConstant(*offset);
      type = fieldStepType(context, type, elem.field, *offset);
    } else {
      return std::nullopt;
    }
  }
  if (!haveAddress)
    return std::nullopt;
  if (typeOut != nullptr)
    *typeOut = type;
  return current;
}

std::optional<PathResolver::Elements>
PathResolver::elementsAt(const core::SummaryPath &path) {
  std::size_t step = path.steps.size();
  for (std::size_t i = 0; i < path.steps.size(); ++i)
    if (path.steps[i].step == core::PathStep::Index)
      step = i;
  if (step == path.steps.size())
    return std::nullopt;
  core::SummaryPath prefix = path;
  prefix.steps.truncate(step);
  QualType type;
  auto base = cellAt(prefix, &type);
  if (!base || base->top || type.isNull())
    return std::nullopt;
  QualType element = type;
  if (const auto *array = context.getAsArrayType(type))
    element = array->getElementType();
  auto size = transfer.sizeOf(element);
  if (!size || *size <= 0)
    return std::nullopt;
  std::int64_t within = 0;
  QualType cellType = element;
  for (std::size_t i = step + 1; i < path.steps.size(); ++i) {
    const core::PathElem &elem = path.steps[i];
    if (elem.step != core::PathStep::Field)
      return std::nullopt;
    auto offset = fieldOffset(context, cellType, elem.field);
    if (!offset)
      return std::nullopt;
    within += *offset;
    QualType fieldType;
    if (const RecordDecl *record = cellType->getAsRecordDecl())
      for (const FieldDecl *field : record->fields())
        if (field->getName() == elem.field)
          fieldType = field->getType();
    cellType = fieldType;
    if (cellType.isNull())
      return std::nullopt;
  }
  Elements out;
  out.stride = *size;
  out.cellType = cellType;
  for (core::Target target : base->targets) {
    target.offset = target.offset.plusConstant(within);
    out.targets.push_back(target);
  }
  return out;
}

/// The element position and the index of element 0's cell at byte `offset`
/// among elements of `stride` bytes.
static std::optional<std::pair<core::CellKey, core::Term>>
elementBase(const core::Term &offset, std::int64_t stride) {
  if (!offset.known || stride <= 0 || std::cmp_greater(stride, 1U << 20U))
    return std::nullopt;
  std::int64_t position = ((offset.constant % stride) + stride) % stride;
  core::CellKey key{.offset = position,
                    .stride = static_cast<std::uint32_t>(stride),
                    .index = core::ZeroSym};
  std::int64_t first = (offset.constant - position) / stride;
  if (offset.isConstant())
    return std::make_pair(key, core::Term::of(first));
  if (offset.scale % stride != 0)
    return std::nullopt;
  return std::make_pair(
      key, core::Term::ofSym(offset.var, offset.scale / stride, first));
}

std::optional<core::Sym>
PathResolver::anyElementAt(const core::SummaryPath &path) {
  auto elements = elementsAt(path);
  if (!elements || elements->cellType.isNull())
    return std::nullopt;
  core::SymInfo hint;
  if (elements->cellType->isPointerType())
    hint.type = core::SymInfo::Type::Pointer;
  else if (transfer.integerType(elements->cellType))
    hint.type = core::SymInfo::Type::Int;
  else
    hint.type = core::SymInfo::Type::Unknown;
  hint.ctype = typeHandle(elements->cellType);
  core::Sym value = core::ZeroSym;
  for (const core::Target &target : elements->targets) {
    auto base = elementBase(target.offset, elements->stride);
    core::Sym some = base ? heap.load(state, target.object, base->first, hint)
                          : transfer.unknownValue(elements->cellType);
    value = value == core::ZeroSym ? some : heap.mergeWeak(state, value, some);
  }
  if (value == core::ZeroSym)
    return std::nullopt;
  return value;
}

std::optional<core::Sym> PathResolver::valueAt(const core::SummaryPath &path,
                                               QualType *typeOut) {
  if (path.root == core::SummaryRoot::Param && path.steps.empty()) {
    if (path.index >= args.size())
      return std::nullopt;
    if (typeOut != nullptr && path.index < call.getNumArgs())
      *typeOut = call.getArg(path.index)->IgnoreParenImpCasts()->getType();
    return args[path.index];
  }
  // A path through an index step: some element, weakly.
  for (const core::PathElem &elem : path.steps)
    if (elem.step == core::PathStep::Index) {
      if (auto elements = elementsAt(path); elements && typeOut != nullptr)
        *typeOut = elements->cellType;
      return anyElementAt(path);
    }
  QualType type;
  auto cell = cellAt(path, &type);
  if (!cell)
    return std::nullopt;
  if (typeOut != nullptr)
    *typeOut = type;
  if (type.isNull())
    return std::nullopt;
  return transfer.load(*cell, type, nullptr);
}

/// An integer the callee left, as the caller's `value` of its own type: its
/// range when every value of it is one of the type's; otherwise the callee
/// wrote bytes of another type there (`memcpy` of a `U32` into a byte
/// buffer), which say nothing of the cell's value (RFC 0017).
static void boundWithinType(core::HeapState &state, core::Sym value,
                            const core::ValueDesc &desc) {
  auto lower = state.zone.lower(value);
  auto upper = state.zone.upper(value);
  if ((desc.lo && lower && *desc.lo < *lower) ||
      (desc.hi && upper && *desc.hi > *upper) ||
      (desc.lo && upper && *desc.lo > *upper) ||
      (desc.hi && lower && *desc.hi < *lower))
    return;
  state.zone.addRange(value, desc.lo, desc.hi);
}

/// The bytes one element's cell of `type` spans: its size, or the whole
/// stride when that is not known.
static std::int64_t cellBytes(const ASTContext &context, QualType type,
                              std::int64_t stride) {
  if (type.isNull() || type->isIncompleteType())
    return stride;
  return std::min<std::int64_t>(
      stride, static_cast<std::int64_t>(
                  context.getTypeSizeInChars(type).getQuantity()));
}

void Transfer::storePastObject(const CallExpr &call,
                               const core::StoreEffect &store,
                               const core::Target &target,
                               const core::Term &pointer, __int128 start,
                               __int128 end) {
  // (A range of unknown values says the callee may have written some of
  // its elements; a known value, that it wrote every one: an element it
  // skipped would hold its entry value too.)
  const core::ValueDesc &value = store.value;
  if (value.kind == core::ValueDesc::Kind::Unknown ||
      (value.kind == core::ValueDesc::Kind::Int && !value.range &&
       (!value.lo || !value.hi)))
    return;
  // (Once per call: a requirement RFC 0030 §7.5 checked there says it.)
  if (start >= end || !pointer.isConstant() || !store.dest.isParam() ||
      store.dest.index >= call.getNumArgs() ||
      run.requirementViolated.contains(&call))
    return;
  const core::ObjectState *object = heap.findObject(state, target.object);
  if (object == nullptr || object->life != core::Life::Live ||
      !object->extent || object->extent->cls != core::ExtentClass::Exact ||
      !object->extent->bytes.isConstant())
    return;
  const std::int64_t size = object->extent->bytes.constant;
  if (start >= 0 && end <= size)
    return;
  auto bytes = [](__int128 count) {
    return std::to_string(static_cast<std::int64_t>(count)) +
           (count == 1 ? " byte" : " bytes");
  };
  std::string callee = "the callee";
  if (const FunctionDecl *direct = call.getDirectCallee())
    callee = "'" + direct->getNameAsString() + "'";
  // Measured from the object's start, as its extent is (RFC 0030 §7.5).
  const Expr &passed = *call.getArg(store.dest.index);
  const std::string argument = spell(pointedObject(passed));
  core::Diagnostic diagnostic;
  diagnostic.id = core::diag::OutOfBounds;
  diagnostic.severity = core::Severity::Error;
  diagnostic.message =
      start < 0 ? callee + " requires '" + argument + "' before its start"
                : callee + " requires " + bytes(end) + " behind '" + argument +
                      "', which has " + bytes(size);
  diagnostic.location =
      toCoreLocation(context.getSourceManager(), passed.getBeginLoc());
  const core::ObjectInfo &info = run.table().info(target.object);
  if ((info.key.kind == core::ObjectKind::HeapRecent ||
       info.key.kind == core::ObjectKind::HeapOld) &&
      info.created.isValid())
    diagnostic.addNote("'" + argument + "' is allocated here", info.created);
  if (run.isPublishing())
    run.ledger().decideAs(call, core::SiteKind::Call, core::Boundary::Call,
                          core::Facet::Spatial,
                          core::FacetDecision::violation(diagnostic.message));
  run.requirementViolated.insert(&call);
  run.report(std::move(diagnostic), core::Certainty::Definite, &call,
             core::Facet::Spatial);
}

void Transfer::boundByRange(core::Sym value, QualType type,
                            const core::ValueDesc &desc) {
  auto integer = integerType(type);
  if (!desc.range || !integer || integer->isBoolean)
    return;
  core::IntegerRange range = desc.range->converted(*integer);
  core::SymInfo &info = heap.infoMut(state, value);
  if (info.values && info.values->type == *integer)
    range = range.intersect(*info.values);
  if (!range.empty())
    info.values = range;
}

core::Sym Transfer::functionValue(const core::ValueDesc &desc) {
  // This unit's functions (defined or declared) by their declarations,
  // another's by name.
  core::SymInfo info;
  info.type = core::SymInfo::Type::Function;
  info.functionsKnown = true;
  for (const std::string &name : desc.functions) {
    if (const FunctionDecl *fn = run.unitRun().functionNamed(name))
      info.functions.push_back(handleOf(fn->getCanonicalDecl()));
    else
      info.foreignFunctions.push_back(name);
  }
  return heap.fresh(state, info);
}

bool Transfer::pendingOnNullArgument(const std::vector<core::Sym> &args,
                                     core::Sym value,
                                     const core::SummaryPath &path,
                                     const core::SourceLocation &where) const {
  // The callee reaches the value through the argument, so the argument is
  // not null there: a release still pending on a null result of the call
  // that made the argument (`moved = wrap(owned, 1)`, which frees `owned`
  // only when it returns null) did not happen on this path.
  if (!path.isParam() || path.steps.empty() || path.index >= args.size())
    return false;
  const core::SymInfo &argument = heap.info(state, args[path.index]);
  std::vector<core::ObjectId> targets;
  for (const core::Target &target : heap.info(state, value).targets)
    targets.push_back(target.object);
  for (const core::PendingCase &pending : argument.pending) {
    if (pending.kind != core::PendingCase::Kind::Release ||
        pending.record.where != where ||
        std::ranges::find(pending.classes, "nonnull") != pending.classes.end())
      continue;
    const core::SymInfo *subject = state.syms.find(pending.subject);
    if (subject == nullptr)
      continue;
    for (const core::Target &target : subject->targets)
      if (std::ranges::find(targets, target.object) != targets.end())
        return true;
  }
  return false;
}

bool Transfer::lostView(const CallExpr &call,
                        const std::vector<core::Sym> &args,
                        const core::SummaryPath &valuePath) {
  if (!valuePath.isParam() || valuePath.steps.empty() ||
      valuePath.index >= args.size())
    return false;
  const core::SymInfo &root = heap.info(state, args[valuePath.index]);
  if (root.type != core::SymInfo::Type::Pointer || root.top ||
      root.targets.empty() || root.null == core::PointerNull::Null)
    return false;
  // The unknown-callee default over what the argument reaches (RFC 0030
  // §5.1): any of it may be what the callee released.
  std::vector<core::ObjectId> start;
  start.reserve(root.targets.size());
  for (const core::Target &target : root.targets)
    start.push_back(target.object);
  core::ReleaseRecord record;
  record.reason = core::ReleaseRecord::Reason::UnknownCallee;
  record.where = toCoreLocation(context.getSourceManager(), call.getBeginLoc());
  record.allPaths = false;
  for (core::ObjectId id : heap.reachableObjects(state, start)) {
    if (!state.objects.contains(id))
      continue;
    core::ObjectKind kind = run.table().info(id).key.kind;
    if (kind == core::ObjectKind::Local || kind == core::ObjectKind::Global ||
        kind == core::ObjectKind::Literal || kind == core::ObjectKind::Function)
      continue;
    core::ObjectState &object = state.objects.at(id);
    if (object.life == core::Life::Live) {
      object.life = core::Life::UnknownReleased;
      object.record = record;
    }
  }
  if (run.isPublishing())
    for (core::SiteId id : run.sites().sitesOf(call)) {
      const SiteInfo *info = run.sites().info(id);
      if (info == nullptr || info->kind != core::SiteKind::Call ||
          info->boundary != core::Boundary::Call ||
          !run.applies(id, core::Facet::Temporal))
        continue;
      run.ledger().decideAs(call, info->kind, info->boundary,
                            core::Facet::Temporal,
                            core::FacetDecision::unresolvedFor(
                                core::UnresolvedReason::RawCast,
                                "incompatible or unknown object view at "
                                "call"));
    }
  return true;
}

/// RFC 0034 §6.3: the new objects a summary leaves in several places (its
/// result, cells) on exits a call's arguments do not show to be the same:
/// each exit numbers its own new objects from #0, so one object of the
/// call stands for several (`if (w) *a = f(); else *b = f();`) and is not
/// singular. The result's parameter test only names a test its exits pass;
/// a store's is the exits it is on.
static std::set<std::uint32_t>
scatteredObjects(const core::Heap &heap, const core::HeapState &state,
                 const core::FunctionEffects &effects,
                 const std::vector<core::Sym> &args) {
  using Test = std::optional<std::pair<std::uint32_t, bool>>;
  // The test the call leaves open, or none; false when it excludes.
  auto open = [&](Test &test) {
    if (!test || test->first >= args.size() ||
        args[test->first] == core::ZeroSym)
      return true;
    std::optional<bool> zero = isZeroValue(heap, state, args[test->first]);
    if (zero && *zero != test->second)
      return false;
    if (zero)
      test.reset();
    return true;
  };
  std::set<core::ResultClass> all;
  for (const core::ResultEffect &alternative : effects.results)
    all.insert(alternative.classes.begin(), alternative.classes.end());
  auto classes = [&](std::vector<core::ResultClass> of) {
    std::ranges::sort(of);
    return std::ranges::includes(of, all) ? std::vector<core::ResultClass>{}
                                          : of;
  };
  struct Places {
    std::vector<core::EffectCase> stores;
    bool may = false;
    std::optional<std::vector<core::ResultClass>> result;
    std::vector<Test> resultTests;
  };
  std::map<std::uint32_t, Places> places;
  for (core::ResultEffect alternative : effects.results)
    if (alternative.value.kind == core::ValueDesc::Kind::Fresh &&
        open(alternative.paramZero)) {
      Places &at = places[alternative.value.object];
      std::vector<core::ResultClass> with =
          at.result.value_or(std::vector<core::ResultClass>{});
      with.insert(with.end(), alternative.classes.begin(),
                  alternative.classes.end());
      at.result = classes(with);
      at.resultTests.push_back(alternative.paramZero);
    }
  for (const core::StoreEffect &store : effects.stores) {
    core::EffectCase when = store.when;
    if (store.value.kind != core::ValueDesc::Kind::Fresh || store.contents ||
        store.dest.isResult() || !open(when.paramZero))
      continue;
    when.classes = classes(when.classes);
    Places &at = places[store.value.object];
    at.stores.push_back(when);
    at.may = at.may || store.may;
  }
  std::set<std::uint32_t> several;
  for (const auto &[object, at] : places) {
    if (at.stores.size() + (at.result ? 1 : 0) < 2)
      continue;
    bool apart = at.may || std::ranges::any_of(at.stores, [&](const auto &c) {
                   return !(c == at.stores.front());
                 });
    if (at.result) {
      const core::EffectCase &store = at.stores.front();
      apart = apart || store.paramsEqual || store.entryZero ||
              store.classes != *at.result;
      for (const Test &test : at.resultTests)
        apart = apart || (store.paramZero && test != store.paramZero);
    }
    if (apart)
      several.insert(object);
  }
  return several;
}

/// The value a `path` description names at the call: the value `at` the
/// path holds, `offset` bytes on, or null where the description says the
/// callee may have left null instead (its entry value joined with null).
core::Sym Transfer::pathValue(core::Sym at, const core::ValueDesc &desc,
                              QualType type) {
  core::Sym value = at;
  if (desc.offset && *desc.offset != 0 &&
      heap.info(state, value).type == core::SymInfo::Type::Pointer) {
    core::SymInfo shifted = heap.info(state, value);
    shifted.entryOf.reset();
    shifted.pending.clear();
    for (core::Target &target : shifted.targets)
      target.offset = target.offset.plusConstant(*desc.offset);
    value = heap.fresh(state, shifted);
  }
  // (Also over an uninitialised value, which reads as null too.)
  if (desc.maybeNull && type->isPointerType() &&
      heap.info(state, value).type == core::SymInfo::Type::Pointer)
    value = heap.mergeWeak(state, value, nullPointer(type));
  return value;
}

// NOLINTNEXTLINE(readability-function-size): one pass over the summary
core::Sym Transfer::instantiate(const CallExpr &call,
                                const core::FunctionEffects &effects,
                                const std::vector<core::Sym> &args) {
  PathResolver resolver(*this, call, args);
  // The callee's new objects by their summary index.
  std::map<std::uint32_t, core::ObjectId> created;
  std::map<std::uint32_t, const core::ValueDesc *> madeBy;
  std::set<std::uint32_t> several =
      scatteredObjects(heap, state, effects, args);
  auto freshObject = [&](const core::ValueDesc &desc, QualType pointee,
                         bool owned) -> core::ObjectId {
    if (auto it = created.find(desc.object); it != created.end()) {
      // RFC 0034 §6.3: rows that give one new object different facts are
      // applied together: their join (an unknown family or extent).
      const core::ValueDesc &first = *madeBy.at(desc.object);
      if (state.objects.contains(it->second)) {
        core::ObjectState &made = state.objects.at(it->second);
        if (first.family != desc.family)
          made.family.clear();
        if (first.extent != desc.extent)
          made.extent.reset();
        made.zeroed = made.zeroed && desc.zeroed;
      }
      return it->second;
    }
    madeBy[desc.object] = &desc;
    core::SummaryPath key = core::SummaryPath::result();
    key.index = desc.object;
    core::ObjectId object =
        run.allocationObject(call, pointee, spell(call), key);
    if (desc.many || several.contains(desc.object)) {
      // One object per element (RFC 0015 §5): several runtime objects.
      core::ObjectKey manyKey = run.table().info(object).key;
      manyKey.kind = core::ObjectKind::HeapOld;
      core::ObjectInfo manyInfo = run.table().info(object);
      manyInfo.singular = false;
      object = run.table().intern(manyKey, manyInfo);
    }
    core::ObjectState allocated;
    allocated.family = desc.family;
    allocated.owned = owned;
    allocated.zeroed = desc.zeroed;
    core::Term extent = core::Term::unknown();
    if (desc.extent && !desc.extent->path) {
      extent = core::Term::of(desc.extent->constant);
    } else if (desc.extent && desc.extent->path &&
               desc.extent->path->isParam() &&
               desc.extent->path->steps.empty() &&
               desc.extent->path->index < args.size()) {
      core::Term base = termOf(args[desc.extent->path->index]);
      if (base.known) {
        base.scale *= desc.extent->scale;
        base.constant =
            (base.constant * desc.extent->scale) + desc.extent->constant;
        if (base.isConstant())
          base.scale = 0;
        extent = base;
      }
    }
    if (extent.known)
      allocated.extent =
          core::Extent{.bytes = extent, .cls = core::ExtentClass::Exact};
    state.objects.set(object, allocated);
    created[desc.object] = object;
    return object;
  };
  core::SourceLocation here =
      toCoreLocation(context.getSourceManager(), call.getBeginLoc());
  // RFC 0004: a raw pointer the callee returned or stored stays raw.
  auto rawValue = [&](core::Sym value, const core::ValueDesc &desc) {
    core::SymInfo &info = heap.infoMut(state, value);
    if (desc.raw && info.type == core::SymInfo::Type::Pointer) {
      info.raw = true;
      info.rawSome = desc.rawSome;
      info.rawAt = here;
      info.rawOrigin = core::SymInfo::RawOrigin::Returned;
      info.rawFrom = spell(*call.getCallee());
    }
  };
  // The result, from its alternatives.
  QualType type = call.getType();
  core::Sym result = core::ZeroSym;
  bool anyNull = false;
  bool anyNonNull = false;
  bool fresh = false;
  auto argumentFails =
      [&](const std::optional<std::pair<std::uint32_t, bool>> &test) {
        if (!test || test->first >= args.size())
          return false;
        auto argument = state.zone.constant(args[test->first]);
        return argument && (*argument == 0) != test->second;
      };
  for (const core::ResultEffect &alternative : effects.results) {
    if (argumentFails(alternative.paramZero))
      continue;
    const core::ValueDesc &desc = alternative.value;
    core::Sym value = core::ZeroSym;
    switch (desc.kind) {
    case core::ValueDesc::Kind::Null:
      anyNull = true;
      continue;
    case core::ValueDesc::Kind::Fresh: {
      QualType pointee =
          type->isPointerType() ? type->getPointeeType() : QualType();
      core::ObjectId object = freshObject(desc, pointee, true);
      core::SymInfo info;
      info.type = core::SymInfo::Type::Pointer;
      info.targets = {core::Target{
          .object = object,
          .offset = desc.interior ? core::Term::unknown()
                                  : core::Term::of(desc.offset.value_or(0))}};
      info.null = core::PointerNull::NonNull;
      info.name = spell(call);
      info.ctype = typeHandle(type);
      value = heap.fresh(state, info);
      break;
    }
    case core::ValueDesc::Kind::Path:
      if (desc.path)
        if (auto at = resolver.valueAt(*desc.path))
          value = pathValue(*at, desc, type);
      break;
    case core::ValueDesc::Kind::Int:
      value = unknownValue(type);
      boundWithinType(state, value, desc);
      boundByRange(value, type, desc);
      break;
    case core::ValueDesc::Kind::Dangling:
      if (type->isPointerType())
        value = danglingValue(call, type, core::SummaryPath::result());
      break;
    case core::ValueDesc::Kind::Function:
      value = functionValue(desc);
      break;
    case core::ValueDesc::Kind::Static:
    case core::ValueDesc::Kind::Unknown:
      break;
    }
    if (value == core::ZeroSym) {
      value = unknownValue(type);
      rawValue(value, desc);
    }
    if (heap.info(state, value).type == core::SymInfo::Type::Pointer) {
      if (desc.maybeNull)
        anyNull = true;
      anyNonNull = true;
    }
    fresh = fresh || desc.kind == core::ValueDesc::Kind::Fresh;
    result =
        result == core::ZeroSym ? value : heap.mergeWeak(state, result, value);
  }
  if (result == core::ZeroSym)
    result = anyNull && type->isPointerType() ? nullPointer(type)
                                              : unknownValue(type);
  if (type->isPointerType()) {
    // RFC 0033 §1: a result that is an argument on some classes and null on
    // others is a value of its own. Made maybe-null in place, it would be
    // the argument's value, and a test of the result would refine the
    // argument (`if (add(list, v) == NULL)` taken as `list == NULL`).
    if (anyNull && anyNonNull &&
        heap.info(state, result).type == core::SymInfo::Type::Pointer &&
        heap.info(state, result).null != core::PointerNull::Maybe) {
      core::SymInfo own = heap.info(state, result);
      result = heap.fresh(state, std::move(own));
    }
    core::SymInfo &info = heap.infoMut(state, result);
    if (info.type == core::SymInfo::Type::Pointer) {
      if (anyNull && anyNonNull)
        info.null = core::PointerNull::Maybe;
      info.allocatorSource = fresh && anyNull;
      if (info.allocatorSource)
        info.nullOrigin =
            core::NullOrigin{.reason = core::NullOrigin::Reason::Allocated,
                             .where = here,
                             .detail = spell(*call.getCallee())};
    }
  }
  resolver.setResult(result, type);
  // RFC 0013: a record result's fields (`result.f` stores) go into the
  // caller's temporary, which then holds nothing else.
  if (type->isRecordType()) {
    bool described = false;
    for (const core::StoreEffect &store : effects.stores)
      described =
          described || (store.dest.isResult() && !store.dest.steps.empty() &&
                        store.dest.steps.front().step == core::PathStep::Field);
    core::ObjectId temporary = run.recordResultObject(call);
    if (described && state.objects.contains(temporary)) {
      core::ObjectState &object = state.objects.at(temporary);
      object.cells = {};
      object.havocked = false;
      object.forgotten.clear();
      object.mayForgotten.clear();
      object.uninitialised = true;
      Address storage;
      storage.targets = {core::Target{.object = temporary}};
      resolver.setResultStorage(storage);
    }
  }
  // Effects on the entry heap.
  auto recordFor = [&](const core::PathEffect &effect) {
    core::ReleaseRecord record;
    record.reason = effect.kind == core::PathEffect::Kind::Move
                        ? core::ReleaseRecord::Reason::Moved
                        : core::ReleaseRecord::Reason::Freed;
    record.where = here;
    record.family = effect.family;
    record.conditional = effect.may || !effect.when.classes.empty();
    record.lossy = effect.lossy;
    return record;
  };
  // A callee's index term at the call.
  auto termAtCall = [&](const core::PathTerm &term) -> core::Term {
    if (!term.path)
      return core::Term::of(term.constant);
    std::optional<core::Sym> value = resolver.valueAt(*term.path);
    if (!value)
      return core::Term::unknown();
    core::Term base = termOf(*value);
    if (!base.known)
      return base;
    __int128 scale = static_cast<__int128>(base.scale) * term.scale;
    __int128 constant =
        (static_cast<__int128>(base.constant) * term.scale) + term.constant;
    if (scale < INT64_MIN || scale > INT64_MAX || constant < INT64_MIN ||
        constant > INT64_MAX)
      return core::Term::unknown();
    if (base.isConstant())
      return core::Term::of(static_cast<std::int64_t>(constant));
    return core::Term::ofSym(base.var, static_cast<std::int64_t>(scale),
                             static_cast<std::int64_t>(constant));
  };
  // The elements `[from, to)` of a callee's range at the call, per target:
  // the position and the caller's index bounds, or none (then weakly).
  // (And the elements' position in the object, `sub[*].sp` at 8 of 16,
  // for some elements weakly; none at an unknown offset.)
  struct CallerRange {
    core::ObjectId object;
    std::optional<std::pair<core::CellKey, std::pair<core::Term, core::Term>>>
        range;
    std::optional<core::CellKey> position = std::nullopt;
  };
  auto someAt = [&](const core::Target &target, std::int64_t stride) {
    CallerRange caller{.object = target.object, .range = std::nullopt};
    if (auto base = elementBase(target.offset, stride))
      caller.position = base->first;
    return caller;
  };
  auto rangesAt = [&](const PathResolver::Elements &elements,
                      const core::ElementRange &range) {
    core::Term from = termAtCall(range.from);
    core::Term to = termAtCall(range.to);
    std::vector<CallerRange> out;
    for (const core::Target &target : elements.targets) {
      CallerRange caller = someAt(target, elements.stride);
      if (auto base = elementBase(target.offset, elements.stride)) {
        auto lo = from.plus(base->second);
        auto hi = to.plus(base->second);
        if (lo && hi && lo->known && hi->known)
          caller.range = std::make_pair(base->first, std::make_pair(*lo, *hi));
      }
      out.push_back(caller);
    }
    return out;
  };
  // RFC 0031 §5.4 (a declared contract first), RFC 0003: a parameter the
  // callee's declaration annotates `WEAVEC_BORROWED` or `WEAVEC_MUT` is not
  // consumed, whatever its definition does (that is its own mismatch).
  std::vector<bool> borrowedParams;
  if (const FunctionDecl *callee = call.getDirectCallee()) {
    SignatureAnnotations signature = collectAnnotations(*callee);
    for (const AnnotationSet &set : signature.params)
      borrowedParams.push_back(set.borrowed || set.mutBorrowed);
  }
  // Values a possible release of the summary reached (no result class
  // tells when it happens).
  std::set<core::Sym> possiblyReleased;
  // Effects name the callee's entry state: an unknown effect, which forgets
  // the cells it reaches, comes after every effect that reads a path
  // through them (RFC 0031 §6.3).
  std::vector<core::PathEffect> ordered;
  for (const core::PathEffect &effect : effects.effects)
    if (effect.kind != core::PathEffect::Kind::Unknown)
      ordered.push_back(effect);
  for (const core::PathEffect &effect : effects.effects)
    if (effect.kind == core::PathEffect::Kind::Unknown)
      ordered.push_back(effect);
  // Their paths too name the entry state: each is resolved before any
  // forgets the cells another's path reads (`*p` and `*p->next`).
  std::vector<std::optional<core::Sym>> unknownValues(ordered.size());
  for (std::size_t i = 0; i < ordered.size(); ++i)
    if (ordered[i].kind == core::PathEffect::Kind::Unknown) {
      core::SummaryPath valuePath = ordered[i].path;
      if (!valuePath.steps.empty() &&
          valuePath.steps.back().step == core::PathStep::Deref)
        valuePath.steps.popBack();
      unknownValues[i] = resolver.valueAt(valuePath);
    }
  for (std::size_t ordinal = 0; ordinal < ordered.size(); ++ordinal) {
    const core::PathEffect &effect = ordered[ordinal];
    if ((effect.kind == core::PathEffect::Kind::Release ||
         effect.kind == core::PathEffect::Kind::Move) &&
        effect.path.isParam() && effect.path.index < borrowedParams.size() &&
        borrowedParams[effect.path.index] && effect.path.steps.size() == 1)
      continue;
    // The value whose object the path names: the path without its final
    // dereference.
    core::SummaryPath valuePath = effect.path;
    if (!valuePath.steps.empty() &&
        valuePath.steps.back().step == core::PathStep::Deref)
      valuePath.steps.popBack();
    if (effect.elements && (effect.kind == core::PathEffect::Kind::Release ||
                            effect.kind == core::PathEffect::Kind::Move)) {
      // RFC 0015 §5: every element of the range released.
      auto elements = resolver.elementsAt(valuePath);
      if (!elements || elements->cellType.isNull())
        continue;
      core::ReleaseRecord record = recordFor(effect);
      core::SymInfo hint;
      hint.type = core::SymInfo::Type::Pointer;
      hint.ctype = typeHandle(elements->cellType);
      for (const CallerRange &caller : rangesAt(*elements, *effect.elements)) {
        if (caller.range) {
          heap.releaseElements(state, caller.object, caller.range->first,
                               caller.range->second.first,
                               caller.range->second.second, record, hint);
          continue;
        }
        // Not expressible here: some elements, weakly.
        core::Sym some = caller.position ? heap.load(state, caller.object,
                                                     *caller.position, hint)
                                         : unknownValue(elements->cellType);
        core::ReleaseRecord weak = record;
        weak.allPaths = false;
        weak.aliasOnly = true;
        if (heap.info(state, some).type == core::SymInfo::Type::Pointer)
          heap.release(state, some, weak);
      }
      continue;
    }
    std::optional<core::Sym> value =
        effect.kind == core::PathEffect::Kind::Unknown
            ? unknownValues[ordinal]
            : resolver.valueAt(valuePath);
    core::PathEffect keyed = effect;
    if (effect.when.paramZero) {
      auto [index, zero] = *effect.when.paramZero;
      std::optional<bool> argument;
      if (index < args.size() && args[index] != core::ZeroSym)
        argument = isZeroValue(heap, state, args[index]);
      if (argument) {
        if (*argument != zero)
          continue; // the argument selects the exits without the effect
      } else {
        keyed.may = true;
      }
    }
    if (effect.when.paramsEqual) {
      std::optional<bool> equal =
          argumentsEqual(args, *effect.when.paramsEqual);
      if (!equal)
        keyed.may = true;
      else if (*equal != effect.when.paramsEqual->equal)
        continue; // the arguments select the paths without the effect
    }
    if (effect.when.entryZero) {
      std::optional<bool> zero;
      if (auto at = resolver.valueAt(effect.when.entryZero->first))
        zero = isZeroValue(heap, state, *at);
      if (!zero)
        keyed.may = true;
      else if (*zero != effect.when.entryZero->second)
        continue; // the value the callee tested selects the other paths
    }
    const core::PathEffect &applied = keyed;
    (void)applied;
    switch (effect.kind) {
    case core::PathEffect::Kind::Release:
    case core::PathEffect::Kind::Move: {
      // RFC 0031 §4.2, RFC 0030 §2.3: a path the caller's memory does not
      // hold as the callee saw it (a `void *` handed a record of another
      // layout): what the callee released is not known here.
      if ((!value || heap.info(state, *value).rawCast ||
           heap.info(state, *value).type != core::SymInfo::Type::Pointer) &&
          lostView(call, args, valuePath))
        break;
      if (!value)
        break;
      // RFC 0031 §4.2: the callee released a pointer where the caller's
      // type holds no pointer (`release_field(&second)` through a `struct
      // first *`): a pointer made of those bytes, released as a
      // `free((void *)n)` would be.
      if (const core::SymInfo &held = heap.info(state, *value);
          held.type == core::SymInfo::Type::Int &&
          held.pointerBehind == core::ZeroSym) {
        if (auto c = state.zone.constant(*value); c && *c == 0)
          break;
        core::Sym raw = unknownValue(context.VoidPtrTy);
        core::SymInfo &rawInfo = heap.infoMut(state, raw);
        rawInfo.raw = true;
        rawInfo.rawAt = here;
        value = raw;
        if (run.isPublishing())
          run.ledger().decideAs(call, core::SiteKind::Call,
                                core::Boundary::Call, core::Facet::Temporal,
                                core::FacetDecision::unresolvedFor(
                                    core::UnresolvedReason::RawCast));
      }
      // An interior release: the callee released the value plus `offset`.
      // (At an offset the callee does not know: into the same objects, at
      // one the caller does not know either.)
      if ((effect.offset != 0 || effect.anyOffset) &&
          heap.info(state, *value).type == core::SymInfo::Type::Pointer) {
        core::SymInfo shifted = heap.info(state, *value);
        shifted.pending.clear();
        shifted.release.reset();
        for (core::Target &target : shifted.targets)
          target.offset = effect.anyOffset
                              ? core::Term::unknown()
                              : target.offset.plusConstant(effect.offset);
        value = heap.fresh(state, shifted);
      }
      const core::SymInfo &info = heap.info(state, *value);
      if (info.type != core::SymInfo::Type::Pointer ||
          info.null == core::PointerNull::Null)
        break;
      core::ReleaseRecord record = recordFor(keyed);
      record.via = valuePath.isRoot() && valuePath.index < call.getNumArgs()
                       ? spell(*call.getArg(valuePath.index))
                       : "";
      // Through an index step: some element, which the caller cannot tell
      // (§4.9): no evidence about any one value.
      bool indexed = false;
      for (const core::PathElem &elem : valuePath.steps)
        indexed = indexed || elem.step == core::PathStep::Index;
      if (indexed) {
        record.allPaths = false;
        record.aliasOnly = true;
      }
      // RFC 0031 §5.5: the callee releases an argument that is no start of
      // a heap object (`release(&x)`), reported at the call.
      if (effect.kind == core::PathEffect::Kind::Release && !indexed &&
          (!valuePath.isParam() || !valuePath.isRoot() ||
           valuePath.index >= call.getNumArgs()))
        calleeRelease(call, *value, valuePath,
                      !effect.may && effect.when.always() && !effect.lossy);
      if (effect.kind == core::PathEffect::Kind::Release && !indexed &&
          valuePath.isParam() && valuePath.isRoot() &&
          valuePath.index < call.getNumArgs() && run.isPublishing()) {
        const Expr &argument = *call.getArg(valuePath.index);
        ReleaseCheck check =
            releaseCheck(*value, record.via, argument, "released");
        if (check.kind == ReleaseCheck::Kind::Violation ||
            check.kind == ReleaseCheck::Kind::Possible) {
          bool definite = check.kind == ReleaseCheck::Kind::Violation &&
                          !record.conditional;
          if (!definite) {
            std::size_t at = check.message.find(" but points");
            if (at != std::string::npos)
              check.message.replace(at, 11, " but may point");
          }
          core::Diagnostic diagnostic;
          diagnostic.id = core::diag::InvalidRelease;
          diagnostic.severity =
              definite ? core::Severity::Error : core::Severity::Warning;
          diagnostic.message = check.message;
          diagnostic.location =
              toCoreLocation(context.getSourceManager(), call.getBeginLoc());
          if (check.noteAt.isValid())
            diagnostic.addNote(check.note, check.noteAt);
          run.report(std::move(diagnostic),
                     definite ? core::Certainty::Definite
                              : core::Certainty::Possible,
                     &call, core::Facet::Spatial);
        }
      }
      // A release through a path of a value this function already released
      // (RFC 0030 §3.1): the second free is the callee's, at the call.
      std::optional<std::string> spelled;
      if (effect.kind == core::PathEffect::Kind::Release && !indexed &&
          run.isPublishing() && !(valuePath.isParam() && valuePath.isRoot())) {
        spelled = spellArgumentPath(call, valuePath);
        // A global the callee releases, spelled by its name here.
        if (!spelled && valuePath.isGlobal())
          if (const VarDecl *var =
                  functionRun().unitRun().globalDecl(valuePath.index))
            spelled = valuePath.toString(var->getNameAsString());
      }
      if (spelled) {
        core::TemporalVerdict before = heap.temporal(state, *value);
        bool certain =
            !keyed.may && effect.when.classes.empty() && !effect.lossy;
        // (Only for values of objects that stand for one runtime object
        // each: a k-limited summary object's elements are not told apart,
        // RFC 0031 §4.6.)
        bool singular = !heap.info(state, *value).top;
        for (const core::Target &target : heap.info(state, *value).targets)
          singular = singular && run.table().info(target.object).singular;
        if (!singular)
          before.kind = core::TemporalVerdict::Kind::Proven;
        // (Not a record this call made, nor one still pending on a null
        // argument the path goes through.)
        if (before.record && (before.record->where == here ||
                              pendingOnNullArgument(args, *value, valuePath,
                                                    before.record->where)))
          before.kind = core::TemporalVerdict::Kind::Proven;
        if (before.record && !before.record->unknownOrigin() &&
            before.record->reason != core::ReleaseRecord::Reason::Moved &&
            (before.kind == core::TemporalVerdict::Kind::Violation ||
             before.kind == core::TemporalVerdict::Kind::MayReleased)) {
          bool definite =
              certain && before.kind == core::TemporalVerdict::Kind::Violation;
          core::Diagnostic diagnostic;
          diagnostic.id = core::diag::DoubleFree;
          diagnostic.severity =
              definite ? core::Severity::Error : core::Severity::Warning;
          diagnostic.message =
              "'" + *spelled + "' " +
              (definite ? "is freed twice" : "may be freed twice");
          diagnostic.location =
              toCoreLocation(context.getSourceManager(), call.getBeginLoc());
          if (before.record->where.isValid())
            diagnostic.addNote(definite ? "previously freed here"
                                        : "previously freed here on some "
                                          "paths",
                               before.record->where);
          run.report(std::move(diagnostic),
                     definite ? core::Certainty::Definite
                              : core::Certainty::Possible,
                     &call, core::Facet::Temporal);
        }
      }
      // RFC 0007: the family the callee releases with against the one the
      // object was allocated in, as a direct release checks it.
      if (!effect.family.empty() && !indexed && run.isPublishing() &&
          info.targets.size() == 1 && !info.top) {
        const core::ObjectState *object =
            heap.findObject(state, info.targets[0].object);
        if (object != nullptr && !object->family.empty() &&
            object->family != effect.family &&
            object->life == core::Life::Live) {
          bool definite = !keyed.may && effect.when.always() && !effect.lossy;
          std::string place =
              spellArgumentPath(call, valuePath)
                  .value_or(valuePath.isParam() && valuePath.isRoot() &&
                                    valuePath.index < call.getNumArgs()
                                ? spell(*call.getArg(valuePath.index))
                                : std::string("an argument"));
          core::Diagnostic diagnostic;
          diagnostic.id = core::diag::MismatchedRelease;
          diagnostic.severity =
              definite ? core::Severity::Error : core::Severity::Warning;
          diagnostic.message =
              "'" + place + "' " + (definite ? "is" : "may be") +
              " released with '" + effect.family +
              "' but must be released with '" + object->family + "'";
          diagnostic.location = here;
          if (run.table().info(info.targets[0].object).created.isValid())
            diagnostic.addNote(
                "allocated here",
                run.table().info(info.targets[0].object).created);
          if (run.isPublishing())
            run.ledger().decideAs(
                call, core::SiteKind::Call, core::Boundary::Call,
                core::Facet::Temporal,
                definite ? core::FacetDecision::violation()
                         : core::FacetDecision::unresolvedFor(
                               core::UnresolvedReason::MayReleased));
          run.report(std::move(diagnostic),
                     definite ? core::Certainty::Definite
                              : core::Certainty::Possible,
                     &call, core::Facet::Temporal);
        }
      }
      heap.release(state, *value, record);
      if (effect.may || effect.lossy)
        possiblyReleased.insert(*value);
      if (!effect.when.classes.empty()) {
        core::PendingCase pending;
        for (core::ResultClass c : effect.when.classes)
          pending.classes.emplace_back(core::toString(c));
        pending.subject = *value;
        pending.record = record;
        pending.record.conditional = effect.may || effect.lossy;
        // (A parameter test the arguments did not decide: tested again
        // when the class is.)
        if (effect.when.paramZero &&
            effect.when.paramZero->first < args.size() &&
            args[effect.when.paramZero->first] != core::ZeroSym &&
            !isZeroValue(heap, state, args[effect.when.paramZero->first]))
          pending.argumentZero =
              std::make_pair(args[effect.when.paramZero->first],
                             effect.when.paramZero->second);
        heap.infoMut(state, result).pending.push_back(pending);
      }
      break;
    }
    case core::PathEffect::Kind::Unknown: {
      if (!value)
        break;
      const core::SymInfo &info = heap.info(state, *value);
      std::vector<core::ObjectId> objects;
      objects.reserve(info.targets.size());
      for (const core::Target &target : info.targets)
        objects.push_back(target.object);
      // The code the callee could not see had the object, and so every
      // object the caller's memory reaches from it (the callee's summary
      // names only those it had materialised), as a direct call to unknown
      // code does (havocReachable()).
      objects = reachableFrom(heap, state, objects);
      core::ReleaseRecord record;
      record.reason = core::ReleaseRecord::Reason::UnknownCallee;
      record.where = here;
      record.allPaths = false;
      for (core::ObjectId id : objects)
        if (state.objects.contains(id)) {
          core::ObjectState &object = state.objects.at(id);
          // (Storage of the caller's frame, a global or a literal is
          // written, never released, RFC 0030 §5.1.)
          core::ObjectKind kind = run.table().info(id).key.kind;
          if (kind != core::ObjectKind::Local &&
              kind != core::ObjectKind::Global &&
              kind != core::ObjectKind::Literal &&
              kind != core::ObjectKind::Function) {
            object.life = core::Life::UnknownReleased;
            object.record = record;
          }
          object.cells = {};
          object.havocked = true;
        }
      break;
    }
    case core::PathEffect::Kind::Escape:
      if (value) {
        const core::SymInfo &info = heap.info(state, *value);
        std::vector<core::ObjectId> objects;
        objects.reserve(info.targets.size());
        for (const core::Target &target : info.targets)
          objects.push_back(target.object);
        for (core::ObjectId id : objects)
          if (state.objects.contains(id))
            state.objects.at(id).escaped = true;
      }
      break;
    case core::PathEffect::Kind::ShareDown:
    case core::PathEffect::Kind::ShareUp:
      break;
    }
  }
  // Stores into the caller's memory. Their places and values name the
  // callee's entry state, so every one is read before any is written.
  struct Write {
    const core::StoreEffect *effect;
    QualType cellType;
    std::optional<Address> cell;
    std::optional<PathResolver::Elements> elements;
    core::Sym value;
  };
  std::vector<Write> writes;
  // Whether a store of a new object's contents could not be placed.
  bool lostContents = false;
  // Stores an argument selects (RFC 0030 §9.1): kept as they apply.
  std::vector<core::StoreEffect> selected;
  // Those whose parameter test the argument leaves open: on the other
  // paths the cell keeps its old value, and what that value says about
  // itself (a release the same call made) holds there.
  std::set<std::size_t> evidenced;
  // The new object a store makes on exactly the paths where the caller's
  // own entry value was null (an undecided lazy initialisation): it exists
  // where that entry test holds.
  std::map<std::size_t, core::EntryTest> madeWhere;
  for (const core::StoreEffect &store : effects.stores) {
    if (!store.when.paramZero && !store.when.entryZero) {
      selected.push_back(store);
      continue;
    }
    // Whether the call's values decide the case: an argument's, or the
    // value at the entry path the callee tested.
    bool decided = true;
    bool excluded = false;
    if (store.when.paramZero) {
      auto [index, zero] = *store.when.paramZero;
      std::optional<bool> argument;
      if (index < args.size() && args[index] != core::ZeroSym)
        argument = isZeroValue(heap, state, args[index]);
      decided = decided && argument.has_value();
      excluded = excluded || (argument && *argument != zero);
    }
    std::optional<core::EntryTest> entryTest;
    if (store.when.entryZero) {
      std::optional<bool> held;
      if (auto at = resolver.valueAt(store.when.entryZero->first)) {
        held = isZeroValue(heap, state, *at);
        if (const core::SymInfo *value = state.syms.find(*at);
            value != nullptr && value->entryOf &&
            value->entryOf->second.isConcrete())
          entryTest = core::EntryTest{.object = value->entryOf->first,
                                      .key = value->entryOf->second,
                                      .zero = store.when.entryZero->second};
      }
      decided = decided && held.has_value();
      excluded = excluded || (held && *held != store.when.entryZero->second);
    }
    if (excluded)
      continue;
    core::StoreEffect applied = store;
    applied.when.paramZero.reset();
    applied.when.entryZero.reset();
    if (!decided && !store.may && !store.when.paramZero && entryTest &&
        store.value.kind == core::ValueDesc::Kind::Fresh)
      madeWhere[selected.size()] = *entryTest;
    applied.may = applied.may || !decided;
    if (!decided)
      evidenced.insert(selected.size());
    selected.push_back(applied);
  }
  // The new objects stored into cells first: the stores below those cells
  // are their contents.
  std::map<const core::StoreEffect *, core::Sym> freshStored;
  std::vector<const core::StoreEffect *> byDepth;
  byDepth.reserve(selected.size());
  for (const core::StoreEffect &store : selected)
    byDepth.push_back(&store);
  std::ranges::stable_sort(
      byDepth, [](const core::StoreEffect *a, const core::StoreEffect *b) {
        return a->dest.steps.size() < b->dest.steps.size();
      });
  for (const core::StoreEffect *pointer : byDepth) {
    const core::StoreEffect &store = *pointer;
    if (store.value.kind != core::ValueDesc::Kind::Fresh)
      continue;
    bool indexed = false;
    for (const core::PathElem &elem : store.dest.steps)
      indexed = indexed || elem.step == core::PathStep::Index;
    QualType cellType;
    if (indexed || !resolver.destination(store, &cellType) ||
        cellType.isNull() || !cellType->isPointerType())
      continue;
    core::ObjectId object =
        freshObject(store.value, cellType->getPointeeType(), true);
    core::SymInfo info;
    info.type = core::SymInfo::Type::Pointer;
    info.targets = {
        core::Target{.object = object,
                     .offset = core::Term::of(store.value.offset.value_or(0))}};
    info.null = store.value.maybeNull ? core::PointerNull::Maybe
                                      : core::PointerNull::NonNull;
    // RFC 0030 §3.2: a new object the callee may have failed to make; a
    // null test of the cell tells the failure, which made no object.
    info.allocatorSource = store.value.maybeNull;
    core::Sym value = heap.fresh(state, info);
    if (auto made =
            madeWhere.find(static_cast<std::size_t>(&store - selected.data()));
        made != madeWhere.end() && state.objects.contains(object))
      state.objects.at(object).existsIfEntry = made->second;
    freshStored[&store] = value;
    resolver.setStored(store, value);
  }
  for (const core::StoreEffect &store : selected) {
    QualType cellType;
    std::optional<Address> cell;
    std::optional<PathResolver::Elements> elements;
    bool indexed = false;
    for (const core::PathElem &elem : store.dest.steps)
      indexed = indexed || elem.step == core::PathStep::Index;
    // A new object's contents the caller cannot place: its other cells are
    // not what the object was made with either (below).
    bool contents = store.contents || store.dest.isResult();
    if (indexed) {
      elements = resolver.destinationElements(store);
      if (!elements || elements->cellType.isNull()) {
        lostContents = lostContents || contents;
        continue;
      }
      cellType = elements->cellType;
    } else {
      cell = resolver.destination(store, &cellType);
      if (!cell || cellType.isNull()) {
        lostContents = lostContents || contents;
        continue;
      }
    }
    core::Sym value = core::ZeroSym;
    switch (store.value.kind) {
    case core::ValueDesc::Kind::Null:
      value = cellType->isPointerType() ? nullPointer(cellType)
                                        : constant(0, cellType);
      break;
    case core::ValueDesc::Kind::Path:
      if (store.value.path)
        if (auto at = resolver.valueAt(*store.value.path))
          value = pathValue(*at, store.value, cellType);
      break;
    case core::ValueDesc::Kind::Int:
      value = unknownValue(cellType);
      boundWithinType(state, value, store.value);
      boundByRange(value, cellType, store.value);
      break;
    case core::ValueDesc::Kind::Dangling:
      if (cellType->isPointerType())
        value = danglingValue(call, cellType, store.dest);
      break;
    case core::ValueDesc::Kind::Function:
      value = functionValue(store.value);
      break;
    case core::ValueDesc::Kind::Fresh: {
      if (auto it = freshStored.find(&store); it != freshStored.end()) {
        value = it->second;
      } else {
        QualType pointee =
            cellType->isPointerType() ? cellType->getPointeeType() : QualType();
        core::ObjectId object = freshObject(store.value, pointee, true);
        core::SymInfo info;
        info.type = core::SymInfo::Type::Pointer;
        info.targets = {core::Target{
            .object = object,
            .offset = store.value.interior
                          ? core::Term::unknown()
                          : core::Term::of(store.value.offset.value_or(0))}};
        info.null = store.value.maybeNull ? core::PointerNull::Maybe
                                          : core::PointerNull::NonNull;
        info.allocatorSource = store.value.maybeNull;
        value = heap.fresh(state, info);
      }
      // A store on some result classes only: on the others the object was
      // never made, which a test of the result tells.
      if (!store.when.classes.empty() && result != core::ZeroSym) {
        core::PendingCase absent;
        absent.kind = core::PendingCase::Kind::Absent;
        for (core::ResultClass c :
             {core::ResultClass::Null, core::ResultClass::NonNull,
              core::ResultClass::Zero, core::ResultClass::Positive,
              core::ResultClass::Negative})
          if (std::ranges::find(store.when.classes, c) ==
              store.when.classes.end())
            absent.classes.emplace_back(core::toString(c));
        absent.subject = value;
        heap.infoMut(state, result).pending.push_back(absent);
      }
      // Null on some classes (the allocation failed there): never made on
      // those, non-null on the others.
      if (!store.absentOn.empty() && result != core::ZeroSym) {
        core::PendingCase absent;
        absent.kind = core::PendingCase::Kind::Absent;
        core::PendingCase made;
        made.kind = core::PendingCase::Kind::NonNull;
        for (core::ResultClass c :
             {core::ResultClass::Null, core::ResultClass::NonNull,
              core::ResultClass::Zero, core::ResultClass::Positive,
              core::ResultClass::Negative})
          (std::ranges::find(store.absentOn, c) != store.absentOn.end() ? absent
                                                                        : made)
              .classes.emplace_back(core::toString(c));
        absent.subject = value;
        made.subject = value;
        heap.infoMut(state, result).pending.push_back(absent);
        heap.infoMut(state, result).pending.push_back(made);
      }
      break;
    }
    default:
      break;
    }
    if (value == core::ZeroSym) {
      value = unknownValue(cellType);
      rawValue(value, store.value);
    }
    writes.push_back(Write{.effect = &store,
                           .cellType = cellType,
                           .cell = std::move(cell),
                           .elements = std::move(elements),
                           .value = value});
  }
  // Contents the caller could not place (a type it reads differently): the
  // new objects' unwritten cells read as unknown, not as zeros.
  if (lostContents) {
    for (const auto &[index, object] : created)
      if (state.objects.contains(object))
        state.objects.at(object).havocked = true;
    if (type->isRecordType()) {
      core::ObjectId temporary = run.recordResultObject(call);
      if (state.objects.contains(temporary))
        state.objects.at(temporary).havocked = true;
    }
  }
  // The objects of the string facts, named in the entry state (§6.3).
  std::vector<std::pair<const core::StringEffect *, core::Target>> strings;
  for (const core::StringEffect &string : effects.strings) {
    core::StoreEffect probe;
    probe.dest = string.path;
    probe.contents = string.contents;
    QualType probeType;
    auto address = resolver.destination(probe, &probeType);
    if (address && !address->top && address->targets.size() == 1 &&
        run.table().info(address->targets[0].object).singular)
      strings.emplace_back(&string, address->targets[0]);
  }
  // Two stores of different values that land on one caller cell (aliased
  // arguments, `writes(&x, &x)`): the summary does not order them, so the
  // cell holds either (the alias context of §6.6 orders them).
  std::map<std::pair<core::ObjectId, std::int64_t>, core::Sym> collided;
  {
    std::map<std::pair<core::ObjectId, std::int64_t>, std::vector<core::Sym>>
        landing;
    for (const Write &write : writes)
      if (write.cell && write.cell->targets.size() == 1 && !write.cell->top &&
          write.cell->targets[0].offset.isConstant())
        landing[{write.cell->targets[0].object,
                 write.cell->targets[0].offset.constant}]
            .push_back(write.value);
    for (auto &[at, values] : landing) {
      std::ranges::sort(values);
      auto repeated = std::ranges::unique(values);
      values.erase(repeated.begin(), repeated.end());
      if (values.size() < 2)
        continue;
      core::Sym joined = values.front();
      for (std::size_t i = 1; i < values.size(); ++i)
        joined = heap.mergeWeak(state, joined, values[i]);
      collided[at] = joined;
    }
  }
  // An unknown value at an object's own path, or over some of its bytes:
  // the callee rewrote them (deriveEffects), so none of the cells there or
  // of its string facts stand. These come first: the summary's other
  // stores are what the callee wrote after. (Its bytes only: a pointer
  // into the middle of a caller's object, such as `&s.end`, names that
  // member.) On some paths only, the cells keep their values beside
  // unknown ones: a pointer the caller left there still reaches its object.
  auto rewrites = [](const core::StoreEffect &store) {
    return store.value.kind == core::ValueDesc::Kind::Unknown &&
           !store.dest.steps.empty() &&
           store.dest.steps.back().step == core::PathStep::Deref;
  };
  auto unknownLike = [&](core::Sym sym) {
    const core::SymInfo &old = heap.info(state, sym);
    core::SymInfo unknown;
    unknown.type = old.type;
    unknown.ctype = old.ctype;
    if (unknown.type == core::SymInfo::Type::Pointer) {
      core::ObjectId any = run.unknownObject();
      heap.ensure(state, any);
      unknown.targets = {core::Target{.object = any}};
    }
    return heap.fresh(state, unknown);
  };
  for (Write &write : writes) {
    const core::StoreEffect &store = *write.effect;
    if (!rewrites(store) || !write.cell)
      continue;
    bool weak = store.may || !store.when.always();
    for (const core::Target &target : write.cell->targets) {
      heap.ensure(state, target.object);
      std::optional<std::int64_t> from;
      std::optional<std::int64_t> size;
      if (target.offset.isConstant()) {
        if (store.bytes) {
          from = target.offset.constant + store.bytes->first;
          size = store.bytes->second - store.bytes->first;
        } else if (auto bytes = sizeOf(write.cellType); bytes && *bytes > 0) {
          from = target.offset.constant;
          size = bytes;
        }
      }
      if (weak)
        heap.weakenCells(state, target.object, from.value_or(0), size,
                         unknownLike);
      else
        heap.forgetCells(state, target.object, from.value_or(0), size);
      state.objects.at(target.object).stored = true;
    }
  }
  for (Write &write : writes) {
    const core::StoreEffect &store = *write.effect;
    QualType cellType = write.cellType;
    std::optional<Address> &cell = write.cell;
    std::optional<PathResolver::Elements> &elements = write.elements;
    core::Sym value = write.value;
    if (cell && cell->targets.size() == 1 && !cell->top &&
        cell->targets[0].offset.isConstant())
      if (auto it = collided.find(
              {cell->targets[0].object, cell->targets[0].offset.constant});
          it != collided.end())
        value = it->second;
    if (elements) {
      // RFC 0015 §5: a range of elements each holding such a value, or (no
      // range) some elements, weakly.
      std::vector<CallerRange> callers;
      if (store.elements)
        callers = rangesAt(*elements, *store.elements);
      else
        for (const core::Target &target : elements->targets)
          callers.push_back(someAt(target, elements->stride));
      // RFC 0031 *Implementation amendments*, "Stores past the caller's
      // object": a range the callee writes on every return, outside the one
      // object the argument points into.
      if (store.elements && !store.may && store.when.always() &&
          effects.returns == core::FunctionEffects::Returns::Always &&
          callers.size() == 1 && callers[0].range &&
          elements->targets.size() == 1)
        if (const core::Term &from = callers[0].range->second.first,
            &to = callers[0].range->second.second;
            from.isConstant() && to.isConstant() && from.constant < to.constant)
          storePastObject(
              call, store, elements->targets[0], elements->targets[0].offset,
              static_cast<__int128>(callers[0].range->first.offset) +
                  (static_cast<__int128>(from.constant) * elements->stride),
              static_cast<__int128>(callers[0].range->first.offset) +
                  (static_cast<__int128>(to.constant - 1) * elements->stride) +
                  cellBytes(context, elements->cellType, elements->stride));
      for (const CallerRange &caller : callers) {
        if (caller.range) {
          heap.writeElements(state, caller.object, caller.range->first,
                             caller.range->second.first,
                             caller.range->second.second, value,
                             store.may || !store.when.always());
          continue;
        }
        heap.ensure(state, caller.object);
        if (caller.position) {
          // (Joined with what the elements held: their values read first.)
          if (!heap.read(state, caller.object, *caller.position))
            run.unwritten(state, caller.object, *caller.position,
                          heap.info(state, value));
          heap.write(state, caller.object, *caller.position, value, true);
          continue;
        }
        // (At an offset not known here: any of its bytes.)
        heap.forgetCells(state, caller.object, 0, std::nullopt);
        state.objects.at(caller.object).havocked = true;
      }
      continue;
    }
    // The same for one cell.
    if (!store.may && store.when.always() &&
        effects.returns == core::FunctionEffects::Returns::Always && cell &&
        !cell->top && cell->targets.size() == 1 &&
        cell->targets[0].offset.isConstant())
      if (auto bytes = sizeOf(cellType); bytes && *bytes > 0 &&
                                         store.dest.isParam() &&
                                         store.dest.index < args.size()) {
        // (Behind the argument, where it points into the object.)
        __int128 start = cell->targets[0].offset.constant;
        __int128 end = start + *bytes;
        if (store.bytes) {
          end = start + store.bytes->second;
          start += store.bytes->first;
        }
        for (const core::Target &argument :
             heap.info(state, args[store.dest.index]).targets)
          if (argument.object == cell->targets[0].object)
            storePastObject(call, store, cell->targets[0], argument.offset,
                            start, end);
      }
    // (Rewritten bytes were forgotten above.)
    if (rewrites(store))
      continue;
    if (store.may || !store.when.always()) {
      // A store on some paths only: the old value or the new.
      for (const core::Target &target : cell->targets)
        heap.ensure(state, target.object);
      Address weakCell = *cell;
      core::Sym old = load(weakCell, cellType, nullptr);
      core::Sym stored = value;
      value = evidenced.contains(
                  static_cast<std::size_t>(write.effect - selected.data()))
                  ? heap.mergePossible(state, old, value)
                  : heap.mergeWeak(state, old, value);
      // A store on some result classes: a test of the result tells which
      // value the cell holds (RFC 0030 §9.1, RFC 0031 §6.3). Not when the
      // call may have released the old or the new value on some class it
      // cannot name: the join keeps that release an alias's (§5.5), where
      // either value alone would make it a possible finding the summary
      // cannot place.
      if (!store.may && !store.when.classes.empty() && !store.when.paramZero &&
          result != core::ZeroSym && value != old && value != stored &&
          !possiblyReleased.contains(old) &&
          !possiblyReleased.contains(stored)) {
        core::PendingCase pending;
        pending.kind = core::PendingCase::Kind::Stored;
        for (core::ResultClass c : store.when.classes)
          pending.classes.emplace_back(core::toString(c));
        pending.subject = value;
        pending.stored = stored;
        pending.previous = old;
        heap.infoMut(state, result).pending.push_back(pending);
      }
    }
    this->store(*cell, value, cellType, nullptr);
  }
  // RFC 0012: the strings the callee left, once its stores are made.
  for (const auto &[string, target] : strings) {
    if (!state.objects.contains(target.object))
      continue;
    core::Term within = termAtCall(string->nulWithin);
    auto at = within.known ? target.offset.plus(within) : std::nullopt;
    if (!at)
      continue;
    std::optional<core::Term> from;
    if (string->nulFrom) {
      core::Term start = termAtCall(*string->nulFrom);
      if (start.known)
        from = target.offset.plus(start);
    }
    core::ObjectState &object = state.objects.at(target.object);
    object.nulWithin = *at;
    object.nulFrom = from;
  }
  // Code the callee ran that the analysis does not see (and the
  // unknown-callee default of an incomplete summary) may write any global.
  if (effects.incomplete || effects.unknownGlobals) {
    core::ReleaseRecord record;
    record.reason = core::ReleaseRecord::Reason::UnknownCallee;
    record.where = here;
    record.allPaths = false;
    if (const FunctionDecl *direct = call.getDirectCallee())
      record.via = direct->getNameAsString();
    forgetGlobals(record);
  }
  // An incomplete summary adds the unknown-callee default (RFC 0030 §5.5):
  // what the arguments reach may be written, and released where it may be.
  if (effects.incomplete)
    for (unsigned i = 0; i < call.getNumArgs() && i < args.size(); ++i) {
      QualType argType = call.getArg(i)->getType();
      if (argType->isPointerType())
        havocArgument(call, args[i],
                      argType->getPointeeType().isConstQualified(), false);
    }
  if (effects.returns == core::FunctionEffects::Returns::Never)
    state.unreachable = true;
  // Cases the arguments left one result class for are decided now.
  settlePending(run, state, result);
  // Guard functions (RFC 0030 §9.2): what a class of the result implies.
  for (const auto &[resultClass, paths] : effects.nonNullOn)
    for (const core::SummaryPath &path : paths)
      if (path.isParam() && path.steps.empty() && path.index < args.size()) {
        core::PendingCase pending;
        pending.classes = {std::string(core::toString(resultClass))};
        pending.kind = core::PendingCase::Kind::NonNull;
        pending.subject = args[path.index];
        heap.infoMut(state, result).pending.push_back(pending);
      }
  return result;
}

} // namespace weavec::analysis::engine
