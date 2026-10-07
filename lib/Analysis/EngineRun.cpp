//===- EngineRun.cpp - One function body in the object engine -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0031 §3–§4: the always-add CFG, the entry state, the fixpoint with
// widening at loop heads, and the final (publishing) pass. Objects are
// interned here, and the unwritten cells of the entry heap are materialised
// (§4.6).
//
//===----------------------------------------------------------------------===//

#include "Engine.h"
#include "weavec/Analysis/Annotations.h"
#include "weavec/Analysis/BypassedDeclarations.h"
#include "weavec/Analysis/ClangLocation.h"

#include "clang/AST/ParentMap.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <set>
#include <string_view>
#include <utility>

using namespace clang;

namespace weavec::analysis::engine {

core::Handle typeHandle(QualType type) noexcept {
  if (type.isNull())
    return 0;
  return handleOf(type.getCanonicalType().getAsOpaquePtr());
}

QualType typeOfHandle(core::Handle handle) noexcept {
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr):
  // a type handle is the type's opaque pointer.
  return QualType::getFromOpaquePtr(
      reinterpret_cast<void *>(static_cast<std::uintptr_t>(handle)));
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
}

/// The largest number of times one field may repeat in an entry path before
/// the path is folded (§4.6 k-limit), and the longest path.
static constexpr unsigned MaxFieldRepeats = 2;
static constexpr std::size_t MaxEntryDepth = 6;
/// §4.8: widening after this many joins at a loop head.
static constexpr unsigned WidenAfter = 2;
/// Widening rounds a loop head stops at program constants before a growing
/// bound is dropped (§4.8).
static constexpr unsigned ThresholdRounds = 8;
static constexpr unsigned MaxVisitsPerBlock = 64;
/// A head many edges reach may change once or twice a round per edge.
static constexpr unsigned VisitsPerPredecessor = 2;
/// Joins at one loop head, changing or not, before the function is over
/// budget: a head that many back edges reach and that keeps changing is
/// not converging, and each join of a large state is costly. A head that
/// many edges reach (an interpreter's dispatch, one edge per opcode) gets
/// as many per edge, so a round of them does not exhaust it.
static constexpr unsigned MaxJoinsPerBlock = 256;
static constexpr unsigned JoinsPerPredecessor = 64;
/// RFC 0034 §7.1: the size of a run's entry states (memory), and of its
/// graph, over budget.
static constexpr std::uint64_t MaxRetainedState = 1000000;
static constexpr unsigned MaxBlocks = 100000;
/// Symbols a block's joined state may hold before the function is over
/// budget: every later join pairs them all (sqlite's `sqlite3VdbeExec`
/// reached 4,677; the next largest in the corpus, under 1,000).
static constexpr std::size_t MaxStateSymbols = 2048;

FunctionRun::FunctionRun(UnitRun &unitRun, const FunctionDecl &fn,
                         LedgerAdapter &adapter, RunMode runMode,
                         const AliasContext *alias, unsigned depth)
    : unit(unitRun), function(fn), aliasContext(alias), contextDepth(depth),
      context(unitRun.context()), out(adapter), publishTo(&adapter),
      mode(runMode), heap(objects, *this) {}

FunctionRun::~FunctionRun() = default;

//===----------------------------------------------------------------------===//
// Objects
//===----------------------------------------------------------------------===//

/// The name a declaration is spelled with.
static std::string nameOf(const NamedDecl &decl) {
  return decl.getNameAsString();
}

core::ObjectId FunctionRun::variableObject(const VarDecl &var) const {
  const VarDecl *canonical = var.getCanonicalDecl();
  if (auto it = localObjects.find(canonical); it != localObjects.end())
    return it->second;
  core::ObjectKey key;
  key.kind = var.hasGlobalStorage() ? core::ObjectKind::Global
                                    : core::ObjectKind::Local;
  key.handle = handleOf(canonical);
  core::ObjectInfo info;
  info.type = typeHandle(var.getType());
  info.singular = true;
  info.name = nameOf(var);
  info.created = toCoreLocation(context.getSourceManager(), var.getLocation());
  core::ObjectId id = objects.intern(key, info);
  localObjects[canonical] = id;
  return id;
}

core::ObjectId FunctionRun::literalObject(const Expr &literal) const {
  core::ObjectKey key;
  key.kind = isa<StringLiteral>(literal) || isa<PredefinedExpr>(literal)
                 ? core::ObjectKind::Literal
                 : core::ObjectKind::Local;
  key.handle = handleOf(&literal);
  key.expression = key.kind == core::ObjectKind::Local;
  core::ObjectInfo info;
  info.type = typeHandle(literal.getType());
  info.name = key.kind == core::ObjectKind::Literal ? "a string literal"
                                                    : "a compound literal";
  info.created =
      toCoreLocation(context.getSourceManager(), literal.getBeginLoc());
  return objects.intern(key, info);
}

core::ObjectId FunctionRun::recordResultObject(const CallExpr &call) const {
  core::ObjectKey key;
  key.kind = core::ObjectKind::Local;
  key.handle = handleOf(&call);
  key.expression = true;
  core::ObjectInfo info;
  info.type = typeHandle(call.getType());
  const FunctionDecl *callee = call.getDirectCallee();
  info.name = "the result of '" +
              (callee != nullptr ? callee->getNameAsString()
                                 : std::string("an indirect call")) +
              "'";
  info.created = toCoreLocation(context.getSourceManager(), call.getBeginLoc());
  return objects.intern(key, info);
}

core::ObjectId
FunctionRun::allocationObject(const Expr &site, QualType pointee,
                              const std::string &name,
                              const core::SummaryPath &path) const {
  core::ObjectKey key;
  key.kind = core::ObjectKind::HeapRecent;
  key.handle = handleOf(&site);
  key.path = path;
  core::ObjectInfo info;
  info.type = pointee.isNull() || pointee->isVoidType() || pointee->isCharType()
                  ? 0
                  : typeHandle(pointee);
  info.name = name;
  info.created = toCoreLocation(context.getSourceManager(), site.getBeginLoc());
  return objects.intern(key, info);
}

core::ObjectId FunctionRun::callResultObject(const Expr &site, QualType pointee,
                                             const std::string &name) const {
  core::ObjectKey key;
  key.kind = core::ObjectKind::CallResult;
  key.handle = handleOf(&site);
  key.path = core::SummaryPath::result().deref();
  core::ObjectInfo info;
  info.type =
      pointee.isNull() || pointee->isVoidType() ? 0 : typeHandle(pointee);
  info.name = name;
  info.created = toCoreLocation(context.getSourceManager(), site.getBeginLoc());
  return objects.intern(key, info);
}

/// Stable handles for the library's hidden state slots: the address of the
/// slot's name in a process-wide set (never a Clang entity).
static core::Handle stateHandle(const std::string &slot) {
  static std::mutex lock;
  static std::set<std::string> slots;
  std::scoped_lock guard(lock);
  return handleOf(&*slots.insert(slot).first);
}

core::ObjectId FunctionRun::stateObject(const std::string &slot) const {
  core::ObjectKey key;
  key.kind = core::ObjectKind::CallResult;
  key.handle = stateHandle(slot);
  key.path = core::SummaryPath::result().deref();
  key.cell = 1;
  core::ObjectInfo info;
  info.name = "<" + slot + ">";
  return objects.intern(key, info);
}

bool FunctionRun::isStateObject(core::ObjectId id) const {
  const core::ObjectKey &key = objects.info(id).key;
  return key.kind == core::ObjectKind::CallResult && key.cell == 1 &&
         key.path == core::SummaryPath::result().deref();
}

core::ObjectId FunctionRun::unknownObject() const {
  core::ObjectKey key;
  key.kind = core::ObjectKind::Unknown;
  core::ObjectInfo info;
  info.singular = false;
  info.name = "an unknown object";
  return objects.intern(key, info);
}

/// The field at byte `offset` of `type`, descending into nested records and
/// arrays, with its byte offset; for an array, the element type at the
/// offset with no field.
namespace {
struct FieldAt {
  const FieldDecl *field = nullptr;
  QualType type;
};
} // namespace

static std::optional<FieldAt> fieldAt(const ASTContext &context, QualType type,
                                      std::int64_t offset) {
  type = type.getCanonicalType();
  for (int depth = 0; depth < 16; ++depth) {
    if (offset < 0)
      return std::nullopt;
    if (const auto *array = context.getAsArrayType(type)) {
      QualType element = array->getElementType();
      if (element->isIncompleteType())
        return std::nullopt;
      std::int64_t size = static_cast<std::int64_t>(
          context.getTypeSizeInChars(element).getQuantity());
      if (size <= 0)
        return std::nullopt;
      offset %= size;
      type = element.getCanonicalType();
      continue;
    }
    const RecordDecl *record = type->getAsRecordDecl();
    if (record == nullptr || !record->isCompleteDefinition()) {
      if (offset == 0)
        return FieldAt{.field = nullptr, .type = type};
      return std::nullopt;
    }
    const ASTRecordLayout &layout = context.getASTRecordLayout(record);
    const FieldDecl *found = nullptr;
    std::int64_t foundOffset = 0;
    for (const FieldDecl *field : record->fields()) {
      auto fieldOffset = static_cast<std::int64_t>(
          layout.getFieldOffset(field->getFieldIndex()) /
          context.getCharWidth());
      QualType fieldType = field->getType();
      std::int64_t fieldSize =
          fieldType->isIncompleteType()
              ? 0
              : static_cast<std::int64_t>(
                    context.getTypeSizeInChars(fieldType).getQuantity());
      bool inside =
          offset >= fieldOffset && (offset < fieldOffset + fieldSize ||
                                    (fieldSize == 0 && offset == fieldOffset) ||
                                    fieldType->isIncompleteArrayType());
      if (inside) {
        found = field;
        foundOffset = fieldOffset;
        if (!record->isUnion())
          break;
        // A union: prefer a pointer member.
        if (fieldType->isPointerType())
          break;
      }
    }
    if (found == nullptr)
      return std::nullopt;
    offset -= foundOffset;
    QualType fieldType = found->getType().getCanonicalType();
    if (offset == 0 && !fieldType->isRecordType() &&
        !context.getAsArrayType(fieldType))
      return FieldAt{.field = found, .type = fieldType};
    if (fieldType->isRecordType() || context.getAsArrayType(fieldType)) {
      type = fieldType;
      if (offset == 0 && !fieldType->isRecordType()) {
        // The first element of an array member.
        continue;
      }
      continue;
    }
    return FieldAt{.field = found, .type = fieldType};
  }
  return std::nullopt;
}

/// The entry path of an object whose cells hold entry values, if it has one.
static std::optional<core::SummaryPath>
entryPathOf(const core::ObjectTable &objects, core::ObjectId id,
            const FunctionDecl &function, UnitRun &unit) {
  const core::ObjectInfo &info = objects.info(id);
  switch (info.key.kind) {
  case core::ObjectKind::Global:
    if (const auto *var =
            dyn_cast_or_null<VarDecl>(fromHandle<Decl>(info.key.handle)))
      return core::SummaryPath::global(unit.globalId(*var));
    return std::nullopt;
  case core::ObjectKind::Entry:
  case core::ObjectKind::EntrySummary:
  case core::ObjectKind::CallResult:
    return info.key.path;
  case core::ObjectKind::Local:
    if (const auto *param = dyn_cast_or_null<ParmVarDecl>(variableOf(info)))
      for (unsigned i = 0; i < function.getNumParams(); ++i)
        if (function.getParamDecl(i)->getCanonicalDecl() ==
            param->getCanonicalDecl())
          return core::SummaryPath::param(i);
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

core::ObjectId FunctionRun::childEntryObject(const core::HeapState &state,
                                             core::ObjectId parent,
                                             core::CellKey key,
                                             QualType pointee,
                                             const FieldDecl *field) const {
  (void)state;
  const core::ObjectInfo &parentInfo = objects.info(parent);
  std::optional<core::SummaryPath> parentPath =
      entryPathOf(objects, parent, function, unit);
  core::ObjectKey childKey;
  core::ObjectInfo info;
  info.type =
      pointee.isNull() || pointee->isVoidType() ? 0 : typeHandle(pointee);
  info.singular = true;
  bool owning =
      field != nullptr && unit.owningSlots.contains(field->getCanonicalDecl());
  info.fromOwningSlot = owning;
  if (owning)
    info.ownedFrom = parent;
  std::string step = field != nullptr ? field->getNameAsString()
                                      : "#" + std::to_string(key.offset);
  // An element no concrete offset names: the objects of every element
  // (§4.2 *Amendment (arrays)*), several runtime objects.
  bool elements = key.isSummary() || key.isSelected();
  if (elements) {
    step = "[*]";
    info.singular = false;
  }
  if (parentPath) {
    core::SummaryPath path = *parentPath;
    if (elements)
      path = elementsOf(path);
    else if (parentInfo.type != 0)
      // Through nested members (`box.data`), so a caller's paths resolve.
      path = cellPath(context, typeOfHandle(parentInfo.type), path, key.offset,
                      false);
    else if (field != nullptr || key.offset != 0)
      path = path.field(step);
    path = path.deref();
    // §4.6 k-limit: a field repeated too often, or a path too long, folds
    // into a summary of everything below the prefix.
    std::map<std::string, unsigned> repeats;
    bool fold = path.steps.size() > MaxEntryDepth * 2;
    for (const core::PathElem &elem : path.steps)
      if (elem.step == core::PathStep::Field &&
          ++repeats[elem.field] > MaxFieldRepeats)
        fold = true;
    childKey.kind = parentInfo.key.kind == core::ObjectKind::CallResult
                        ? core::ObjectKind::CallResult
                        : core::ObjectKind::Entry;
    childKey.handle = parentInfo.key.kind == core::ObjectKind::CallResult
                          ? parentInfo.key.handle
                          : 0;
    if (fold || parentInfo.key.kind == core::ObjectKind::EntrySummary) {
      // The summary of the chain: the prefix before the first repeat.
      core::SummaryPath prefix = path.rootPath();
      std::map<std::string, unsigned> seen;
      for (const core::PathElem &elem : path.steps) {
        if (elem.step == core::PathStep::Field && ++seen[elem.field] > 1)
          break;
        prefix.steps.pushBack(elem);
      }
      childKey.kind = core::ObjectKind::EntrySummary;
      childKey.path = std::move(prefix);
      info.singular = false;
      // §4.5 D3 below the k-limit: a summary reached through an owning slot
      // of an entry object stands for objects owned below it (a load through
      // it by an owning slot stays owned below that object), so it is one
      // object per such owner, and distinct from the owner and what the
      // owner is owned by. Reached any other way, it is owned by nothing.
      core::ObjectId owner = 0;
      if (owning)
        owner = parentInfo.key.kind == core::ObjectKind::EntrySummary
                    ? parentInfo.ownedFrom
                    : parent;
      childKey.parent = owner;
      info.ownedFrom = owner;
    } else {
      childKey.path = std::move(path);
      if (key.isConcrete() && childKey.kind == core::ObjectKind::Entry)
        info.heldIn = std::make_pair(parent, key.offset);
    }
    info.name = parentInfo.name.empty()
                    ? step
                    : parentInfo.name + (elements ? "[*]" : "->" + step);
  } else {
    // Below an object with no entry path (an allocation reached by an
    // unknown callee, an unknown object): an unknown object.
    return unknownObject();
  }
  return objects.intern(childKey, info);
}

/// The extent a kind gives an entry object.
static std::optional<core::Extent> extentOfKind(const ASTContext &context,
                                                const KindEntry *kind,
                                                QualType pointee) {
  std::optional<std::int64_t> width;
  if (!pointee.isNull() && !pointee->isIncompleteType() &&
      !pointee->isVoidType() && !pointee->isFunctionType())
    width = static_cast<std::int64_t>(
        context.getTypeSizeInChars(pointee).getQuantity());
  if (kind == nullptr) {
    // The A1/A3 Single default: one element.
    if (width)
      return core::Extent{.bytes = core::Term::of(*width),
                          .cls = core::ExtentClass::LowerBound};
    return std::nullopt;
  }
  switch (kind->kind.shape) {
  case core::PointerShape::Single:
    if (width)
      return core::Extent{.bytes = core::Term::of(*width),
                          .cls = core::ExtentClass::LowerBound};
    return std::nullopt;
  case core::PointerShape::Counted:
  case core::PointerShape::Sized:
    if (kind->kind.extent.isConstant()) {
      std::int64_t elementSize =
          kind->kind.shape == core::PointerShape::Counted && width ? *width : 1;
      core::ExtentClass cls =
          kind->extentClass.value_or(core::ExtentClass::LowerBound);
      return core::Extent{
          .bytes = core::Term::of(kind->kind.extent.offset * elementSize),
          .cls = cls};
    }
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

core::Sym FunctionRun::entryValue(core::HeapState &state, QualType type,
                                  core::ObjectId parent, core::CellKey key,
                                  const FieldDecl *field) const {
  type = type.getCanonicalType();
  core::SymInfo value;
  if (type->isPointerType()) {
    QualType pointee = type->getPointeeType();
    if (pointee->isFunctionType()) {
      value.type = core::SymInfo::Type::Function;
      value.functionsKnown = false;
      core::Sym sym = heap.fresh(state, value);
      return sym;
    }
    core::ObjectId child = childEntryObject(state, parent, key, pointee, field);
    heap.ensure(state, child);
    const KindEntry *kind = field != nullptr ? unit.fieldKind(*field) : nullptr;
    core::ObjectState &childState = heap.object(state, child);
    // The kind's extent holds of every object an element stands for.
    if (!childState.extent &&
        (objects.info(child).singular || !key.isConcrete()))
      childState.extent = extentOfKind(context, kind, pointee);
    // A count in a sibling field (`WEAVEC_COUNTED_BY(len)`).
    if (kind != nullptr && field != nullptr &&
        core::hasExtent(kind->kind.shape) && kind->kind.extent.path &&
        kind->kind.extent.path->root == core::ExtentPath::Root::Field &&
        !kind->shapeFromSystemHeader()) {
      const RecordDecl *record = field->getParent();
      const FieldDecl *sibling = nullptr;
      for (const FieldDecl *candidate : record->fields())
        if (candidate->getName() == kind->kind.extent.path->field)
          sibling = candidate;
      if (sibling != nullptr && record->isCompleteDefinition() &&
          sibling->getType()->isIntegralOrEnumerationType()) {
        const ASTRecordLayout &layout = context.getASTRecordLayout(record);
        std::int64_t base =
            key.offset - static_cast<std::int64_t>(
                             layout.getFieldOffset(field->getFieldIndex()) /
                             context.getCharWidth());
        core::CellKey siblingKey{
            .offset =
                base + static_cast<std::int64_t>(
                           layout.getFieldOffset(sibling->getFieldIndex()) /
                           context.getCharWidth())};
        core::Sym count = core::ZeroSym;
        if (auto existing = heap.read(state, parent, siblingKey))
          count = *existing;
        else
          count =
              unwritten(state, parent, siblingKey,
                        core::SymInfo{.type = core::SymInfo::Type::Int,
                                      .ctype = typeHandle(sibling->getType())});
        std::int64_t elementSize =
            kind->kind.shape == core::PointerShape::Counted &&
                    !pointee->isVoidType() && !pointee->isIncompleteType()
                ? static_cast<std::int64_t>(
                      context.getTypeSizeInChars(pointee).getQuantity())
                : 1;
        core::Term bytes =
            core::Term::ofSym(count, kind->kind.extent.scale * elementSize,
                              kind->kind.extent.offset * elementSize);
        core::Extent extent{
            .bytes = bytes,
            .cls = kind->extentClass.value_or(core::ExtentClass::Declared)};
        // RFC 0030 §7.6: an inferred invariant holds as C computes `f *
        // sizeof *d`, which may wrap: the bytes are that product's value,
        // never more than the term (RFC 0031 *Implementation amendments*).
        if (kind->kind.source == core::KindSource::Inferred &&
            bytes.scale > 1) {
          auto top = state.zone.upper(count);
          __int128 most =
              top ? (static_cast<__int128>(*top) * bytes.scale) + bytes.constant
                  : static_cast<__int128>(INT64_MAX) + 1;
          if (!top || most > INT64_MAX) {
            core::SymInfo product{.type = core::SymInfo::Type::Int,
                                  .ctype = typeHandle(context.getSizeType())};
            product.unwrapped = bytes;
            product.defined =
                core::SymDefinition{.op = core::IntegerOp::Multiply,
                                    .left = count,
                                    .constant = bytes.scale};
            core::Sym computed = heap.fresh(state, product);
            state.zone.addRange(computed, 0, std::nullopt);
            extent.bytes = core::Term::ofSym(computed);
            extent.unwrapped = bytes;
          }
        }
        heap.object(state, child).extent = extent;
      }
    }
    value.type = core::SymInfo::Type::Pointer;
    value.targets = {
        core::Target{.object = child, .offset = core::Term::of(0)}};
    value.null = kind != nullptr && kind->declaresNonnull()
                     ? core::PointerNull::NonNull
                     : core::PointerNull::Maybe;
    value.name = objects.info(child).name;
    value.ctype = typeHandle(type);
    return heap.fresh(state, value);
  }
  if (type->isIntegralOrEnumerationType()) {
    value.type = core::SymInfo::Type::Int;
    core::IntegerType integer{
        .width = static_cast<unsigned>(context.getTypeSize(type)),
        .isSigned = type->isSignedIntegerOrEnumerationType(),
        .isBoolean = type->isBooleanType()};
    value.intType = integer;
    value.ctype = typeHandle(type);
    core::Sym sym = heap.fresh(state, value);
    if (integer.width <= 63 || !integer.isSigned) {
      if (integer.isSigned) {
        std::int64_t hi =
            static_cast<std::int64_t>(std::uint64_t{1} << (integer.width - 1)) -
            1;
        state.zone.addRange(sym, -hi - 1, hi);
      } else if (integer.width < 63) {
        state.zone.addRange(
            sym, 0,
            static_cast<std::int64_t>(std::uint64_t{1} << integer.width) - 1);
      } else {
        state.zone.addRange(sym, 0, std::nullopt);
      }
    }
    return sym;
  }
  value.type = core::SymInfo::Type::Unknown;
  value.ctype = typeHandle(type);
  return heap.fresh(state, value);
}

bool FunctionRun::typesMayAlias(core::Handle first, core::Handle second) const {
  if (first == 0 || second == 0 || first == second)
    return true;
  if (!unit.input.options.strictAliasing)
    return true;
  QualType a = typeOfHandle(first);
  QualType b = typeOfHandle(second);
  if (a.isNull() || b.isNull())
    return true;
  if (a->isCharType() || b->isCharType() || a->isVoidType() || b->isVoidType())
    return true;
  if (a->isIncompleteType() || b->isIncompleteType())
    return true;
  if (context.typesAreCompatible(a.getUnqualifiedType(),
                                 b.getUnqualifiedType()))
    return true;
  // An object may contain the other as a member (a struct and its first
  // field): the aggregate may alias its members.
  auto contains = [&](QualType outer, QualType inner) {
    const RecordDecl *record = outer->getAsRecordDecl();
    if (record == nullptr || !record->isCompleteDefinition())
      return false;
    return llvm::any_of(record->fields(), [&](const FieldDecl *field) {
      QualType type = field->getType().getCanonicalType();
      if (context.typesAreCompatible(type.getUnqualifiedType(),
                                     inner.getUnqualifiedType()))
        return true;
      if (const auto *array = context.getAsArrayType(type);
          array != nullptr && context.typesAreCompatible(
                                  array->getElementType().getUnqualifiedType(),
                                  inner.getUnqualifiedType()))
        return true;
      // (Nested aggregates: conservatively yes.)
      return type->isRecordType() && type != outer.getCanonicalType();
    });
  };
  return contains(a, b) || contains(b, a);
}

/// The function an object of `type` initialised by `init` holds at byte
/// `offset`, when its initializer names one there.
static const FunctionDecl *initializedFunction(const ASTContext &context,
                                               QualType type, const Expr *init,
                                               std::int64_t offset) {
  for (int depth = 0; depth < 16 && init != nullptr && offset >= 0; ++depth) {
    init = init->IgnoreParens();
    type = type.getCanonicalType();
    const auto *list = dyn_cast<InitListExpr>(init);
    if (list != nullptr && list->isSyntacticForm() &&
        list->getSemanticForm() != nullptr)
      list = list->getSemanticForm();
    if (const auto *array = context.getAsConstantArrayType(type)) {
      if (list == nullptr)
        return nullptr;
      QualType element = array->getElementType();
      if (element->isIncompleteType())
        return nullptr;
      std::int64_t size = context.getTypeSizeInChars(element).getQuantity();
      if (size <= 0)
        return nullptr;
      auto index = static_cast<unsigned>(offset / size);
      if (index >= list->getNumInits())
        return nullptr;
      init = list->getInit(index);
      type = element;
      offset %= size;
      continue;
    }
    if (const RecordDecl *record = type->getAsRecordDecl()) {
      if (list == nullptr || record->isUnion() ||
          !record->isCompleteDefinition())
        return nullptr;
      const ASTRecordLayout &layout = context.getASTRecordLayout(record);
      const Expr *next = nullptr;
      for (const FieldDecl *field : record->fields()) {
        if (field->isBitField() ||
            field->getFieldIndex() >= list->getNumInits())
          continue;
        auto at = static_cast<std::int64_t>(
            layout.getFieldOffset(field->getFieldIndex()) /
            context.getCharWidth());
        std::int64_t size =
            field->getType()->isIncompleteType()
                ? 0
                : context.getTypeSizeInChars(field->getType()).getQuantity();
        if (offset >= at && offset < at + size) {
          next = list->getInit(field->getFieldIndex());
          type = field->getType();
          offset -= at;
          break;
        }
      }
      init = next;
      continue;
    }
    if (offset != 0 || list != nullptr)
      return nullptr;
    const Expr *designator = init->IgnoreParenCasts();
    if (const auto *address = dyn_cast<UnaryOperator>(designator);
        address != nullptr && address->getOpcode() == UO_AddrOf)
      designator = address->getSubExpr()->IgnoreParenCasts();
    if (const auto *ref = dyn_cast<DeclRefExpr>(designator))
      return dyn_cast<FunctionDecl>(ref->getDecl());
    return nullptr;
  }
  return nullptr;
}

core::Sym FunctionRun::unwritten(core::HeapState &state, core::ObjectId object,
                                 core::CellKey key,
                                 const core::SymInfo &hint) const {
  ++materialisations;
  // Bytes rewritten on some paths only: the value the cell would read
  // otherwise, or an unknown one.
  if (const core::ObjectState &weak = heap.ensure(state, object);
      !weak.forgets(key) && weak.mayForget(key)) {
    std::vector<std::pair<std::int64_t, std::int64_t>> ranges =
        weak.mayForgotten;
    state.objects.at(object).mayForgotten.clear();
    core::Sym plain = unwritten(state, object, key, hint);
    state.objects.at(object).mayForgotten = std::move(ranges);
    const core::SymInfo &known = heap.info(state, plain);
    core::SymInfo unknown;
    unknown.type = known.type;
    unknown.ctype = known.ctype;
    if (unknown.type == core::SymInfo::Type::Pointer) {
      core::ObjectId any = unknownObject();
      heap.ensure(state, any);
      unknown.targets = {core::Target{.object = any}};
    }
    core::Sym merged = heap.mergeWeak(state, plain, heap.fresh(state, unknown));
    if (!key.isSummary())
      state.objects.at(object).cells.set(key, merged);
    return merged;
  }
  const core::ObjectState &target = heap.ensure(state, object);
  const core::ObjectInfo &info = objects.info(object);
  QualType objectType = info.type != 0 ? typeOfHandle(info.type) : QualType();
  std::optional<FieldAt> at;
  if (!objectType.isNull())
    at = fieldAt(context, objectType,
                 key.isSelected() ? key.position().offset : key.offset);
  // (An element position is kept modulo its stride, so it may name another
  // member that shares it, `n` for `kids[j]` in `{int n; T *kids[4];}`: the
  // lvalue's type is the element's then.)
  if (at && !key.isConcrete() && hint.ctype != 0 &&
      !ASTContext::hasSameUnqualifiedType(at->type, typeOfHandle(hint.ctype)))
    at.reset();
  QualType cellType = at ? at->type : QualType();
  if (cellType.isNull() && hint.ctype != 0)
    cellType = typeOfHandle(hint.ctype);
  const FieldDecl *field = at ? at->field : nullptr;
  core::Sym value = core::ZeroSym;
  // A summary cell holds only what stores through unknown indices wrote
  // (§4.2 *Amendment (arrays)*): the value of an element no store reached
  // is returned, not stored.
  auto setCell = [&](core::Sym sym) {
    if (!key.isSummary())
      state.objects.at(object).cells.set(key, sym);
    return sym;
  };
  // Memory an unknown callee reached holds whatever it left there
  // (RFC 0030 §5.1), whatever the object's kind.
  if (target.forgets(key) && info.key.kind != core::ObjectKind::Focus) {
    core::SymInfo unknown;
    unknown.type = hint.type;
    if (unknown.type == core::SymInfo::Type::Unknown)
      unknown.type = !cellType.isNull() && cellType->isPointerType()
                         ? core::SymInfo::Type::Pointer
                         : core::SymInfo::Type::Int;
    unknown.ctype = !cellType.isNull() ? typeHandle(cellType) : hint.ctype;
    if (unknown.type == core::SymInfo::Type::Pointer) {
      core::ObjectId any = unknownObject();
      heap.ensure(state, any);
      unknown.targets = {core::Target{.object = any}};
    }
    return setCell(heap.fresh(state, unknown));
  }
  switch (info.key.kind) {
  case core::ObjectKind::Literal: {
    // A string literal's bytes (RFC 0012).
    const auto *expr = fromHandle<Expr>(info.key.handle);
    const auto *literal = dyn_cast_or_null<StringLiteral>(expr);
    if (const auto *predefined = dyn_cast_or_null<PredefinedExpr>(expr))
      literal = predefined->getFunctionName();
    if (literal != nullptr && literal->getCharByteWidth() == 1 &&
        !key.isSummary() && key.offset >= 0 &&
        std::cmp_less_equal(key.offset, literal->getByteLength()) &&
        !cellType.isNull() && cellType->isIntegerType() &&
        context.getTypeSizeInChars(cellType).getQuantity() == 1) {
      llvm::StringRef bytes = literal->getBytes();
      char byte = std::cmp_less(key.offset, bytes.size())
                      ? bytes[static_cast<std::size_t>(key.offset)]
                      : '\0';
      std::int64_t number = cellType->isUnsignedIntegerType()
                                ? static_cast<unsigned char>(byte)
                                : static_cast<signed char>(byte);
      core::SymInfo constant;
      constant.type = core::SymInfo::Type::Int;
      constant.intType = core::IntegerType{
          .width = 8, .isSigned = !cellType->isUnsignedIntegerType()};
      constant.ctype = typeHandle(cellType);
      core::Sym sym = heap.fresh(state, constant);
      state.zone.addRange(sym, number, number);
      return setCell(sym);
    }
    break;
  }
  case core::ObjectKind::Global: {
    // A function table the unit never writes holds its initializer's
    // targets, cell by cell.
    const auto *var = fromHandle<VarDecl>(info.key.handle);
    const VarDecl *initialized = nullptr;
    const Expr *init =
        var != nullptr ? var->getAnyInitializer(initialized) : nullptr;
    if (init != nullptr && key.isConcrete() && !cellType.isNull() &&
        cellType->isFunctionPointerType() && unit.keepsInitializer(*var))
      if (const FunctionDecl *fn =
              initializedFunction(context, var->getType(), init, key.offset)) {
        core::SymInfo known;
        known.type = core::SymInfo::Type::Function;
        known.functionsKnown = true;
        known.functions = {handleOf(fn->getCanonicalDecl())};
        known.name = fn->getNameAsString();
        // Its entry value (§6.2: no store).
        known.entryOf = std::make_pair(object, key);
        return setCell(heap.fresh(state, known));
      }
    break;
  }
  case core::ObjectKind::Focus: {
    // Every candidate's value at the cell.
    std::vector<core::ObjectId> focused = target.candidates;
    for (core::ObjectId candidate : focused) {
      if (!state.objects.contains(candidate))
        continue;
      core::Sym sym = heap.load(state, candidate, key, hint);
      value = value == core::ZeroSym ? sym : heap.mergeWeak(state, value, sym);
    }
    if (value == core::ZeroSym)
      value = cellType.isNull()
                  ? heap.fresh(state, core::SymInfo{.type = hint.type})
                  : entryValue(state, cellType, object, key, field);
    return setCell(value);
  }
  case core::ObjectKind::Local:
  case core::ObjectKind::HeapRecent:
  case core::ObjectKind::HeapOld:
    if (info.key.kind == core::ObjectKind::Local &&
        entryPathOf(objects, object, function, unit)) {
      // A parameter passed by value: its members are entry values.
      if (!cellType.isNull())
        return setCell(entryValue(state, cellType, object, key, field));
    }
    if (target.zeroed || info.key.kind == core::ObjectKind::Local ||
        target.uninitialised) {
      core::SymInfo zero;
      zero.type = hint.type;
      if (zero.type == core::SymInfo::Type::Unknown)
        zero.type = !cellType.isNull() && cellType->isPointerType()
                        ? core::SymInfo::Type::Pointer
                        : core::SymInfo::Type::Int;
      zero.ctype = !cellType.isNull() ? typeHandle(cellType) : hint.ctype;
      bool uninit = !target.zeroed;
      // RFC 0030 §5.4: in a function that calls `setjmp`, a `longjmp` may
      // return to it after a store this path does not see; its local reads
      // as possibly unassigned, not certainly.
      bool jumped =
          uninit && info.key.kind == core::ObjectKind::Local && callsSetjmp();
      // RFC 0033 §1: a local whose address is taken may be written where
      // no summary records it (a callee handed `&op_info`); that it was
      // never written is then no fact the program states, and it reads as
      // possibly unassigned.
      if (uninit && info.key.kind == core::ObjectKind::Local)
        if (const clang::VarDecl *var = localVariable(object);
            var != nullptr && isPassedToCall(*var))
          jumped = true;
      if (zero.type == core::SymInfo::Type::Pointer) {
        zero.null = core::PointerNull::Null;
        zero.uninit = uninit && !jumped;
        // RFC 0030 §11: null only because zero-initialisation lowered the
        // allocation; C leaves it garbage, so its use is checked, not a
        // definite null dereference.
        zero.mayUninit = (target.zeroed && target.uninitialised &&
                          info.key.kind != core::ObjectKind::Local) ||
                         jumped;
      } else {
        zero.uninit = uninit && !jumped;
        zero.mayUninit = jumped;
      }
      core::Sym sym = heap.fresh(state, zero);
      if (zero.type == core::SymInfo::Type::Int && !uninit)
        state.zone.addRange(sym, 0, 0);
      return setCell(sym);
    }
    [[fallthrough]];
  default:
    break;
  }
  // The cell's entry value, marked as such (§6.2: an unchanged cell is no
  // store).
  auto entryCell = [&](core::Sym sym) {
    if (key.isConcrete()) {
      core::SymInfo &entry = heap.infoMut(state, sym);
      entry.entryOf = std::make_pair(object, key);
      entry.entryOrigins = {*entry.entryOf};
    }
    return setCell(sym);
  };
  if (!cellType.isNull() &&
      (cellType->isPointerType() || cellType->isIntegralOrEnumerationType()))
    return entryCell(entryValue(state, cellType, object, key, field));
  core::SymInfo unknown;
  unknown.type = hint.type;
  unknown.ctype = hint.ctype;
  if (hint.type == core::SymInfo::Type::Pointer) {
    QualType pointee;
    if (hint.ctype != 0) {
      QualType hinted = typeOfHandle(hint.ctype);
      if (!hinted.isNull() && hinted->isPointerType())
        pointee = hinted->getPointeeType();
    }
    core::ObjectId child = childEntryObject(state, object, key, pointee, field);
    heap.ensure(state, child);
    unknown.targets = {core::Target{.object = child}};
  }
  return entryCell(heap.fresh(state, unknown));
}

std::vector<core::ObjectId>
FunctionRun::roots(const core::HeapState &state) const {
  std::vector<core::ObjectId> found;
  for (const auto &[id, object] : state.objects) {
    core::ObjectKind kind = objects.info(id).key.kind;
    if ((kind == core::ObjectKind::Local || kind == core::ObjectKind::Global) &&
        object.life != core::Life::Ended)
      found.push_back(id);
  }
  return found;
}

void FunctionRun::dropDeadLocals(core::HeapState &state, unsigned block) const {
  if (block >= liveInBlock.size() || state.unreachable)
    return;
  const llvm::BitVector &live = liveInBlock[block];
  std::vector<core::ObjectId> dead;
  for (const auto &[id, object] : state.objects) {
    const core::ObjectInfo &info = objects.info(id);
    if (info.key.kind != core::ObjectKind::Local || info.key.dead)
      continue;
    const VarDecl *var = variableOf(info);
    // (A parameter's holder and a fixed local are read at the exits.)
    if (var == nullptr || isa<ParmVarDecl>(var) ||
        std::ranges::find(fixedLocals, var) != fixedLocals.end())
      continue;
    auto index = localIndex.find(var->getCanonicalDecl());
    if (index == localIndex.end() || index->second >= live.size() ||
        live.test(index->second) || addressTaken.test(index->second))
      continue;
    // (Within its scope a check may still name it, as a witness.)
    auto scope = localScope.find(var->getCanonicalDecl());
    const SourceLocation at =
        block < blockLocation.size() ? blockLocation[block] : SourceLocation();
    if (scope == localScope.end() || at.isInvalid())
      continue;
    const SourceManager &sm = context.getSourceManager();
    bool inside = !sm.isBeforeInTranslationUnit(at, scope->second.getBegin()) &&
                  !sm.isBeforeInTranslationUnit(scope->second.getEnd(), at);
    if (inside)
      continue;
    dead.push_back(id);
  }
  for (core::ObjectId id : dead)
    state.objects.erase(id);
}

void FunctionRun::dropDeadValues(core::HeapState &state, unsigned block) const {
  if (block >= carriedLiveIn.size() || state.unreachable || state.exprs.empty())
    return;
  const llvm::SparseBitVector<> &live = carriedLiveIn[block];
  state.exprs.eraseIf([&](core::Handle handle, core::Sym) {
    auto index = carriedIndex.find(handle);
    return index != carriedIndex.end() && !live.test(index->second);
  });
}

bool FunctionRun::callsSetjmp() const {
  const SiteIndex::FunctionSites *own = sites().function(function);
  return own != nullptr && own->callsSetjmp;
}

const SiteIndex &FunctionRun::sites() const {
  return unit.authoritative.siteIndex();
}

bool FunctionRun::applies(core::SiteId id, core::Facet facet) const {
  return unit.authoritative.applies(id, facet);
}

//===----------------------------------------------------------------------===//
// Leaks (RFC 0007, RFC 0031 §5.8)
//===----------------------------------------------------------------------===//

bool FunctionRun::liveAfter(const Stmt &at, const VarDecl &var) const {
  auto index = localIndex.find(var.getCanonicalDecl());
  if (index == localIndex.end())
    return true;
  if (addressTaken.test(index->second))
    return true;
  auto live = liveAfterStmt.find(&at);
  if (live == liveAfterStmt.end())
    return true;
  return live->second.test(index->second);
}

bool FunctionRun::liveBefore(const Stmt &at, const VarDecl &var) const {
  auto index = localIndex.find(var.getCanonicalDecl());
  if (index == localIndex.end())
    return true;
  if (addressTaken.test(index->second))
    return true;
  auto live = liveBeforeStmt.find(&at);
  if (live == liveBeforeStmt.end())
    return true;
  return live->second.test(index->second);
}

void FunctionRun::checkLeaks(core::HeapState &state, const Stmt &at,
                             LeakPoint point, const std::string &released) {
  if (!publishing || state.unreachable)
    return;
  // RFC 0030 §3.4: not at a return from `main`.
  bool atExit = point == LeakPoint::Exit || point == LeakPoint::ExitEdge;
  // `main`'s frame lasts until the program exits (§3.4).
  if ((atExit || point == LeakPoint::Scope) && function.isMain())
    return;
  // Nor on a path that ends the program.
  if (currentBlockNoReturn)
    return;
  std::vector<core::ObjectId> rootIds;
  for (const auto &[id, object] : state.objects) {
    const core::ObjectInfo &info = objects.info(id);
    if (object.life == core::Life::Ended)
      continue;
    if (info.key.kind == core::ObjectKind::Global) {
      rootIds.push_back(id);
      continue;
    }
    if (info.key.kind != core::ObjectKind::Local)
      continue;
    const VarDecl *var = variableOf(info);
    if (var == nullptr) {
      // A compound literal lives to the end of its block; a call's record
      // result is a temporary of its statement, dead at an exit.
      if ((!atExit && point != LeakPoint::Scope) ||
          !(info.key.expression &&
            isa_and_nonnull<CallExpr>(fromHandle<Stmt>(info.key.handle))))
        rootIds.push_back(id);
      continue;
    }
    if (atExit)
      continue;
    if (point == LeakPoint::Scope) {
      rootIds.push_back(id);
      continue;
    }
    // `main`'s frame lasts until the program exits (§3.4).
    bool live = function.isMain() ||
                (point == LeakPoint::Statement ? liveBefore(at, *var)
                                               : liveAfter(at, *var));
    if (live)
      rootIds.push_back(id);
  }
  // Entry objects are the caller's: what they hold is escaped.
  for (const auto &[id, object] : state.objects) {
    core::ObjectKind kind = objects.info(id).key.kind;
    if ((kind == core::ObjectKind::Entry && !object.owned) ||
        kind == core::ObjectKind::EntrySummary ||
        kind == core::ObjectKind::CallResult)
      rootIds.push_back(id);
  }
  std::vector<core::ObjectId> reachable = heap.reachableObjects(state, rootIds);
  std::set<core::ObjectId> reached(reachable.begin(), reachable.end());
  std::vector<core::ObjectId> leaked;
  for (const auto &[id, object] : state.objects) {
    core::ObjectKind kind = objects.info(id).key.kind;
    bool candidate = kind == core::ObjectKind::HeapRecent ||
                     kind == core::ObjectKind::HeapOld ||
                     (kind == core::ObjectKind::Entry && object.owned);
    // (`alloca` storage is the frame's, RFC 0030 §8.2: it ends, never leaks.)
    if (!candidate || !object.owned || object.escaped ||
        object.life != core::Life::Live || reached.contains(id) ||
        object.family == core::StackFamily)
      continue;
    // Not made on this path (the test that selected its allocation failed).
    if (object.existsIf && state.syms.contains(object.existsIf->first))
      if (auto zero = isZeroValue(heap, state, object.existsIf->first);
          zero && *zero != object.existsIf->second)
        continue;
    leaked.push_back(id);
  }
  for (core::ObjectId id : leaked) {
    core::ObjectState &object = state.objects.at(id);
    object.owned = false;
    reportedLeaks.insert(id);
    // Reported at the last use of the value when there is one, else where
    // it is found dead.
    // A value found dead where a statement starts, or at a `return`, is
    // reported there; one lost at the end of a block or of the body at its
    // last use.
    const Stmt *where = &at;
    bool atStatement = point == LeakPoint::Statement ||
                       point == LeakPoint::Release ||
                       point == LeakPoint::ExitEdge ||
                       (point == LeakPoint::Exit && isa<ReturnStmt>(at));
    if (!atStatement && object.lastUse != 0 && point != LeakPoint::Scope)
      where = fromHandle<Stmt>(object.lastUse);
    // RFC 0007: once per path on which the resource is lost. The blocks of
    // the final pass start from the fixpoint's states, which never saw a
    // report: a loss in a block an earlier report's block reaches is the
    // same loss, found again.
    bool again = false;
    for (const auto &[other, block] : leakBlocks)
      again = again || (other == id && blockReaches(block, currentBlock));
    if (again)
      continue;
    leakBlocks.emplace_back(id, currentBlock);
    std::string name = object.holder;
    const core::ObjectInfo &info = objects.info(id);
    std::string message;
    if (name.empty()) {
      // RFC 0007: `result of '<f>' is leaked` at a discarded allocating call.
      std::string callee = info.name;
      if (const auto *call =
              dyn_cast_or_null<CallExpr>(fromHandle<Expr>(info.key.handle));
          call != nullptr && (info.key.kind == core::ObjectKind::HeapRecent ||
                              info.key.kind == core::ObjectKind::HeapOld)) {
        if (const FunctionDecl *direct = call->getDirectCallee())
          callee = direct->getNameAsString();
      }
      message = "result of '" + callee + "' is leaked";
    } else if (point == LeakPoint::Release && !released.empty() &&
               name != released) {
      message = ("'" + llvm::Twine(name) + "' is leaked when '" + released +
                 "' is freed")
                    .str();
    } else {
      message = "'" + name + "' is leaked";
    }
    core::Diagnostic diagnostic;
    diagnostic.id = core::diag::Leak;
    diagnostic.severity = core::Severity::Warning;
    diagnostic.message = std::move(message);
    // RFC 0007 notes: where the resource came from (a discarded result has
    // no holder to point at).
    if (auto owner = declaredOwner.find(id); owner != declaredOwner.end()) {
      if (!name.empty() && owner->second.second.isValid())
        diagnostic.addNote("'" + owner->second.first +
                               "' is declared WEAVEC_OWNED here",
                           owner->second.second);
    } else if (!name.empty() && info.created.isValid()) {
      if (info.key.kind == core::ObjectKind::Entry)
        diagnostic.addNote("'" + info.name + "' is declared WEAVEC_OWNED here",
                           info.created);
      else
        diagnostic.addNote("allocated here", info.created);
    }
    diagnostic.location = toCoreLocation(
        context.getSourceManager(),
        point == LeakPoint::Scope ? where->getEndLoc() : where->getBeginLoc());
    report(std::move(diagnostic), core::Certainty::Possible, where,
           std::nullopt);
  }
}

bool FunctionRun::blockReaches(unsigned from, unsigned to) const {
  if (from == to)
    return true;
  if (!cfg)
    return false;
  std::vector<const CFGBlock *> byId(cfg->getNumBlockIDs(), nullptr);
  for (const CFGBlock *block : *cfg)
    byId[block->getBlockID()] = block;
  std::vector<bool> seen(byId.size(), false);
  std::vector<unsigned> pending{from};
  while (!pending.empty()) {
    unsigned id = pending.back();
    pending.pop_back();
    if (id >= byId.size() || seen[id] || byId[id] == nullptr)
      continue;
    seen[id] = true;
    for (const CFGBlock::AdjacentBlock &succ : byId[id]->succs())
      if (const CFGBlock *next = succ.getReachableBlock()) {
        if (next->getBlockID() == to)
          return true;
        pending.push_back(next->getBlockID());
      }
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Reporting
//===----------------------------------------------------------------------===//

void FunctionRun::report(core::Diagnostic diagnostic, core::Certainty certainty,
                         const Stmt *site, std::optional<core::Facet> facet) {
  if (!publishing)
    return;
  // RFC 0034 §6.1: in the replay, a report of a candidate at its location
  // witnesses it on this path when definite, and counters it when not.
  if (witnessing) {
    for (Candidate &candidate : candidates)
      if (candidate.id == diagnostic.id &&
          candidate.where == diagnostic.location) {
        if (certainty == core::Certainty::Definite &&
            diagnostic.severity == core::Severity::Error)
          candidate.reportedHere = true;
        else
          candidate.otherwiseHere = true;
      }
    return;
  }
  auto key =
      std::make_pair(site, std::string(diagnostic.id) + diagnostic.message);
  if (!reported.insert(key).second)
    return;
  if (mode == RunMode::Authoritative && site != nullptr && facet &&
      certainty == core::Certainty::Definite &&
      diagnostic.severity == core::Severity::Error)
    candidates.push_back(Candidate{.site = site,
                                   .facet = *facet,
                                   .id = std::string(diagnostic.id),
                                   .where = diagnostic.location});
  out.report(std::move(diagnostic), certainty, site, facet);
}

/// RFC 0034 §6.1: the replay's bounds.
static constexpr unsigned ReplayPaths = 256;
static constexpr unsigned ReplayTransfers = 20000;
static constexpr unsigned ReplayLoopTurns = 2;

void FunctionRun::confirmCandidates() {
  if (candidates.empty() || mode != RunMode::Authoritative || !cfg)
    return;
  LedgerAdapter witness(context, sites(), {}, LedgerAdapter::Mode::Witness);
  // A decision of a candidate's facet at its site that is no violation:
  // this path reaches the site and does not fail it.
  witness.observer = [this](const Stmt &site, core::Facet facet,
                            const core::FacetDecision &decision) {
    if (decision.outcome == core::SiteOutcome::Violation)
      return;
    for (Candidate &candidate : candidates)
      if (candidate.site == &site && candidate.facet == facet)
        candidate.otherwiseHere = true;
  };
  publishTo = &witness;
  witnessing = true;
  publishing = true;
  struct Frame {
    unsigned block = 0;
    core::HeapState state;
    std::vector<std::uint8_t> turns;
  };
  std::vector<Frame> stack;
  const unsigned entry = cfg->getEntry().getBlockID();
  bool complete = entryStates[entry].has_value();
  if (complete)
    stack.push_back(
        Frame{.block = entry,
              .state = *entryStates[entry],
              .turns = std::vector<std::uint8_t>(cfg->getNumBlockIDs(), 0)});
  unsigned paths = 0;
  unsigned transfersLeft = ReplayTransfers;
  std::vector<const CFGBlock *> byId(cfg->getNumBlockIDs(), nullptr);
  for (const CFGBlock *block : *cfg)
    byId[block->getBlockID()] = block;
  while (!stack.empty()) {
    Frame frame = std::move(stack.back());
    stack.pop_back();
    const CFGBlock *block = byId[frame.block];
    if (block == nullptr)
      continue;
    // A loop taken more than twice on one path leaves it from its head's
    // fixpoint state, by its exits only; anything else repeated stops.
    bool leaveLoop = false;
    if (frame.turns[frame.block]++ >= ReplayLoopTurns) {
      if (!loopHead[frame.block] || !entryStates[frame.block])
        continue;
      frame.state = *entryStates[frame.block];
      leaveLoop = true;
    }
    if (transfersLeft-- == 0) {
      complete = false;
      break;
    }
    // (Each path reports for itself: what makes a finding once per run.)
    requirementViolated.clear();
    reportedUninit.clear();
    std::vector<std::pair<unsigned, core::HeapState>> outs;
    transferBlock(*block, frame.state, outs);
    // A definite report in this block witnesses its candidate whatever the
    // site's own decision says (a trusted facet still reports, and the
    // authoritative pass links the error to it); anything else counters.
    for (Candidate &candidate : candidates) {
      if (candidate.reportedHere)
        candidate.witnessed = true;
      else if (candidate.otherwiseHere)
        candidate.countered = true;
      candidate.reportedHere = candidate.otherwiseHere = false;
    }
    if (outs.empty()) {
      if (++paths > ReplayPaths) {
        complete = false;
        break;
      }
      continue;
    }
    for (auto &[succ, state] : llvm::reverse(outs)) {
      if (leaveLoop) {
        const auto body = loopBody.find(frame.block);
        if (body != loopBody.end() && body->second.test(succ))
          continue;
      }
      dropDeadLocals(state, succ);
      dropDeadValues(state, succ);
      stack.push_back(Frame{
          .block = succ, .state = std::move(state), .turns = frame.turns});
    }
  }
  witnessing = false;
  publishing = false;
  publishTo = &out;
  for (const Candidate &candidate : candidates)
    if (!complete || !candidate.witnessed || candidate.countered)
      out.unconfirm(*candidate.site, candidate.facet, candidate.id);
}

//===----------------------------------------------------------------------===//
// The CFG and the fixpoint
//===----------------------------------------------------------------------===//

void FunctionRun::buildCfg() {
  CFG::BuildOptions options;
  options.setAllAlwaysAdd();
  options.AddLifetime = true;
  options.AddImplicitDtors = false;
  options.AddScopes = false;
  options.PruneTriviallyFalseEdges = true;
  cfg = CFG::buildCFG(&function, function.getBody(), &context, options);
  // RFC 0034 §7.1: no pre-pass (they are quadratic) over a graph too large.
  if (!cfg || cfg->getNumBlockIDs() > MaxBlocks)
    return;
  entryStates.assign(cfg->getNumBlockIDs(), std::nullopt);
  visits.assign(cfg->getNumBlockIDs(), 0);
  joinsAt.assign(cfg->getNumBlockIDs(), 0);
  loopHead.assign(cfg->getNumBlockIDs(), false);
  // The arms of conditional operators.
  struct ArmCollector : public RecursiveASTVisitor<ArmCollector> {
    llvm::DenseMap<const Expr *, const Expr *> &arms;
    explicit ArmCollector(llvm::DenseMap<const Expr *, const Expr *> &out)
        : arms(out) {}
    // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method):
    // RecursiveASTVisitor's CRTP hooks are found by name.
    bool VisitAbstractConditionalOperator(AbstractConditionalOperator *op) {
      // (The CFG evaluates an arm's parentheses as the expression inside.)
      arms[op->getTrueExpr()->IgnoreParens()] = op;
      arms[op->getFalseExpr()->IgnoreParens()] = op;
      return true;
    }
    // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  };
  ArmCollector(arms).TraverseStmt(function.getBody());
  // RFC 0017: dimensions of a declaration with an initializer are taken
  // before the initializer runs, as CodeGen does.
  for (const CFGBlock *block : *cfg) {
    for (const CFGElement &element : *block) {
      auto stmt = element.getAs<CFGStmt>();
      const auto *decl = stmt ? dyn_cast<DeclStmt>(stmt->getStmt()) : nullptr;
      if (decl == nullptr)
        continue;
      for (const Decl *each : decl->decls()) {
        const auto *var = dyn_cast<VarDecl>(each);
        if (var == nullptr || var->getInit() == nullptr ||
            var->hasGlobalStorage() ||
            !var->getType()->isVariablyModifiedType())
          continue;
        // The initializer's nodes; the first of them the block evaluates.
        struct Nodes : RecursiveASTVisitor<Nodes> {
          llvm::DenseSet<const Stmt *> all;
          // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method):
          // RecursiveASTVisitor's CRTP hooks are found by name.
          bool VisitStmt(Stmt *s) {
            all.insert(s);
            return true;
          }
          // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
        } nodes;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): visitor API
        nodes.TraverseStmt(const_cast<Expr *>(var->getInit()));
        for (const CFGElement &candidate : *block)
          if (auto first = candidate.getAs<CFGStmt>();
              first && nodes.all.contains(first->getStmt())) {
            vlaCaptureBefore[first->getStmt()].push_back(var);
            vlaCapturedEarly.insert(var);
            break;
          }
      }
    }
  }
  // Which block evaluates each expression, and which values a later block
  // reads.
  for (const CFGBlock *block : *cfg)
    for (const CFGElement &element : *block)
      if (auto stmt = element.getAs<CFGStmt>())
        evaluatedIn[stmt->getStmt()] = block->getBlockID();
  for (const CFGBlock *block : *cfg) {
    auto noteChildren = [&](const Stmt *parent) {
      // (A conditional's value is the one its arm recorded under it, §2:
      // it reads neither its condition nor its arms.)
      if (isa<ConditionalOperator>(parent))
        return;
      for (const Stmt *child : parent->children()) {
        if (child == nullptr)
          continue;
        auto it = evaluatedIn.find(child);
        if (it != evaluatedIn.end() && it->second != block->getBlockID())
          if (const auto *expr = dyn_cast<Expr>(child)) {
            crossBlock.insert(expr);
            // (Spent once read, except an operand of a conditional or
            // logical operator, whose untaken side keeps its last value.)
            const auto *logical = dyn_cast<BinaryOperator>(parent);
            if (!isa<AbstractConditionalOperator>(parent) &&
                (logical == nullptr || !logical->isLogicalOp()))
              consumedBy[block->getBlockID()].push_back(handleOf(expr));
          }
      }
    };
    for (const CFGElement &element : *block)
      if (auto stmt = element.getAs<CFGStmt>())
        noteChildren(stmt->getStmt());
    // (An expression terminator, `?:` or `&&`, reads its operands; a
    // statement's only its condition, below.)
    if (const Stmt *terminator = block->getTerminatorStmt();
        terminator != nullptr && isa<Expr>(terminator))
      noteChildren(terminator);
    if (const Stmt *condition = block->getTerminatorCondition(false)) {
      auto it = evaluatedIn.find(condition);
      if (it != evaluatedIn.end() && it->second != block->getBlockID())
        if (const auto *conditionExpr = dyn_cast<Expr>(condition))
          crossBlock.insert(conditionExpr);
    }
  }
  // Liveness of the carried expression values (`dropDeadValues`): a block
  // reads the ones in its elements' subtrees, its expression terminator's
  // and its condition's (the transfer reads a carried value of any
  // expression evaluated elsewhere), and evaluating one again, or an arm
  // of a conditional (which records the operator's value), replaces it.
  {
    carriedIndex.clear();
    llvm::DenseMap<const Expr *, unsigned> index;
    auto track = [&](const Expr *expr) {
      if (index.try_emplace(expr, index.size()).second)
        carriedIndex.try_emplace(handleOf(expr), index.size() - 1);
    };
    for (const Expr *expr : crossBlock)
      track(expr);
    for (const auto &[arm, op] : arms)
      track(op);
    const unsigned blocks = cfg->getNumBlockIDs();
    // (Sparse: thousands of blocks carry thousands of short-lived values.)
    std::vector<llvm::SparseBitVector<>> use(blocks);
    std::vector<llvm::SparseBitVector<>> kill(blocks);
    for (const CFGBlock *block : *cfg) {
      llvm::SparseBitVector<> &reads = use[block->getBlockID()];
      llvm::SparseBitVector<> &writes = kill[block->getBlockID()];
      std::vector<const Stmt *> pending;
      for (const CFGElement &element : *block)
        if (auto stmt = element.getAs<CFGStmt>()) {
          pending.push_back(stmt->getStmt());
          if (const auto *expr = dyn_cast<Expr>(stmt->getStmt())) {
            if (auto it = index.find(expr); it != index.end())
              writes.set(it->second);
            if (auto arm = arms.find(expr); arm != arms.end())
              writes.set(index.find(arm->second)->second);
          }
        }
      // (A conditional's branch reads only its condition.)
      if (const Stmt *terminator = block->getTerminatorStmt();
          terminator != nullptr && isa<Expr>(terminator) &&
          !isa<AbstractConditionalOperator>(terminator))
        pending.push_back(terminator);
      if (const Stmt *condition = block->getTerminatorCondition(false))
        pending.push_back(condition);
      while (!pending.empty()) {
        const Stmt *stmt = pending.back();
        pending.pop_back();
        if (const auto *expr = dyn_cast<Expr>(stmt))
          if (auto it = index.find(expr); it != index.end())
            reads.set(it->second);
        // (A conditional reads only the value its arm recorded; an opaque
        // value, `a ?: b`'s, reads its source.)
        if (isa<AbstractConditionalOperator>(stmt))
          continue;
        if (const auto *opaque = dyn_cast<OpaqueValueExpr>(stmt))
          if (const Expr *source = opaque->getSourceExpr())
            pending.push_back(source);
        for (const Stmt *child : stmt->children())
          if (child != nullptr)
            pending.push_back(child);
      }
      if (const auto *conditional =
              dyn_cast_or_null<AbstractConditionalOperator>(
                  block->getTerminatorStmt()))
        if (auto it = index.find(conditional); it != index.end())
          writes.set(it->second);
    }
    carriedLiveIn.assign(blocks, llvm::SparseBitVector<>());
    std::vector<const CFGBlock *> postOrder(cfg->begin(), cfg->end());
    for (bool changed = true; changed;) {
      changed = false;
      for (const CFGBlock *block : postOrder) {
        const unsigned id = block->getBlockID();
        llvm::SparseBitVector<> live;
        for (const CFGBlock::AdjacentBlock &succ : block->succs())
          if (const CFGBlock *next = succ.getReachableBlock())
            live |= carriedLiveIn[next->getBlockID()];
        live.intersectWithComplement(kill[id]);
        live |= use[id];
        if (live != carriedLiveIn[id]) {
          carriedLiveIn[id] = std::move(live);
          changed = true;
        }
      }
    }
  }
  // Locals, which of them have their address taken, and statement-level
  // elements (for leaks).
  struct LocalCollector : public RecursiveASTVisitor<LocalCollector> {
    std::vector<const VarDecl *> vars;
    llvm::DenseSet<const VarDecl *> taken;
    // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method):
    // RecursiveASTVisitor's CRTP hooks are found by name.
    bool VisitVarDecl(VarDecl *var) {
      if (!var->hasGlobalStorage())
        vars.push_back(var);
      if (var->getType()->isArrayType() || var->getType()->isRecordType())
        taken.insert(var);
      return true;
    }
    bool VisitUnaryOperator(UnaryOperator *op) {
      if (op->getOpcode() == UO_AddrOf)
        if (const auto *ref =
                dyn_cast<DeclRefExpr>(op->getSubExpr()->IgnoreParens()))
          if (const auto *var = dyn_cast<VarDecl>(ref->getDecl()))
            taken.insert(var);
      return true;
    }
    // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  };
  LocalCollector locals;
  for (const ParmVarDecl *param : function.parameters())
    locals.vars.push_back(param);
  locals.TraverseStmt(function.getBody());
  for (const VarDecl *var : locals.vars)
    localIndex.try_emplace(var->getCanonicalDecl(), localIndex.size());
  addressTaken.resize(localIndex.size());
  for (const VarDecl *var : locals.taken)
    if (auto it = localIndex.find(var->getCanonicalDecl());
        it != localIndex.end())
      addressTaken.set(it->second);
  struct ParamWrites : public RecursiveASTVisitor<ParamWrites> {
    std::set<const VarDecl *> written;
    void note(const Expr *expr) {
      if (const auto *ref = dyn_cast<DeclRefExpr>(expr->IgnoreParenImpCasts()))
        if (const auto *var = dyn_cast<VarDecl>(ref->getDecl()))
          written.insert(var->getCanonicalDecl());
    }
    // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method):
    // RecursiveASTVisitor's CRTP hooks are found by name.
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
  };
  ParamWrites writes;
  writes.TraverseStmt(function.getBody());
  for (unsigned i = 0; i < function.getNumParams(); ++i)
    if (!writes.written.contains(function.getParamDecl(i)->getCanonicalDecl()))
      unmodifiedParams.insert(i);
  if (const auto *body = dyn_cast_or_null<CompoundStmt>(function.getBody()))
    for (const Stmt *stmt : body->body())
      if (const auto *decl = dyn_cast<DeclStmt>(stmt))
        for (const Decl *each : decl->decls())
          if (const auto *var = dyn_cast<VarDecl>(each);
              var != nullptr && var->hasLocalStorage() && var->hasInit() &&
              var->getType()->isPointerType() &&
              !writes.written.contains(var->getCanonicalDecl()))
            fixedLocals.push_back(var);
  ParentMap parents(function.getBody());
  // Where each block is, and each local's scope (`dropDeadLocals`).
  {
    const SourceManager &sm = context.getSourceManager();
    blockLocation.assign(cfg->getNumBlockIDs(), SourceLocation());
    for (const CFGBlock *block : *cfg) {
      SourceLocation at;
      for (const CFGElement &element : *block)
        if (auto stmt = element.getAs<CFGStmt>()) {
          at = stmt->getStmt()->getBeginLoc();
          break;
        }
      if (at.isInvalid())
        if (const Stmt *term = block->getTerminatorStmt())
          at = term->getBeginLoc();
      if (at.isValid())
        blockLocation[block->getBlockID()] = sm.getExpansionLoc(at);
    }
    class Declarations : public RecursiveASTVisitor<Declarations> {
    public:
      std::vector<const DeclStmt *> found;
      // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method):
      // RecursiveASTVisitor's CRTP hooks are found by name.
      bool VisitDeclStmt(DeclStmt *decl) {
        found.push_back(decl);
        return true;
      }
      // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
    } declarations;
    declarations.TraverseStmt(function.getBody());
    for (const DeclStmt *decl : declarations.found) {
      const Stmt *scope = parents.getParent(decl);
      while (scope != nullptr && !isa<CompoundStmt>(scope))
        scope = parents.getParent(scope);
      if (scope == nullptr)
        continue;
      for (const Decl *each : decl->decls())
        if (const auto *var = dyn_cast<VarDecl>(each))
          localScope[var->getCanonicalDecl()] =
              SourceRange(sm.getExpansionLoc(var->getLocation()),
                          sm.getExpansionLoc(scope->getEndLoc()));
    }
  }
  for (const CFGBlock *block : *cfg)
    for (const CFGElement &element : *block)
      if (auto stmt = element.getAs<CFGStmt>()) {
        const Stmt *parent = parents.getParent(stmt->getStmt());
        if (parent == nullptr ||
            (!isa<Expr>(parent) && !isa<DeclStmt>(parent) &&
             !isa<ReturnStmt>(parent)))
          statementLevel.insert(stmt->getStmt());
      }
  // Backward liveness of the locals, per element.
  {
    // (A sub-expression that is an element of its own is counted there.)
    llvm::DenseSet<const Stmt *> elements;
    for (const CFGBlock *block : *cfg)
      for (const CFGElement &element : *block)
        if (auto stmt = element.getAs<CFGStmt>())
          elements.insert(stmt->getStmt());
    auto width = localIndex.size();
    std::vector<llvm::BitVector> liveIn(cfg->getNumBlockIDs(),
                                        llvm::BitVector(width));
    // (To the fixpoint, blocks from the exit back: the live-in sets decide
    // which locals a block's state keeps, `dropDeadLocals`.)
    std::vector<const CFGBlock *> backward(cfg->begin(), cfg->end());
    std::ranges::sort(backward, [](const CFGBlock *a, const CFGBlock *b) {
      return a->getBlockID() < b->getBlockID();
    });
    bool changed = true;
    while (changed) {
      changed = false;
      for (const CFGBlock *block : backward) {
        llvm::BitVector current(width);
        for (const CFGBlock::AdjacentBlock &succ : block->succs())
          if (const CFGBlock *reachable = succ.getReachableBlock())
            current |= liveIn[reachable->getBlockID()];
        for (const CFGElement &element : llvm::reverse(*block)) {
          auto stmt = element.getAs<CFGStmt>();
          if (!stmt)
            continue;
          liveAfterStmt[stmt->getStmt()] = current;
          // Every local the element refers to, however deep (an element
          // may hold its operands: `ra->tt == 0`).
          auto refer = [&](const Stmt *root, auto &self) -> void {
            if (root == nullptr)
              return;
            if (const auto *ref = dyn_cast<DeclRefExpr>(root))
              if (const auto *var = dyn_cast<VarDecl>(ref->getDecl()))
                if (auto found = localIndex.find(var->getCanonicalDecl());
                    found != localIndex.end())
                  current.set(found->second);
            for (const Stmt *child : root->children())
              if (child != nullptr && !elements.contains(child))
                self(child, self);
          };
          // (A declaration makes fresh storage: nothing earlier reads it;
          // its initializer does.)
          if (const auto *decl = dyn_cast<DeclStmt>(stmt->getStmt())) {
            for (const Decl *each : decl->decls())
              if (const auto *var = dyn_cast<VarDecl>(each))
                if (auto found = localIndex.find(var->getCanonicalDecl());
                    found != localIndex.end())
                  current.reset(found->second);
            for (const Decl *each : decl->decls())
              if (const auto *var = dyn_cast<VarDecl>(each))
                refer(var->getInit(), refer);
          } else {
            refer(stmt->getStmt(), refer);
          }
          liveBeforeStmt[stmt->getStmt()] = current;
        }
        if (current != liveIn[block->getBlockID()]) {
          liveIn[block->getBlockID()] = current;
          changed = true;
        }
      }
    }
    liveInBlock = std::move(liveIn);
  }
  // Loop heads: targets of back edges in a depth-first order.
  std::vector<int> color(cfg->getNumBlockIDs(), 0);
  std::vector<std::pair<const CFGBlock *, CFGBlock::const_succ_iterator>> stack;
  const CFGBlock &entry = cfg->getEntry();
  stack.emplace_back(&entry, entry.succ_begin());
  color[entry.getBlockID()] = 1;
  while (!stack.empty()) {
    auto &[block, it] = stack.back();
    if (it == block->succ_end()) {
      color[block->getBlockID()] = 2;
      stack.pop_back();
      continue;
    }
    const CFGBlock *succ = it->getReachableBlock();
    ++it;
    if (succ == nullptr)
      continue;
    if (color[succ->getBlockID()] == 1) {
      loopHead[succ->getBlockID()] = true;
      backEdges.emplace(block->getBlockID(), succ->getBlockID());
    } else if (color[succ->getBlockID()] == 0) {
      color[succ->getBlockID()] = 1;
      stack.emplace_back(succ, succ->succ_begin());
    }
  }
  std::vector<const CFGBlock *> byId(cfg->getNumBlockIDs(), nullptr);
  for (const CFGBlock *block : *cfg)
    byId[block->getBlockID()] = block;
  for (auto [source, head] : backEdges) {
    auto [it, inserted] =
        loopBody.try_emplace(head, llvm::BitVector(cfg->getNumBlockIDs()));
    llvm::BitVector &body = it->second;
    body.set(head);
    std::vector<unsigned> pending;
    if (!body.test(source)) {
      body.set(source);
      pending.push_back(source);
    }
    while (!pending.empty()) {
      const CFGBlock *block = byId[pending.back()];
      pending.pop_back();
      if (block == nullptr)
        continue;
      for (const CFGBlock::AdjacentBlock &pred : block->preds())
        if (const CFGBlock *from = pred.getReachableBlock();
            from != nullptr && !body.test(from->getBlockID())) {
          body.set(from->getBlockID());
          pending.push_back(from->getBlockID());
        }
    }
  }
}

void FunctionRun::initialState(core::HeapState &state) {
  Transfer transfer(*this, state);
  // RFC 0030 §11: a local whose declaration a jump can bypass holds garbage
  // from entry: the declaration is where zero-initialisation would run.
  if (function.getBody() != nullptr)
    for (const VarDecl *var : bypassedDeclarations(*function.getBody()))
      heap.ensure(state, variableObject(*var)).uninitialised = true;
  // Integer parameters first, so a pointer's declared extent can name them.
  std::vector<core::Sym> paramValues(function.getNumParams(), core::ZeroSym);
  for (unsigned i = 0; i < function.getNumParams(); ++i) {
    const ParmVarDecl *param = function.getParamDecl(i);
    core::ObjectId holder = variableObject(*param);
    core::ObjectState &holderState = heap.ensure(state, holder);
    if (auto size = transfer.sizeOf(param->getType()))
      holderState.extent = core::Extent{.bytes = core::Term::of(*size),
                                        .cls = core::ExtentClass::Exact};
    QualType type = param->getType().getCanonicalType();
    if (type->isIntegralOrEnumerationType()) {
      paramValues[i] =
          entryValue(state, type, holder, core::CellKey{}, nullptr);
      if (aliasContext != nullptr && i < aliasContext->constants.size() &&
          aliasContext->constants[i]) {
        const std::int64_t constant = *aliasContext->constants[i];
        auto integer = transfer.integerType(type);
        // (An unsigned 64-bit value above `INT64_MAX` comes as its bits; the
        // zone cannot hold it, its interval does.)
        if (constant < 0 && integer && !integer->isSigned &&
            integer->width == 64)
          heap.infoMut(state, paramValues[i]).values =
              core::IntegerRange::singleton(core::IntegerValue::ofBits(
                  *integer, static_cast<std::uint64_t>(constant)));
        else
          state.zone.addRange(paramValues[i], constant, constant);
      }
      // C11 5.1.2.2.1: `argc` is non-negative.
      if (i == 0 && function.isMain())
        state.zone.addRange(paramValues[i], 0, std::nullopt);
      heap.write(state, holder, core::CellKey{}, paramValues[i], false);
    }
  }
  for (unsigned i = 0; i < function.getNumParams(); ++i) {
    const ParmVarDecl *param = function.getParamDecl(i);
    QualType type = param->getType().getCanonicalType();
    if (!type->isPointerType())
      continue;
    core::ObjectId holder = variableObject(*param);
    QualType pointee = type->getPointeeType();
    // §6.6: a parameter the caller made share another's object.
    if (aliasContext != nullptr && i < aliasContext->params.size() &&
        aliasContext->params[i].first != i) {
      auto [rep, offset] = aliasContext->params[i];
      core::ObjectId repHolder = variableObject(*function.getParamDecl(rep));
      if (auto repValue = heap.read(state, repHolder, core::CellKey{})) {
        if (offset == 0) {
          heap.write(state, holder, core::CellKey{}, *repValue, false);
          continue;
        }
        core::SymInfo shared = heap.info(state, *repValue);
        for (core::Target &target : shared.targets)
          target.offset = target.offset.plusConstant(offset);
        shared.name = param->getNameAsString();
        heap.write(state, holder, core::CellKey{}, heap.fresh(state, shared),
                   false);
        continue;
      }
    }
    core::SymInfo value;
    if (pointee->isFunctionType()) {
      value.type = core::SymInfo::Type::Function;
      value.functionsKnown = false;
      // §7 *Amendment (cross-unit contexts)*: the callbacks the caller
      // passes, this unit's by their declarations, another's by name.
      if (aliasContext != nullptr)
        for (const auto &[bound, names] : aliasContext->callbacks) {
          if (bound != i)
            continue;
          value.functionsKnown = true;
          for (const std::string &name : names) {
            if (const FunctionDecl *fn = unit.functionNamed(name))
              value.functions.push_back(handleOf(fn->getCanonicalDecl()));
            else
              value.foreignFunctions.push_back(name);
          }
        }
      heap.write(state, holder, core::CellKey{}, heap.fresh(state, value),
                 false);
      continue;
    }
    core::ObjectKey key;
    key.kind = core::ObjectKind::Entry;
    key.path = core::SummaryPath::param(i).deref();
    core::ObjectInfo info;
    info.type = pointee->isVoidType() ? 0 : typeHandle(pointee);
    info.name = param->getNameAsString();
    info.created =
        toCoreLocation(context.getSourceManager(), param->getLocation());
    core::ObjectId object = objects.intern(key, info);
    core::ObjectState &objectState = heap.ensure(state, object);
    bool nonnull = false;
    objectState.extent = paramExtent(i, paramValues, nonnull);
    // §6.6 *Amendment (numeric contexts)*: the integers the caller's object
    // holds are the entry values of its cells.
    if (aliasContext != nullptr)
      for (const auto &[owner, offset, number] : aliasContext->cells) {
        if (owner != i ||
            heap.read(state, object, core::CellKey{.offset = offset}))
          continue;
        core::Sym cell =
            unwritten(state, object, core::CellKey{.offset = offset},
                      core::SymInfo{.type = core::SymInfo::Type::Int});
        if (heap.info(state, cell).type == core::SymInfo::Type::Int)
          state.zone.addRange(cell, number, number);
      }
    // RFC 0002: an owned parameter is this function's to release.
    for (const ParmVarDecl *redecl : {param})
      if (getAnnotations(*redecl).owned) {
        heap.object(state, object).owned = true;
        heap.object(state, object).holder = param->getNameAsString();
        heap.object(state, object).family = std::string(core::HeapFamily);
      }
    value.type = core::SymInfo::Type::Pointer;
    value.targets = {core::Target{.object = object}};
    value.null =
        nonnull ? core::PointerNull::NonNull : core::PointerNull::Maybe;
    value.name = param->getNameAsString();
    value.ctype = typeHandle(type);
    // RFC 0004: a parameter declared `WEAVEC_RAW` is a raw origin.
    if (getAnnotations(*param).raw) {
      value.raw = true;
      value.rawAt =
          toCoreLocation(context.getSourceManager(), param->getLocation());
      value.rawOrigin = core::SymInfo::RawOrigin::Declared;
    }
    core::Sym sym = heap.fresh(state, value);
    heap.write(state, holder, core::CellKey{}, sym, false);
  }
}

/// §6.6: globals the caller made point into a parameter's object.
static void seedContextGlobals(FunctionRun &run, core::HeapState &state,
                               const AliasContext *alias,
                               const FunctionDecl &function) {
  if (alias == nullptr)
    return;
  core::Heap &heap = run.domain();
  // Argument cells that point into one object: the second holds the
  // first's value, shifted.
  auto pointee = [&](unsigned param) -> std::optional<core::ObjectId> {
    if (param >= function.getNumParams())
      return std::nullopt;
    auto held =
        heap.read(state, run.variableObject(*function.getParamDecl(param)),
                  core::CellKey{});
    if (!held)
      return std::nullopt;
    const core::SymInfo &value = heap.info(state, *held);
    if (value.type != core::SymInfo::Type::Pointer || value.targets.size() != 1)
      return std::nullopt;
    return value.targets.front().object;
  };
  for (const AliasContext::CellAlias &cell : alias->cellAliases) {
    auto object = pointee(cell.param);
    auto repObject = pointee(cell.repParam);
    if (!object || !repObject)
      continue;
    core::SymInfo hint;
    hint.type = core::SymInfo::Type::Pointer;
    core::CellKey repKey{.offset = cell.repCell};
    core::Sym repValue = core::ZeroSym;
    if (auto held = heap.read(state, *repObject, repKey))
      repValue = *held;
    else
      repValue = run.unwritten(state, *repObject, repKey, hint);
    core::SymInfo shared = heap.info(state, repValue);
    for (core::Target &target : shared.targets)
      target.offset = target.offset.plusConstant(cell.shift);
    heap.ensure(state, *object);
    heap.write(state, *object, core::CellKey{.offset = cell.cell},
               heap.fresh(state, shared), false);
  }
  // Globals that hold pointers into one object: the second's value is the
  // first's, shifted.
  for (const auto &[var, rep, offset] : alias->globalAliases) {
    core::ObjectId repObject = run.variableObject(*rep);
    heap.ensure(state, repObject);
    core::SymInfo hint;
    hint.type = core::SymInfo::Type::Pointer;
    hint.ctype = typeHandle(rep->getType());
    core::Sym repValue = core::ZeroSym;
    if (auto held = heap.read(state, repObject, core::CellKey{}))
      repValue = *held;
    else
      repValue = run.unwritten(state, repObject, core::CellKey{}, hint);
    core::SymInfo shared = heap.info(state, repValue);
    for (core::Target &target : shared.targets)
      target.offset = target.offset.plusConstant(offset);
    shared.name = var->getNameAsString();
    core::ObjectId global = run.variableObject(*var);
    heap.ensure(state, global);
    heap.write(state, global, core::CellKey{}, heap.fresh(state, shared),
               false);
  }
  for (const auto &[var, rep, offset] : alias->globals) {
    core::ObjectId repHolder = run.variableObject(*function.getParamDecl(rep));
    auto repValue = heap.read(state, repHolder, core::CellKey{});
    if (!repValue)
      continue;
    core::SymInfo shared = heap.info(state, *repValue);
    for (core::Target &target : shared.targets)
      target.offset = target.offset.plusConstant(offset);
    shared.name = var->getNameAsString();
    core::ObjectId global = run.variableObject(*var);
    heap.ensure(state, global);
    heap.write(state, global, core::CellKey{}, heap.fresh(state, shared),
               false);
  }
  // §6.6 *Amendment (numeric contexts)*: the integers the object a pointer
  // global points to holds.
  for (const auto &[var, offset, number] : alias->globalCells) {
    core::ObjectId global = run.variableObject(*var);
    heap.ensure(state, global);
    core::Sym pointer = core::ZeroSym;
    if (auto held = heap.read(state, global, core::CellKey{}))
      pointer = *held;
    else
      pointer =
          run.unwritten(state, global, core::CellKey{},
                        core::SymInfo{.type = core::SymInfo::Type::Pointer,
                                      .ctype = typeHandle(var->getType())});
    const core::SymInfo &info = heap.info(state, pointer);
    if (info.type != core::SymInfo::Type::Pointer || info.top ||
        info.targets.size() != 1 || !info.targets[0].offset.isConstant())
      continue;
    core::ObjectId object = info.targets[0].object;
    core::CellKey key{.offset = info.targets[0].offset.constant + offset};
    if (heap.read(state, object, key))
      continue;
    core::Sym cell = run.unwritten(
        state, object, key, core::SymInfo{.type = core::SymInfo::Type::Int});
    if (heap.info(state, cell).type == core::SymInfo::Type::Int)
      state.zone.addRange(cell, number, number);
  }
}

/// The pointer a branch condition tests against null (`p`, `!p`, `p ==
/// NULL`, `NULL != p`), for the note of a null finding on its null edge
/// (RFC 0008).
static const Expr *nullTested(const Expr *condition,
                              const ASTContext &context) {
  while (condition != nullptr) {
    condition = condition->IgnoreParenImpCasts();
    if (const auto *unary = dyn_cast<UnaryOperator>(condition);
        unary != nullptr && unary->getOpcode() == UO_LNot) {
      condition = unary->getSubExpr();
      continue;
    }
    if (const auto *binary = dyn_cast<BinaryOperator>(condition);
        binary != nullptr &&
        (binary->getOpcode() == BO_EQ || binary->getOpcode() == BO_NE)) {
      auto isNull = [&](const Expr *e) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): Clang API
        return e->isNullPointerConstant(const_cast<ASTContext &>(context),
                                        Expr::NPC_ValueDependentIsNotNull) !=
               Expr::NPCK_NotNull;
      };
      if (isNull(binary->getRHS()))
        condition = binary->getLHS();
      else if (isNull(binary->getLHS()))
        condition = binary->getRHS();
      else
        return nullptr;
      continue;
    }
    break;
  }
  return condition != nullptr && condition->getType()->isPointerType()
             ? condition
             : nullptr;
}

bool FunctionRun::transferBlock(
    const CFGBlock &block, core::HeapState state,
    std::vector<std::pair<unsigned, core::HeapState>> &outs) {
  currentBlock = block.getBlockID();
  currentBlockNoReturn = block.hasNoReturnElement();
  // §4.1: the operations a join kept are found again by value numbering.
  for (const auto &[sym, info] : state.syms)
    if (info.defined)
      operations[{info.defined->op, info.defined->left, info.defined->right,
                  info.defined->constant, info.ctype}] = sym;
  Transfer transfer(*this, state);
  for (const CFGElement &element : block) {
    transfer.element(element);
    if (state.unreachable)
      return true;
  }
  transfer.finishBlock(block);
  if (state.unreachable)
    return true;
  const auto *condition =
      dyn_cast_or_null<Expr>(block.getTerminatorCondition(false));
  // The block of a short-circuit operand branches on that operand (`b` of
  // `if (a || b)`), which is its last element, not on the whole `a || b`.
  if (const auto *logical = dyn_cast_or_null<BinaryOperator>(
          condition ? condition->IgnoreParens() : nullptr);
      logical != nullptr && logical->isLogicalOp())
    if (const Expr *last = block.getLastCondition();
        last != nullptr && last != logical)
      condition = last;
  core::Sym conditionSym = core::ZeroSym;
  // A `switch` refines its value on each edge by the labels (RFC 0017).
  const auto *switchStmt =
      dyn_cast_or_null<SwitchStmt>(block.getTerminatorStmt());
  if (switchStmt != nullptr) {
    if (condition != nullptr &&
        condition->getType()->isIntegralOrEnumerationType())
      conditionSym = transfer.conditionValue(*condition);
  } else if (condition != nullptr && block.succ_size() == 2) {
    conditionSym = transfer.conditionValue(*condition);
  }
  // RFC 0008: the pointer the condition tests, whose null edge gets the
  // test as the reason it may be null.
  core::Sym testedSym = core::ZeroSym;
  core::SourceLocation testedAt;
  if (conditionSym != core::ZeroSym)
    if (const Expr *tested = nullTested(condition, context)) {
      testedSym = transfer.valueOf(*tested);
      testedAt =
          toCoreLocation(context.getSourceManager(), tested->getBeginLoc());
    }
  // What dies with the block is leaked before the collection drops it.
  if (publishing) {
    const Stmt *last = nullptr;
    for (const CFGElement &element : block)
      if (auto stmt = element.getAs<CFGStmt>())
        last = stmt->getStmt();
    if (last != nullptr && !isa<ReturnStmt>(last)) {
      // The condition's value is still needed on the edges.
      core::HeapState probe = state;
      if (conditionSym != core::ZeroSym)
        probe.exprs.set(handleOf(&block), conditionSym);
      checkLeaks(probe, *last, LeakPoint::BlockEnd);
      for (core::ObjectId id : reportedLeaks)
        if (state.objects.contains(id))
          state.objects.at(id).owned = false;
    } else if (last == nullptr) {
      // Lifetimes that end here drop what only they held, reported at the
      // end of their scope.
      const Stmt *scope = function.getBody();
      for (const CFGElement &element : block)
        if (auto lifetime = element.getAs<CFGLifetimeEnds>())
          if (const Stmt *trigger = lifetime->getTriggerStmt()) {
            scope = trigger;
            break;
          }
      checkLeaks(state, *scope, LeakPoint::Scope);
    }
  }
  // The condition survives the collection (it is read on the edges).
  core::Handle pin = handleOf(&block);
  if (conditionSym != core::ZeroSym)
    state.exprs.set(pin, conditionSym);
  heap.collect(state, roots(state), nullptr);
  unsigned index = 0;
  for (const CFGBlock::AdjacentBlock &adjacent : block.succs()) {
    const CFGBlock *succ = adjacent.getReachableBlock();
    unsigned position = index++;
    if (succ == nullptr)
      continue;
    core::HeapState copy = state;
    if (conditionSym != core::ZeroSym && switchStmt != nullptr) {
      if (!Transfer::refineSwitchEdge(*this, copy, conditionSym,
                                      condition->getType(), *switchStmt, *succ,
                                      position + 1 == block.succ_size()))
        continue;
    } else if (conditionSym != core::ZeroSym &&
               !Transfer::refine(*this, copy, conditionSym, position == 0)) {
      continue;
    }
    if (testedSym != core::ZeroSym && copy.syms.contains(testedSym) &&
        state.syms.contains(testedSym)) {
      core::SymInfo &tested = heap.infoMut(copy, testedSym);
      if (tested.type == core::SymInfo::Type::Pointer &&
          tested.null == core::PointerNull::Null &&
          heap.info(state, testedSym).null != core::PointerNull::Null &&
          !tested.allocatorSource)
        tested.nullOrigin =
            core::NullOrigin{.reason = core::NullOrigin::Reason::Tested,
                             .where = testedAt,
                             .detail = {}};
    }
    copy.exprs.erase(pin);
    // A conditional's value is its arm's on this path: none an earlier
    // evaluation left (an arm that records none reads as unknown).
    if (const auto *conditional = dyn_cast_or_null<AbstractConditionalOperator>(
            block.getTerminatorStmt()))
      copy.exprs.erase(handleOf(conditional));
    // The values this block read from earlier blocks are spent (their
    // symbols need not stay).
    if (auto spent = consumedBy.find(block.getBlockID());
        spent != consumedBy.end())
      for (core::Handle handle : spent->second)
        copy.exprs.erase(handle);
    outs.emplace_back(succ->getBlockID(), std::move(copy));
  }
  return true;
}

namespace {
/// The integer constants of a body: its literals, `sizeof`s and array
/// sizes (in elements and bytes).
class ConstantCollector : public RecursiveASTVisitor<ConstantCollector> {
public:
  explicit ConstantCollector(const ASTContext &context) : context(context) {}
  std::set<std::int64_t> values;

  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method):
  // RecursiveASTVisitor's CRTP hooks are found by name.
  bool VisitIntegerLiteral(IntegerLiteral *literal) {
    if (literal->getValue().getActiveBits() <= 62)
      add(static_cast<std::int64_t>(literal->getValue().getZExtValue()));
    return true;
  }
  bool VisitUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *trait) {
    Expr::EvalResult value;
    if (!trait->isValueDependent() && trait->EvaluateAsInt(value, context))
      add(value.Val.getInt().getExtValue());
    return true;
  }
  bool VisitVarDecl(VarDecl *var) {
    if (const ConstantArrayType *array =
            context.getAsConstantArrayType(var->getType())) {
      add(static_cast<std::int64_t>(array->getSize().getZExtValue()));
      if (!var->getType()->isIncompleteType())
        add(static_cast<std::int64_t>(
            context.getTypeSizeInChars(var->getType()).getQuantity()));
    }
    return true;
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)

private:
  const ASTContext &context;
  // `i < n` bounds `i` by `n - 1`, and `i <= n` lets it reach `n + 1`.
  void add(std::int64_t value) {
    if (values.size() < 256)
      for (std::int64_t near : {value - 1, value, value + 1}) {
        values.insert(near);
        values.insert(-near);
      }
  }
};
} // namespace

std::vector<std::int64_t> FunctionRun::wideningThresholds(unsigned head) const {
  // §4.8: a bound that grew at a loop head stops at the nearest constant
  // the loop tests (`i < 3` keeps `i <= 3`), else goes to the type's
  // limits.
  ConstantCollector collector(context);
  collector.values = {-1, 0, 1};
  auto body = loopBody.find(head);
  if (body == loopBody.end()) {
    if (Stmt *statements = function.getBody())
      collector.TraverseStmt(statements);
  } else {
    for (const CFGBlock *block : *cfg)
      if (body->second.test(block->getBlockID()))
        if (const Stmt *condition = block->getTerminatorCondition(false))
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): visitor API
          collector.TraverseStmt(const_cast<Stmt *>(condition));
  }
  return {collector.values.begin(), collector.values.end()};
}

RunResult FunctionRun::run() {
  RunResult result;
  if (std::getenv("WEAVEC_ENGINE_TRACE") != nullptr)
    llvm::errs() << "run " << function.getNameAsString() << " mode "
                 << static_cast<int>(mode) << " depth " << contextDepth << "\n";
  if (mode == RunMode::Authoritative)
    validateAnnotations();
  buildCfg();
  if (!cfg || cfg->getNumBlockIDs() > MaxBlocks) {
    result.effects.incomplete = cfg ? "its graph is over budget" : "no CFG";
    overBudget = result.overBudget = true;
    result.spentBudget = spentBudget = cfg != nullptr;
    return result;
  }
  // Reverse post-order for the worklist priority.
  std::vector<unsigned> rpo;
  {
    std::vector<bool> seen(cfg->getNumBlockIDs(), false);
    std::vector<unsigned> post;
    std::vector<std::pair<const CFGBlock *, CFGBlock::const_succ_iterator>>
        stack;
    const CFGBlock &entry = cfg->getEntry();
    seen[entry.getBlockID()] = true;
    stack.emplace_back(&entry, entry.succ_begin());
    while (!stack.empty()) {
      auto &[block, it] = stack.back();
      if (it == block->succ_end()) {
        post.push_back(block->getBlockID());
        stack.pop_back();
        continue;
      }
      const CFGBlock *succ = it->getReachableBlock();
      ++it;
      if (succ != nullptr && !seen[succ->getBlockID()]) {
        seen[succ->getBlockID()] = true;
        stack.emplace_back(succ, succ->succ_begin());
      }
    }
    rpo.assign(post.rbegin(), post.rend());
  }
  std::vector<unsigned> rank(cfg->getNumBlockIDs(), ~0U);
  for (unsigned i = 0; i < rpo.size(); ++i)
    rank[rpo[i]] = i;
  std::map<unsigned, std::vector<std::int64_t>> thresholds;
  std::vector<const CFGBlock *> byId(cfg->getNumBlockIDs(), nullptr);
  for (const CFGBlock *block : *cfg)
    byId[block->getBlockID()] = block;
  order.clear();
  for (unsigned id : rpo)
    order.push_back(byId[id]);
  auto blockById = [&](unsigned id) { return byId[id]; };
  core::HeapState start;
  initialState(start);
  seedContextGlobals(*this, start, aliasContext, function);
  entryStates[cfg->getEntry().getBlockID()] = start;
  std::set<std::pair<unsigned, unsigned>> worklist; // (rank, block)
  worklist.emplace(rank[cfg->getEntry().getBlockID()],
                   cfg->getEntry().getBlockID());
  const std::uint64_t budget = unit.input.options.budget;
  // RFC 0033 §9: this run's share of what the unit has left.
  const std::uint64_t share = unit.runShare();
  static const bool Trace = std::getenv("WEAVEC_ENGINE_TRACE") != nullptr;
  auto thresholdsOf = [&](unsigned head) -> const std::vector<std::int64_t> & {
    auto [it, inserted] = thresholds.try_emplace(head);
    if (inserted)
      it->second = wideningThresholds(head);
    return it->second;
  };
  // §12: a loop head a back edge changed waits until nothing in its loop
  // is pending, so it takes in every back edge of the round at once rather
  // than restarting the round at each (a dispatch loop's many cases).
  std::vector<bool> changedByBackEdge(cfg->getNumBlockIDs(), false);
  auto nextBlock = [&] {
    for (auto candidate = worklist.begin(); candidate != worklist.end();
         ++candidate) {
      unsigned head = candidate->second;
      if (!changedByBackEdge[head])
        return candidate;
      const llvm::BitVector &body = loopBody.find(head)->second;
      bool bodyPending = false;
      for (auto other = std::next(candidate);
           other != worklist.end() && !bodyPending; ++other)
        bodyPending = body.test(other->second);
      if (!bodyPending)
        return candidate;
    }
    // (Irreducible loops whose bodies hold each other's heads.)
    return worklist.begin();
  };
  // RFC 0034 §7.1: work is the size of what a transfer or a join handles.
  const auto sizeOf = [](const core::HeapState &state) -> std::uint64_t {
    return 1 + state.syms.size() + state.objects.size();
  };
  const auto spend = [&](std::uint64_t amount) {
    work += amount;
    unit.unitWork += amount;
  };
  retained = sizeOf(start);
  while (!worklist.empty()) {
    auto next = nextBlock();
    unsigned id = next->second;
    worklist.erase(next);
    changedByBackEdge[id] = false;
    ++transfers;
    if ((budget != 0 && work > budget) || (share != 0 && work > share) ||
        retained > MaxRetainedState) {
      overBudget = true;
      spentBudget =
          (budget != 0 && work > budget) || retained > MaxRetainedState;
      break;
    }
    const CFGBlock *block = byId[id];
    if (block == nullptr || !entryStates[id])
      continue;
    spend(sizeOf(*entryStates[id]));
    std::vector<std::pair<unsigned, core::HeapState>> outs;
    transferBlock(*block, *entryStates[id], outs);
    if (Trace)
      llvm::errs() << "visit B" << id << " -> " << outs.size() << " outs\n";
    for (auto &[succ, state] : outs) {
      dropDeadLocals(state, succ);
      dropDeadValues(state, succ);
      if (Trace)
        llvm::errs() << "  to B" << succ
                     << (entryStates[succ] ? " join" : " first") << "\n"
                     << heap.dump(state);
      if (!entryStates[succ]) {
        retained += sizeOf(state);
        entryStates[succ] = std::move(state);
        worklist.emplace(rank[succ], succ);
        continue;
      }
      ++joins;
      // (A loop entered again from outside, with what an enclosing loop's
      // round changed: its budget counts from here.)
      if (loopHead[succ] && !backEdges.contains({id, succ})) {
        visits[succ] = 0;
        joinsAt[succ] = 0;
      }
      core::HeapState joined = heap.join(
          *entryStates[succ], state, handleOf(blockById(succ)), loopHead[succ]);
      spend(sizeOf(joined) + sizeOf(state));
      // Widening starts after a loop head's first joins; its budget counts
      // the joins that changed its state (another back edge of the same
      // round that adds nothing is none), and all its joins.
      if (loopHead[succ] && ++joinsAt[succ] > WidenAfter)
        joined = heap.widen(
            *entryStates[succ], joined, handleOf(blockById(succ)),
            visits[succ] > ThresholdRounds ? std::vector<std::int64_t>{}
                                           : thresholdsOf(succ));
      bool changed = !(joined == *entryStates[succ]);
      // (A settled loop head may come back renumbered: that is no change.)
      if (changed && loopHead[succ] &&
          heap.equivalent(joined, *entryStates[succ]))
        changed = false;
      if (loopHead[succ] && changed)
        ++visits[succ];
      const unsigned joinLimit = std::max<unsigned>(
          MaxJoinsPerBlock, JoinsPerPredecessor * blockById(succ)->pred_size());
      const unsigned visitLimit = std::max<unsigned>(
          MaxVisitsPerBlock,
          VisitsPerPredecessor * blockById(succ)->pred_size());
      if (visits[succ] > visitLimit || joinsAt[succ] > joinLimit ||
          joined.syms.size() > MaxStateSymbols) {
        overBudget = true;
        worklist.clear();
        break;
      }
      if (Trace)
        llvm::errs() << "  joined at B" << succ << "\n" << heap.dump(joined);
      if (changed) {
        retained += sizeOf(joined);
        retained -= std::min(retained, sizeOf(*entryStates[succ]));
        entryStates[succ] = std::move(joined);
        worklist.emplace(rank[succ], succ);
        if (backEdges.contains({id, succ}))
          changedByBackEdge[succ] = true;
      }
    }
  }
  result.transfers = transfers;
  result.work = work;
  // §12: what the run cost.
  if (core::AnalysisStats *stats = unit.input.options.stats) {
    stats->add("engine_runs");
    stats->add("block_transfers", transfers);
    stats->add("work", work);
    stats->add("joins", joins);
    stats->add("materialisations", materialisations);
    stats->atLeast("objects_max", objects.size());
    std::uint64_t zone = 0;
    for (const std::optional<core::HeapState> &entry : entryStates)
      if (entry)
        zone = std::max<std::uint64_t>(zone, entry->zone.symbols().size());
    stats->atLeast("zone_symbols_max", zone);
  }
  if (overBudget) {
    result.overBudget = true;
    result.spentBudget = spentBudget;
    result.effects.incomplete = "the analysis budget was exceeded";
    return result;
  }
  finalPass();
  // RFC 0030 §7.6: what the function hands out at its exits meets the
  // invariants (its own locals are gone).
  if (checkingInvariants)
    for (const core::HeapState &exit : exits)
      for (const auto &[id, object] : exit.objects)
        if (object.life == core::Life::Live &&
            objects.info(id).key.kind != core::ObjectKind::Local)
          checkInvariants(exit, id);
  if (const char *level = std::getenv("WEAVEC_ENGINE_DUMP");
      level != nullptr && std::string_view(level) == "3")
    dump(llvm::errs());
  result.effects = deriveEffects();
  // RFC 0034 §6.1: after the summary, so the replay changes nothing it
  // derives from.
  confirmCandidates();
  return result;
}

void FunctionRun::finalPass() {
  publishing = mode != RunMode::Summary;
  inFinalPass = true;
  exits.clear();
  requirementViolated.clear();
  // Blocks with no statements that reach the exit without a branch: an
  // edge into one leaves the function (the join after an `if` at the end of
  // the body).
  std::vector<std::optional<bool>> fallsToExit(cfg->getNumBlockIDs());
  auto emptyToExit = [&](auto &self, const CFGBlock *block, int depth) -> bool {
    if (block == nullptr || depth > 8)
      return false;
    if (block == &cfg->getExit())
      return true;
    std::optional<bool> &known = fallsToExit[block->getBlockID()];
    if (known)
      return *known;
    bool empty =
        block->getTerminatorStmt() == nullptr && block->succ_size() == 1;
    for (const CFGElement &element : *block)
      empty = empty && !element.getAs<CFGStmt>();
    known = empty && self(self, *block->succ_begin(), depth + 1);
    return *known;
  };
  // Every reachable block once, from its fixpoint entry state, in order.
  for (const CFGBlock *block : order) {
    if (!entryStates[block->getBlockID()])
      continue;
    std::vector<std::pair<unsigned, core::HeapState>> outs;
    core::HeapState state = *entryStates[block->getBlockID()];
    transferBlock(*block, state, outs);
    // Falling off the end of the body (a `return` noted its own exit).
    bool returns = false;
    for (const CFGElement &element : *block)
      if (auto stmt = element.getAs<CFGStmt>())
        returns = isa<ReturnStmt>(stmt->getStmt());
    for (auto &[succ, outState] : outs) {
      // RFC 0031 §5.8: an object this path leaves live that the join
      // ahead no longer shows live is leaked on this path, at the branch.
      const CFGBlock *next = nullptr;
      for (const CFGBlock::AdjacentBlock &adjacent : block->succs())
        if (adjacent && adjacent->getBlockID() == succ)
          next = adjacent;
      if (publishing && !returns && succ != cfg->getExit().getBlockID() &&
          block->getTerminatorStmt() != nullptr &&
          emptyToExit(emptyToExit, next, 0) && entryStates[succ]) {
        core::HeapState edge = outState;
        for (const auto &[id, object] : entryStates[succ]->objects)
          if (object.life == core::Life::Live && object.owned &&
              edge.objects.contains(id))
            edge.objects.at(id).escaped = true;
        checkLeaks(edge, *block->getTerminatorStmt(), LeakPoint::ExitEdge);
      }
      if (succ == cfg->getExit().getBlockID() && !returns) {
        Transfer transfer(*this, outState);
        if (publishing) {
          transfer.decideExitSite(*function.getBody(), false);
          transfer.exitLifetimes(*function.getBody(), nullptr);
        }
        checkLeaks(outState, *function.getBody(), LeakPoint::Exit);
        exits.push_back(outState);
      }
    }
  }
  publishing = false;
}

void FunctionRun::dump(llvm::raw_ostream &os) {
  os << "function " << function.getNameAsString() << "\n";
  if (!cfg) {
    os << "  (no CFG)\n";
    return;
  }
  for (const CFGBlock *block : *cfg) {
    os << " block B" << block->getBlockID() << "\n";
    if (entryStates[block->getBlockID()])
      os << heap.dump(*entryStates[block->getBlockID()]);
    else
      os << "  unreachable\n";
  }
}

} // namespace weavec::analysis::engine
