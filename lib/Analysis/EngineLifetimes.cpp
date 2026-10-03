//===- EngineLifetimes.cpp - Lifetimes and boundaries in the object engine
//-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0031 §5.6–§5.7 and RFC 0030 §6: what the frame leaves behind and what
// a boundary hands over.
//
// - Frame storage (a local, a parameter, a compound literal, an `alloca`
//   block) that a use reaches after its lifetime ended, or that a cell the
//   caller can reach still points to at the exit, is `lifetime-too-short`,
//   reported where the pointer was stored (RFC 0002, RFC 0011 *Deferred
//   lifetime checks*).
// - At a call and at an exit the engine publishes the boundary facts of
//   RFC 0030 §9.4 over the objects the other side can reach (§5.6).
// - `WEAVEC_ASSUME(e)` is an Assume site whose assertion facet is proven,
//   checked or contradicted, and the analysis assumes `e` after it
//   (RFC 0030 §6.2).
//
//===----------------------------------------------------------------------===//

#include "Engine.h"
#include "weavec/Analysis/Annotations.h"
#include "weavec/Analysis/BypassedDeclarations.h"
#include "weavec/Analysis/ClangLocation.h"

#include "clang/AST/RecordLayout.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

#include <deque>
#include <map>
#include <set>

using namespace clang;

namespace weavec::analysis::engine {

//===----------------------------------------------------------------------===//
// Frame storage
//===----------------------------------------------------------------------===//

bool FunctionRun::isFrameObject(const core::HeapState &state,
                                core::ObjectId id) const {
  const core::ObjectInfo &info = objects.info(id);
  switch (info.key.kind) {
  case core::ObjectKind::Local:
    // Locals, parameters, compound literals and temporaries: a variable
    // with static storage is a `Global` object.
    return !info.key.dead;
  case core::ObjectKind::HeapRecent:
  case core::ObjectKind::HeapOld:
    // RFC 0030 §8.2: `alloca` storage belongs to the frame.
    if (const core::ObjectState *object = state.objects.find(id))
      return object->family == core::StackFamily;
    return false;
  default:
    return false;
  }
}

std::string FunctionRun::frameName(core::ObjectId id) const {
  const core::ObjectInfo &info = objects.info(id);
  if (info.key.kind == core::ObjectKind::HeapRecent ||
      info.key.kind == core::ObjectKind::HeapOld)
    return "<alloca>";
  return info.name;
}

void FunctionRun::noteFrameStore(core::ObjectId holder, core::CellKey key,
                                 core::ObjectId frame,
                                 std::string holderSpelling) {
  if (currentElement == nullptr)
    return;
  frameStores[{holder, key, frame}] =
      FrameStore{.at = currentElement, .holder = std::move(holderSpelling)};
}

const FunctionRun::FrameStore *
FunctionRun::frameStore(core::ObjectId holder, core::CellKey key,
                        core::ObjectId frame) const {
  auto it = frameStores.find({holder, key, frame});
  return it == frameStores.end() ? nullptr : &it->second;
}

void FunctionRun::noteBorrowStore(core::ObjectId holder, core::CellKey key,
                                  std::string holderSpelling) {
  if (currentElement == nullptr)
    return;
  borrowStores[{holder, key}] =
      FrameStore{.at = currentElement, .holder = std::move(holderSpelling)};
}

const FunctionRun::FrameStore *
FunctionRun::borrowStore(core::ObjectId holder, core::CellKey key) const {
  auto it = borrowStores.find({holder, key});
  return it == borrowStores.end() ? nullptr : &it->second;
}

const VarDecl *FunctionRun::localVariable(core::ObjectId id) const {
  for (const auto &[decl, object] : localObjects)
    if (object == id)
      return dyn_cast<VarDecl>(decl);
  return nullptr;
}

bool FunctionRun::isAddressTaken(const VarDecl &var) const {
  auto index = localIndex.find(var.getCanonicalDecl());
  return index != localIndex.end() && addressTaken.test(index->second);
}

void Transfer::recordFrameStore(core::ObjectId holder, core::CellKey key,
                                core::Sym value, const Expr *at,
                                const std::string &holderName) {
  const core::SymInfo &info = heap.info(state, value);
  if (info.type != core::SymInfo::Type::Pointer)
    return;
  auto spelling = [&] {
    if (const auto *assign = dyn_cast_or_null<BinaryOperator>(at);
        assign != nullptr && assign->isAssignmentOp())
      return spell(*assign->getLHS());
    return holderName;
  };
  if (info.derived)
    run.noteBorrowStore(holder, key, spelling());
  std::vector<core::ObjectId> frames;
  for (const core::Target &target : info.targets)
    if (target.object != holder && run.isFrameObject(state, target.object))
      frames.push_back(target.object);
  if (frames.empty())
    return;
  std::string spelled = spelling();
  for (core::ObjectId frame : frames)
    run.noteFrameStore(holder, key, frame, spelled);
}

/// The byte offset of `field` in its record.
static std::int64_t offsetOf(const ASTContext &context,
                             const FieldDecl &field) {
  const ASTRecordLayout &layout = context.getASTRecordLayout(field.getParent());
  return static_cast<std::int64_t>(
      layout.getFieldOffset(field.getFieldIndex()) / context.getCharWidth());
}

/// The direct field of record type `type` at byte `offset`, if one starts
/// there.
static const FieldDecl *fieldStartingAt(const ASTContext &context,
                                        QualType type, std::int64_t offset) {
  if (type.isNull())
    return nullptr;
  const RecordDecl *record = type->getAsRecordDecl();
  if (record == nullptr || !record->isCompleteDefinition())
    return nullptr;
  for (const FieldDecl *field : record->fields())
    if (offsetOf(context, *field) == offset && !field->getName().empty())
      return field;
  return nullptr;
}

/// A cell's spelling for messages: `g`, `st->fs`, `*out`.
static std::string cellName(const FunctionRun &run, core::ObjectId object,
                            core::CellKey key) {
  const core::ObjectInfo &info = run.table().info(object);
  QualType type = info.type != 0 ? typeOfHandle(info.type) : QualType();
  const FieldDecl *field =
      key.isSummary() ? nullptr : fieldStartingAt(run.ast(), type, key.offset);
  if (info.key.kind == core::ObjectKind::Global) {
    std::string name = info.name;
    return field != nullptr ? name + "." + field->getNameAsString() : name;
  }
  if (field != nullptr)
    return info.name + "->" + field->getNameAsString();
  return "*" + info.name;
}

/// The innermost named field of `type` whose storage holds byte `offset`:
/// through the elements of arrays and the members of nested records. Null
/// when `type` is no record, or no named field holds the byte.
static const FieldDecl *fieldHolding(const ASTContext &context, QualType type,
                                     std::int64_t offset) {
  const FieldDecl *found = nullptr;
  for (unsigned depth = 0; depth < 32 && !type.isNull() && offset >= 0;
       ++depth) {
    if (const ArrayType *array = context.getAsArrayType(type)) {
      QualType element = array->getElementType();
      if (element->isIncompleteType() || !element->isConstantSizeType())
        break;
      std::int64_t size = context.getTypeSizeInChars(element).getQuantity();
      if (size <= 0)
        break;
      offset %= size;
      type = element;
      continue;
    }
    const RecordDecl *record = type->getAsRecordDecl();
    if (record == nullptr || !record->isCompleteDefinition() ||
        record->isInvalidDecl())
      break;
    const FieldDecl *holder = nullptr;
    for (const FieldDecl *field : record->fields()) {
      if (field->isBitField() || field->getType()->isIncompleteType() ||
          !field->getType()->isConstantSizeType())
        continue;
      std::int64_t start = offsetOf(context, *field);
      std::int64_t size =
          context.getTypeSizeInChars(field->getType()).getQuantity();
      if (offset >= start && offset < start + size) {
        holder = field;
        break;
      }
    }
    if (holder == nullptr)
      break;
    if (!holder->getName().empty() && !record->getName().empty())
      found = holder;
    offset -= offsetOf(context, *holder);
    type = holder->getType();
  }
  return found;
}

std::string cellClass(const FunctionRun &run, core::ObjectId object,
                      core::CellKey key) {
  const core::ObjectInfo &info = run.table().info(object);
  QualType type = info.type != 0 ? typeOfHandle(info.type) : QualType();
  // RFC 0030 §9.4: the class of a cell is the field that holds it, and for a
  // global outside any record the global. An element of an array is of its
  // array's class (a summary cell's offset is its position in an element):
  // a dangling pointer stored into `g[1]` or `r->slot[i]` breaks the entry
  // assumption of every load from that array.
  if (const FieldDecl *field = fieldHolding(run.ast(), type, key.offset)) {
    const RecordDecl *record = field->getParent();
    return record->getKindName().str() + " " + record->getNameAsString() + "." +
           field->getNameAsString();
  }
  if (info.key.kind == core::ObjectKind::Global)
    return info.name;
  return {};
}

/// RFC 0030 §9.4: whether a cell is an owning slot: a field or global some
/// function of the unit releases a value loaded from, or one declared
/// `WEAVEC_OWNED`.
static bool isOwningCell(const FunctionRun &run, core::ObjectId object,
                         core::CellKey key) {
  if (key.isSummary())
    return false;
  const core::ObjectInfo &info = run.table().info(object);
  QualType type = info.type != 0 ? typeOfHandle(info.type) : QualType();
  const Decl *slot = fieldStartingAt(run.ast(), type, key.offset);
  if (slot == nullptr && info.key.kind == core::ObjectKind::Global &&
      key.offset == 0)
    slot = fromHandle<VarDecl>(info.key.handle);
  if (slot == nullptr)
    return false;
  if (run.unitRun().owningSlots.contains(slot->getCanonicalDecl()))
    return true;
  if (const auto *named = dyn_cast<ValueDecl>(slot))
    return getAnnotations(*named).owned;
  return false;
}

/// The summary path of a cell of an object reached by `path`.
static core::SummaryPath cellPath(const FunctionRun &run,
                                  const core::SummaryPath &path,
                                  core::ObjectId object, core::CellKey key) {
  if (key.isSummary())
    return path.indexed();
  const core::ObjectInfo &info = run.table().info(object);
  QualType type = info.type != 0 ? typeOfHandle(info.type) : QualType();
  if (const FieldDecl *field = fieldStartingAt(run.ast(), type, key.offset))
    return path.field(field->getName());
  if (key.offset != 0)
    return path.field("#" + std::to_string(key.offset));
  return path;
}

void Transfer::danglingUse(const Expr &operand,
                           const core::TemporalVerdict &verdict,
                           const Stmt *site, bool definite) {
  if (!run.isPublishing() || verdict.object == 0)
    return;
  core::ObjectId frame = verdict.object;
  // Storage a callee left behind (`danglingValue`): its own exit reported
  // it.
  if (!run.isFrameObject(state, frame))
    return;
  // The cell the pointer was read from, and where it was put there.
  const FunctionRun::FrameStore *stored = nullptr;
  const Expr *lvalue = operand.IgnoreParenImpCasts();
  if (lvalue->isGLValue() && !lvalue->HasSideEffects(context)) {
    Address address = addressOf(*lvalue);
    if (!address.top && address.targets.size() == 1 &&
        address.targets[0].offset.isConstant())
      stored = run.frameStore(
          address.targets[0].object,
          core::CellKey{.offset = address.targets[0].offset.constant}, frame);
  }
  std::string holder = stored != nullptr && !stored->holder.empty()
                           ? stored->holder
                           : spell(operand);
  std::string name = run.frameName(frame);
  core::Diagnostic diagnostic;
  diagnostic.id = core::diag::LifetimeTooShort;
  diagnostic.severity =
      definite ? core::Severity::Error : core::Severity::Warning;
  diagnostic.message =
      "'" + holder + "' may outlive '" + name + "', which it points to";
  diagnostic.location = toCoreLocation(
      context.getSourceManager(),
      stored != nullptr ? stored->at->getBeginLoc() : operand.getBeginLoc());
  const core::ObjectInfo &info = run.table().info(frame);
  if (info.created.isValid())
    diagnostic.addNote("'" + name + "' is declared here", info.created);
  run.report(std::move(diagnostic),
             definite ? core::Certainty::Definite : core::Certainty::Possible,
             site, core::Facet::Temporal);
}

core::Sym Transfer::danglingValue(const CallExpr &call, QualType type,
                                  const core::SummaryPath &path) {
  QualType pointee = type->getPointeeType();
  std::string callee = call.getDirectCallee() != nullptr
                           ? call.getDirectCallee()->getNameAsString()
                           : spell(*call.getCallee());
  core::ObjectId object = run.allocationObject(
      call, pointee, "the storage of '" + callee + "'", path);
  core::ObjectState ended;
  ended.life = core::Life::MayEnded;
  state.objects.set(object, ended);
  core::SymInfo info;
  info.type = core::SymInfo::Type::Pointer;
  info.targets = {core::Target{.object = object}};
  info.null = core::PointerNull::NonNull;
  info.name = spell(call);
  info.ctype = typeHandle(type);
  return heap.fresh(state, info);
}

std::optional<std::string>
Transfer::spellArgumentPath(const CallExpr &call,
                            const core::SummaryPath &path) const {
  if (!path.isParam() || path.isRoot() || path.index >= call.getNumArgs())
    return std::nullopt;
  const Expr *arg = call.getArg(path.index)->IgnoreParenImpCasts();
  std::string text = spell(*arg);
  bool addressOf = false;
  if (const auto *unary = dyn_cast<UnaryOperator>(arg);
      unary != nullptr && unary->getOpcode() == UO_AddrOf) {
    text = spell(*unary->getSubExpr());
    addressOf = true;
  }
  if (text.empty())
    return std::nullopt;
  // Pairs of a dereference and a field.
  for (std::size_t i = 0; i + 1 < path.steps.size(); i += 2) {
    if (path.steps[i].step != core::PathStep::Deref ||
        path.steps[i + 1].step != core::PathStep::Field)
      return std::nullopt;
    text += (addressOf && i == 0 ? "." : "->") + path.steps[i + 1].field;
  }
  if (path.steps.size() % 2 != 0)
    return std::nullopt;
  return text;
}

void Transfer::calleeRelease(const CallExpr &call, core::Sym value,
                             const core::SummaryPath &path, bool certain) {
  if (!run.isPublishing())
    return;
  const core::SymInfo &info = heap.info(state, value);
  if (info.type != core::SymInfo::Type::Pointer || info.top ||
      info.targets.empty() || info.null == core::PointerNull::Null)
    return;
  // Storage that is no heap object on every path (RFC 0030 §3.4); a
  // position inside an object the caller did not allocate here is the
  // callee's business, as it may compose the offset back.
  std::optional<core::ObjectId> storage;
  bool every = true;
  for (const core::Target &target : info.targets) {
    const core::ObjectInfo &objectInfo = run.table().info(target.object);
    const core::ObjectState *object = heap.findObject(state, target.object);
    bool nonHeap = objectInfo.key.kind == core::ObjectKind::Local ||
                   objectInfo.key.kind == core::ObjectKind::Global ||
                   objectInfo.key.kind == core::ObjectKind::Literal ||
                   (object != nullptr && object->family == core::StackFamily);
    if (nonHeap && !storage)
      storage = target.object;
    every = every && nonHeap;
  }
  std::string subject =
      path.isParam() && path.isRoot() && path.index < call.getNumArgs()
          ? spell(*call.getArg(path.index))
          : std::string("the pointer");
  if (auto spelled = spellArgumentPath(call, path))
    subject = *spelled;
  if (!storage) {
    // RFC 0008: an allocation of this function released at a known
    // position past its start.
    if (info.targets.size() != 1)
      return;
    const core::Target &target = info.targets.front();
    const core::ObjectInfo &objectInfo = run.table().info(target.object);
    if ((objectInfo.key.kind != core::ObjectKind::HeapRecent &&
         objectInfo.key.kind != core::ObjectKind::HeapOld) ||
        !target.offset.isConstant() || target.offset.constant == 0)
      return;
    std::int64_t bytes = target.offset.constant;
    std::string position = std::to_string(bytes) + " bytes";
    if (path.isParam() && path.isRoot() && path.index < call.getNumArgs()) {
      QualType type = call.getArg(path.index)->IgnoreParenImpCasts()->getType();
      if (type->isPointerType())
        if (auto size = sizeOf(type->getPointeeType());
            size && *size > 0 && bytes % *size == 0)
          position = std::to_string(bytes / *size) +
                     (bytes / *size == 1 ? " element" : " elements");
    }
    bool definite = certain && objectInfo.singular;
    core::Diagnostic diagnostic;
    diagnostic.id = core::diag::InvalidRelease;
    diagnostic.severity =
        definite ? core::Severity::Error : core::Severity::Warning;
    diagnostic.message = "'" + subject + "' is released but " +
                         (definite ? "points " : "may point ") + position +
                         (bytes > 0 ? " past" : " before") +
                         " the start of its allocation";
    diagnostic.location =
        toCoreLocation(context.getSourceManager(), call.getBeginLoc());
    if (objectInfo.created.isValid())
      diagnostic.addNote("allocated here", objectInfo.created);
    run.report(std::move(diagnostic),
               definite ? core::Certainty::Definite : core::Certainty::Possible,
               &call, core::Facet::Spatial);
    return;
  }
  const core::ObjectInfo &objectInfo = run.table().info(*storage);
  bool literal = objectInfo.key.kind == core::ObjectKind::Literal;
  std::string name = objectInfo.key.kind == core::ObjectKind::Global
                         ? objectInfo.name
                         : run.frameName(*storage);
  bool definite = every && certain;
  std::string may = definite ? "" : "may ";
  core::Diagnostic diagnostic;
  diagnostic.id = core::diag::InvalidRelease;
  diagnostic.severity =
      definite ? core::Severity::Error : core::Severity::Warning;
  diagnostic.message =
      "'" + subject + "' is released but " + may +
      (definite ? "points" : "point") +
      (literal ? std::string(" to a string literal") +
                     (definite ? "" : ", which is not a heap object")
               : " to '" + name + "', which is not a heap object");
  diagnostic.location =
      toCoreLocation(context.getSourceManager(), call.getBeginLoc());
  if (!literal && objectInfo.created.isValid())
    diagnostic.addNote("'" + name + "' is declared here", objectInfo.created);
  run.report(std::move(diagnostic),
             definite ? core::Certainty::Definite : core::Certainty::Possible,
             &call, core::Facet::Spatial);
}

//===----------------------------------------------------------------------===//
// Boundaries (§5.6)
//===----------------------------------------------------------------------===//

namespace {
/// An object the other side of a boundary reaches, with how it names it.
struct Reached {
  core::ObjectId object = 0;
  core::SummaryPath path;
};
} // namespace

/// Every object reachable from `roots` through the memory, first path
/// first. Frame objects are entered only when `enterFrames`.
static std::vector<Reached> reachWithPaths(FunctionRun &run,
                                           const core::HeapState &state,
                                           std::vector<Reached> roots,
                                           bool enterFrames) {
  const core::Heap &heap = run.domain();
  std::set<core::ObjectId> seen;
  std::deque<Reached> work;
  std::vector<Reached> out;
  for (Reached &root : roots)
    if (seen.insert(root.object).second)
      work.push_back(std::move(root));
  while (!work.empty()) {
    Reached current = std::move(work.front());
    work.pop_front();
    const core::ObjectState *object = state.objects.find(current.object);
    if (object == nullptr)
      continue;
    out.push_back(current);
    if (object->life == core::Life::Released)
      continue;
    for (const auto &[key, sym] : object->cells) {
      const core::SymInfo &value = heap.info(state, sym);
      if (value.type != core::SymInfo::Type::Pointer)
        continue;
      for (const core::Target &target : value.targets) {
        if (!enterFrames && run.isFrameObject(state, target.object))
          continue;
        if (!seen.insert(target.object).second)
          continue;
        work.push_back(Reached{
            .object = target.object,
            .path = cellPath(run, current.path, current.object, key).deref()});
      }
    }
  }
  return out;
}

/// Whether `value` may hold a pointer the program released (not the
/// unknown-callee default, which is not a release the boundary can name),
/// and where it was released.
static std::optional<core::SourceLocation>
releasedValue(const core::Heap &heap, const core::HeapState &state,
              const core::SymInfo &value) {
  auto counts = [](const core::ReleaseRecord &record) {
    return !record.unknownOrigin() &&
           record.reason != core::ReleaseRecord::Reason::Moved;
  };
  if (value.null == core::PointerNull::Null)
    return std::nullopt;
  if (value.release && counts(*value.release))
    return value.release->where;
  for (const core::Target &target : value.targets) {
    const core::ObjectState *object = heap.findObject(state, target.object);
    if (object == nullptr)
      continue;
    if ((object->life == core::Life::Released ||
         object->life == core::Life::MayReleased) &&
        object->record && counts(*object->record))
      return object->record->where;
    if (object->life == core::Life::Ended ||
        object->life == core::Life::MayEnded)
      return core::SourceLocation{};
  }
  return std::nullopt;
}

void Transfer::callBoundary(const CallExpr &call,
                            const std::vector<core::Sym> &args) {
  if (!run.isPublishing() || state.unreachable)
    return;
  // §5.7: frame storage a global holds at a call is handed to the callee
  // through the global.
  for (const auto &[id, object] : state.objects) {
    if (run.table().info(id).key.kind != core::ObjectKind::Global)
      continue;
    for (const auto &[key, sym] : object.cells)
      for (const core::Target &target : heap.info(state, sym).targets)
        if (run.isFrameObject(state, target.object))
          run.exposedFrames.insert(target.object);
  }
  // RFC 0030 §9.4: the boundaries are the Call sites, the exits of calls
  // that do not return, and the library calls that hand an argument to a
  // callback.
  const SiteIndex &sites = run.ledger().siteIndex();
  bool boundary = false;
  for (core::SiteId id : sites.sitesOf(call))
    if (const SiteInfo *info = sites.info(id)) {
      if (info->boundary)
        boundary = true;
      if (info->library && info->library->entry->hasCallback())
        boundary = true;
      // RFC 0030 §2.1: the exit a call that does not return stands for.
      if (info->boundary == core::Boundary::Exit &&
          run.ledger().applies(info->id, core::Facet::Temporal))
        run.ledger().decideAs(call, info->kind, info->boundary,
                              core::Facet::Temporal,
                              core::FacetDecision::proven());
    }
  if (!boundary)
    return;
  UnitRun &unit = run.unitRun();
  std::vector<Reached> roots;
  for (unsigned i = 0; i < call.getNumArgs() && i < args.size(); ++i) {
    if (args[i] == core::ZeroSym)
      continue;
    const core::SymInfo &value = heap.info(state, args[i]);
    if (value.type != core::SymInfo::Type::Pointer)
      continue;
    for (const core::Target &target : value.targets)
      roots.push_back(Reached{.object = target.object,
                              .path = core::SummaryPath::param(i).deref()});
  }
  for (const auto &[id, object] : state.objects) {
    const core::ObjectInfo &info = run.table().info(id);
    if (info.key.kind != core::ObjectKind::Global)
      continue;
    const auto *var = fromHandle<VarDecl>(info.key.handle);
    if (var == nullptr)
      continue;
    roots.push_back(Reached{
        .object = id, .path = core::SummaryPath::global(unit.globalId(*var))});
  }
  BoundaryFacts facts;
  std::set<std::pair<core::SummaryPath, std::string>> seen;
  // Owning cells the callee reaches, for *owner uniqueness*.
  struct Owner {
    core::ObjectId holder;
    core::SummaryPath place;
    std::string placeClass;
    std::string name;
    core::Sym value;
    core::Target target;
  };
  std::vector<Owner> owners;
  for (const Reached &reached : reachWithPaths(run, state, roots, true)) {
    // (A holder that may be gone is its own temporal fact: its cells matter
    // only where it is live, as at an exit.)
    const core::ObjectState *object = state.objects.find(reached.object);
    if (object == nullptr || object->life != core::Life::Live)
      continue;
    for (const auto &[key, sym] : object->cells) {
      const core::SymInfo &value = heap.info(state, sym);
      if (value.type != core::SymInfo::Type::Pointer)
        continue;
      if (isOwningCell(run, reached.object, key) && !value.top &&
          value.targets.size() == 1 && value.null != core::PointerNull::Null &&
          run.table().info(value.targets[0].object).singular) {
        std::string placeClass = cellClass(run, reached.object, key);
        if (!placeClass.empty())
          owners.push_back(
              Owner{.holder = reached.object,
                    .place = cellPath(run, reached.path, reached.object, key),
                    .placeClass = std::move(placeClass),
                    .name = cellName(run, reached.object, key),
                    .value = sym,
                    .target = value.targets[0]});
      }
      auto released = releasedValue(heap, state, value);
      if (!released)
        continue;
      std::string placeClass = cellClass(run, reached.object, key);
      if (placeClass.empty())
        continue;
      core::SummaryPath place =
          cellPath(run, reached.path, reached.object, key);
      if (!seen.insert({place, placeClass}).second)
        continue;
      facts.dangling.push_back(
          BoundaryFacts::Dangling{.place = place,
                                  .released = *released,
                                  .placeClass = std::move(placeClass),
                                  .name = cellName(run, reached.object, key)});
    }
  }
  // *Owner uniqueness*: two owning places the other side can reach that
  // hold the same object; what it releases through one it releases
  // through the other.
  for (std::size_t i = 0; i < owners.size(); ++i)
    for (std::size_t j = i + 1; j < owners.size(); ++j) {
      const Owner &a = owners[i];
      const Owner &b = owners[j];
      if (a.place == b.place)
        continue;
      if (a.value != b.value && !(a.target == b.target))
        continue;
      facts.sharedOwners.push_back(
          BoundaryFacts::SharedOwners{.first = a.place,
                                      .second = b.place,
                                      .placeClass = a.placeClass,
                                      .otherClass = b.placeClass,
                                      .names = a.name + "' and '" + b.name});
    }
  // RFC 0031 §8, the owner forest: an owning cell whose object is
  // reachable, through owning cells, from the object it owns.
  std::map<core::ObjectId, std::vector<core::ObjectId>> owns;
  for (const Owner &owner : owners)
    owns[owner.holder].push_back(owner.target.object);
  for (const Owner &owner : owners) {
    std::set<core::ObjectId> seenObjects;
    std::vector<core::ObjectId> work{owner.target.object};
    bool cycle = false;
    while (!work.empty() && !cycle) {
      core::ObjectId id = work.back();
      work.pop_back();
      if (id == owner.holder) {
        cycle = true;
        break;
      }
      if (!seenObjects.insert(id).second)
        continue;
      if (auto it = owns.find(id); it != owns.end())
        work.insert(work.end(), it->second.begin(), it->second.end());
    }
    if (cycle)
      facts.sharedOwners.push_back(
          BoundaryFacts::SharedOwners{.first = owner.place,
                                      .second = owner.place,
                                      .placeClass = owner.placeClass,
                                      .otherClass = owner.placeClass,
                                      .names = owner.name,
                                      .cycle = true});
  }
  if (!facts.dangling.empty() || !facts.sharedOwners.empty())
    run.ledger().boundary(call, std::move(facts));
}

//===----------------------------------------------------------------------===//
// Exits (§5.7)
//===----------------------------------------------------------------------===//

/// Decides the temporal facet of the exit site of `exit`.
static void decideExit(const FunctionRun &run, const Stmt &exit,
                       const core::FacetDecision &decision) {
  const SiteIndex &sites = run.ledger().siteIndex();
  auto id = sites.findExit(exit);
  if (!id)
    return;
  const SiteInfo *info = sites.info(*id);
  if (info == nullptr || !run.ledger().applies(*id, core::Facet::Temporal))
    return;
  run.ledger().decideAs(*info->stmt, info->kind, info->boundary,
                        core::Facet::Temporal, decision);
}

void Transfer::exitLifetimes(const Stmt &exit, const ReturnStmt *ret) {
  if (!run.isPublishing() || state.unreachable)
    return;
  const SourceManager &sm = context.getSourceManager();
  auto lifetimeError = [&](const std::string &holder, core::ObjectId frame,
                           SourceLocation at, bool definite) {
    std::string name = run.frameName(frame);
    core::Diagnostic diagnostic;
    diagnostic.id = core::diag::LifetimeTooShort;
    diagnostic.severity =
        definite ? core::Severity::Error : core::Severity::Warning;
    diagnostic.message =
        "'" + holder + "' may outlive '" + name + "', which it points to";
    diagnostic.location = toCoreLocation(sm, at);
    const core::ObjectInfo &info = run.table().info(frame);
    if (info.created.isValid()) {
      diagnostic.addNote("'" + name + "' is declared here", info.created);
      // Where it goes out of scope: the end of the block that declares it,
      // or this exit for the function's own scope.
      SourceLocation end = isa<CompoundStmt>(exit)
                               ? cast<CompoundStmt>(exit).getRBracLoc()
                               : exit.getBeginLoc();
      if (const VarDecl *var = run.localVariable(frame)) {
        std::vector<std::pair<const Stmt *, const CompoundStmt *>> work{
            {run.decl().getBody(), nullptr}};
        while (!work.empty()) {
          auto [s, block] = work.back();
          work.pop_back();
          if (s == nullptr)
            continue;
          if (const auto *decls = dyn_cast<DeclStmt>(s)) {
            if (std::find(decls->decl_begin(), decls->decl_end(), var) !=
                    decls->decl_end() &&
                block != nullptr && block != run.decl().getBody())
              end = block->getRBracLoc();
            continue;
          }
          const auto *compound = dyn_cast<CompoundStmt>(s);
          for (const Stmt *child : s->children())
            work.emplace_back(child, compound != nullptr ? compound : block);
        }
      }
      if (end.isValid())
        diagnostic.addNote("'" + name + "' goes out of scope here",
                           toCoreLocation(sm, end));
    }
    decideExit(run, exit,
               definite ? core::FacetDecision::violation()
                        : core::FacetDecision::unresolvedFor(
                              core::UnresolvedReason::MayDangle));
    run.report(std::move(diagnostic),
               definite ? core::Certainty::Definite : core::Certainty::Possible,
               &exit, core::Facet::Temporal);
  };
  // The frame objects a value may point to, and whether it points to
  // nothing else.
  auto framesOf = [&](const core::SymInfo &value, bool &only) {
    std::vector<core::ObjectId> frames;
    only = !value.targets.empty() && !value.top &&
           value.null == core::PointerNull::NonNull;
    for (const core::Target &target : value.targets) {
      if (run.isFrameObject(state, target.object))
        frames.push_back(target.object);
      else
        only = false;
    }
    return frames;
  };
  // A record returned by value that holds a pointer to the frame.
  if (ret != nullptr && ret->getRetValue() != nullptr &&
      ret->getRetValue()->getType()->isRecordType()) {
    Address address = addressOf(*ret->getRetValue());
    for (const core::Target &target : address.targets) {
      const core::ObjectState *object = heap.findObject(state, target.object);
      if (object == nullptr)
        continue;
      for (const auto &[key, sym] : object->cells) {
        const core::SymInfo &value = heap.info(state, sym);
        if (value.type != core::SymInfo::Type::Pointer)
          continue;
        bool only = false;
        std::vector<core::ObjectId> frames = framesOf(value, only);
        if (frames.empty())
          continue;
        lifetimeError(spell(*ret->getRetValue()), frames.front(),
                      ret->getRetValue()->getBeginLoc(),
                      only && address.targets.size() == 1);
        break;
      }
    }
  }
  // Cells the caller can reach: below the parameters and the globals.
  std::vector<Reached> roots;
  UnitRun &unit = run.unitRun();
  for (const auto &[id, object] : state.objects) {
    const core::ObjectInfo &info = run.table().info(id);
    if (info.key.dead)
      continue;
    switch (info.key.kind) {
    case core::ObjectKind::Global:
      if (const auto *var = fromHandle<VarDecl>(info.key.handle))
        roots.push_back(
            Reached{.object = id,
                    .path = core::SummaryPath::global(unit.globalId(*var))});
      break;
    case core::ObjectKind::Entry:
    case core::ObjectKind::EntrySummary:
      roots.push_back(Reached{.object = id, .path = info.key.path});
      break;
    default:
      break;
    }
  }
  BoundaryFacts facts;
  for (const Reached &reached : reachWithPaths(run, state, roots, false)) {
    if (run.isFrameObject(state, reached.object))
      continue;
    const core::ObjectState *object = state.objects.find(reached.object);
    if (object == nullptr || object->life != core::Life::Live)
      continue;
    bool global =
        run.table().info(reached.object).key.kind == core::ObjectKind::Global;
    for (const auto &[key, sym] : object->cells) {
      const core::SymInfo &value = heap.info(state, sym);
      if (value.type != core::SymInfo::Type::Pointer)
        continue;
      bool only = false;
      std::vector<core::ObjectId> frames = framesOf(value, only);
      if (frames.empty())
        continue;
      core::ObjectId frame = frames.front();
      // RFC 0030 §9.4 amendment 1: storage whose lifetime ended, where the
      // caller finds it.
      std::string placeClass = cellClass(run, reached.object, key);
      if (!placeClass.empty())
        facts.dangling.push_back(BoundaryFacts::Dangling{
            .place = cellPath(run, reached.path, reached.object, key),
            .released = {},
            .placeClass = std::move(placeClass),
            .name = cellName(run, reached.object, key)});
      // §5.7: a global that held the storage at a call was handed to the
      // callee; what it holds at the exit is the boundary's fact only.
      if (global && run.exposedFrames.contains(frame))
        continue;
      const FunctionRun::FrameStore *stored =
          run.frameStore(reached.object, key, frame);
      std::string holder = stored != nullptr && !stored->holder.empty()
                               ? stored->holder
                               : cellName(run, reached.object, key);
      lifetimeError(holder, frame,
                    stored != nullptr ? stored->at->getBeginLoc()
                                      : exit.getBeginLoc(),
                    only);
    }
  }
  if (!facts.dangling.empty())
    run.ledger().boundary(exit, std::move(facts));
  // §5.7: storage of this frame left in objects the result reaches (a new
  // object the function returns) outlives the frame too.
  if (ret == nullptr || state.result == core::ZeroSym)
    return;
  const core::SymInfo &result = heap.info(state, state.result);
  if (result.type != core::SymInfo::Type::Pointer || result.top)
    return;
  std::set<core::ObjectId> seen;
  for (const Reached &reached : reachWithPaths(run, state, roots, false))
    seen.insert(reached.object);
  std::vector<Reached> fromResult;
  for (const core::Target &target : result.targets) {
    core::ObjectKind kind = run.table().info(target.object).key.kind;
    if ((kind == core::ObjectKind::HeapRecent ||
         kind == core::ObjectKind::HeapOld) &&
        !seen.contains(target.object))
      fromResult.push_back(
          Reached{.object = target.object,
                  .path = core::SummaryPath::result().deref()});
  }
  if (fromResult.empty())
    return;
  for (const Reached &reached : reachWithPaths(run, state, fromResult, false)) {
    if (seen.contains(reached.object) ||
        run.isFrameObject(state, reached.object))
      continue;
    const core::ObjectState *object = state.objects.find(reached.object);
    if (object == nullptr || object->life != core::Life::Live)
      continue;
    for (const auto &[key, sym] : object->cells) {
      const core::SymInfo &value = heap.info(state, sym);
      if (value.type != core::SymInfo::Type::Pointer)
        continue;
      bool only = false;
      std::vector<core::ObjectId> frames = framesOf(value, only);
      if (frames.empty())
        continue;
      const FunctionRun::FrameStore *stored =
          run.frameStore(reached.object, key, frames.front());
      std::string holder = stored != nullptr && !stored->holder.empty()
                               ? stored->holder
                               : cellName(run, reached.object, key);
      lifetimeError(holder, frames.front(),
                    stored != nullptr ? stored->at->getBeginLoc()
                                      : exit.getBeginLoc(),
                    only);
    }
  }
}

//===----------------------------------------------------------------------===//
// Raw pointers (RFC 0004)
//===----------------------------------------------------------------------===//

bool FunctionRun::inUnsafeRegion(const Stmt &stmt) const {
  if (const SiteIndex::FunctionSites *sites =
          out.siteIndex().function(function);
      sites != nullptr && sites->unsafe)
    return true;
  for (const FunctionDecl *redecl : function.redecls())
    if (getAnnotations(*redecl).unsafe)
      return true;
  if (!parentMap)
    parentMap = std::make_unique<ParentMap>(function.getBody());
  for (const Stmt *at = &stmt; at != nullptr; at = parentMap->getParent(at))
    if (isUnsafeBlock(*at))
      return true;
  return false;
}

bool FunctionRun::isBypassed(const Expr &operand) const {
  const auto *ref = dyn_cast<DeclRefExpr>(operand.IgnoreParenImpCasts());
  const auto *var =
      ref != nullptr ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
  if (var == nullptr || function.getBody() == nullptr)
    return false;
  if (!bypassed) {
    bypassed.emplace();
    for (const VarDecl *declared : bypassedDeclarations(*function.getBody()))
      bypassed->insert(declared->getCanonicalDecl());
  }
  return bypassed->contains(var->getCanonicalDecl());
}

core::Sym Transfer::launder(core::Sym value, const AnnotationSet &declared,
                            const std::string &target, const Expr &source) {
  if (!declared.safeKind())
    return value;
  const core::SymInfo info = heap.info(state, value);
  if (info.type != core::SymInfo::Type::Pointer || !info.raw)
    return value;
  if (run.isPublishing() && !run.inUnsafeRegion(source)) {
    const Expr *stripped = source.IgnoreParenImpCasts();
    std::string phrase = "raw pointer";
    std::string name;
    if (isa<DeclRefExpr>(stripped) || isa<MemberExpr>(stripped)) {
      name = spell(*stripped);
      phrase += " '" + name + "'";
    }
    std::string message =
        target.empty()
            ? phrase +
                  " is returned from a function whose return type is "
                  "annotated " +
                  macroSpelling(declared) + " outside an unsafe region"
            : phrase + " is assigned to '" + target + "', which is declared " +
                  macroSpelling(declared) + ", outside an unsafe region";
    core::Diagnostic diagnostic;
    diagnostic.id = core::diag::UnsafeOperation;
    diagnostic.severity = core::Severity::Error;
    diagnostic.message = std::move(message);
    const SourceManager &sm = context.getSourceManager();
    diagnostic.location = toCoreLocation(sm, source.getBeginLoc());
    if (info.rawAt.isValid())
      diagnostic.addNote((name.empty() ? std::string("the pointer is raw: ")
                                       : "'" + name + "' is raw: ") +
                             "cast from an integer here",
                         info.rawAt);
    diagnostic.addNote("move this operation into a WEAVEC_UNSAFE block or "
                       "function, or assert the pointer's ownership first",
                       toCoreLocation(sm, source.getBeginLoc()));
    run.report(std::move(diagnostic), core::Certainty::Definite, nullptr,
               std::nullopt);
  }
  // The assertion holds from here on either way: not asserting would only
  // cascade into a report per later use.
  core::SymInfo asserted = info;
  asserted.raw = false;
  asserted.pending.clear();
  asserted.condition.reset();
  return heap.fresh(state, asserted);
}

void Transfer::inlineAssembly(const GCCAsmStmt &assembly) {
  // RFC 0030 §5.7: the statement may have released, retained or replaced
  // what its pointer operands reach, as an unknown callee may.
  core::ReleaseRecord record;
  record.reason = core::ReleaseRecord::Reason::UnknownCallee;
  record.via = "inline assembly";
  record.where =
      toCoreLocation(context.getSourceManager(), assembly.getBeginLoc());
  record.allPaths = false;
  std::vector<core::ObjectId> start;
  auto operand = [&](const Expr *expr) {
    if (expr == nullptr || !expr->getType()->isPointerType())
      return;
    for (const core::Target &target : heap.info(state, valueOf(*expr)).targets)
      start.push_back(target.object);
  };
  for (const Expr *input : assembly.inputs())
    operand(input);
  for (const Expr *output : assembly.outputs()) {
    operand(output);
    // What an output operand holds afterwards is unknown.
    if (output != nullptr && output->isGLValue()) {
      Address address = addressOf(*output);
      store(address, unknownValue(output->getType()), output->getType(),
            nullptr);
    }
  }
  unknownEffect(start, record, /*mayOwn=*/false);
}

void Transfer::cleanupFunction(const VarDecl &var) {
  // RFC 0030 §5.1: `__attribute__((cleanup(f)))` calls `f(&var)` where the
  // variable's scope ends, at no call expression. Until that call is
  // analysed like any other, it is an unknown callee handed the variable:
  // it may have released, retained or replaced what the variable reaches.
  const auto *attribute = var.getAttr<CleanupAttr>();
  core::ReleaseRecord record;
  record.reason = core::ReleaseRecord::Reason::UnknownCallee;
  record.via = attribute != nullptr && attribute->getFunctionDecl() != nullptr
                   ? attribute->getFunctionDecl()->getNameAsString()
                   : std::string("a cleanup function");
  record.where = toCoreLocation(context.getSourceManager(), var.getLocation());
  record.allPaths = false;
  // The function was handed the last pointer to what the variable owns:
  // releasing it is its job, so nothing leaks here.
  unknownEffect({run.variableObject(var)}, record, /*mayOwn=*/true);
}

void Transfer::unknownEffect(const std::vector<core::ObjectId> &start,
                             const core::ReleaseRecord &record, bool mayOwn) {
  std::set<core::ObjectId> seen(start.begin(), start.end());
  std::vector<core::ObjectId> work = start;
  while (!work.empty()) {
    core::ObjectId id = work.back();
    work.pop_back();
    if (!state.objects.contains(id))
      continue;
    for (const auto &[key, sym] : state.objects.at(id).cells)
      for (const core::Target &target : heap.info(state, sym).targets)
        if (seen.insert(target.object).second)
          work.push_back(target.object);
  }
  for (core::ObjectId id : seen) {
    if (!state.objects.contains(id))
      continue;
    core::ObjectKind kind = run.table().info(id).key.kind;
    core::ObjectState &object = state.objects.at(id);
    object.cells = {};
    object.havocked = true;
    object.escaped = true;
    if (kind != core::ObjectKind::Local && kind != core::ObjectKind::Global &&
        kind != core::ObjectKind::Literal &&
        kind != core::ObjectKind::Function && object.life == core::Life::Live) {
      object.life = core::Life::UnknownReleased;
      object.record = record;
      if (mayOwn)
        object.owned = false;
    }
  }
}

//===----------------------------------------------------------------------===//
// Assumptions (RFC 0030 §6.2)
//===----------------------------------------------------------------------===//

/// `e` in `weavec_assume_((e) != 0)`.
static const Expr &assumedExpression(const Expr &argument) {
  const Expr *e = argument.IgnoreParenImpCasts();
  if (const auto *compare = dyn_cast<BinaryOperator>(e);
      compare != nullptr && compare->getOpcode() == BO_NE)
    if (const auto *zero =
            dyn_cast<IntegerLiteral>(compare->getRHS()->IgnoreParenImpCasts());
        zero != nullptr && zero->getValue() == 0)
      return *compare->getLHS()->IgnoreParens();
  return *e;
}

/// Refines `state` by `e` being `truth`; false when that is infeasible.
static bool assume(FunctionRun &run, core::HeapState &state, const Expr &e,
                   bool truth, int depth) {
  const Expr *x = e.IgnoreParens();
  if (depth > 16)
    return true;
  if (const auto *cast = dyn_cast<ImplicitCastExpr>(x)) {
    switch (cast->getCastKind()) {
    case CK_NoOp:
    case CK_IntegralCast:
    case CK_IntegralToBoolean:
    case CK_BooleanToSignedIntegral:
      return assume(run, state, *cast->getSubExpr(), truth, depth + 1);
    default:
      break;
    }
  }
  if (const auto *unary = dyn_cast<UnaryOperator>(x);
      unary != nullptr && unary->getOpcode() == UO_LNot)
    return assume(run, state, *unary->getSubExpr(), !truth, depth + 1);
  if (const auto *binary = dyn_cast<BinaryOperator>(x)) {
    BinaryOperatorKind op = binary->getOpcode();
    bool conjunction = (op == BO_LAnd && truth) || (op == BO_LOr && !truth);
    bool disjunction = (op == BO_LAnd && !truth) || (op == BO_LOr && truth);
    if (conjunction)
      return assume(run, state, *binary->getLHS(), truth, depth + 1) &&
             assume(run, state, *binary->getRHS(), truth, depth + 1);
    if (disjunction) {
      // `!a || (a && !b)` for `!(a && b)`; `a || (!a && b)` for `a || b`.
      bool first = op == BO_LOr;
      core::HeapState left = state;
      bool leftFeasible =
          assume(run, left, *binary->getLHS(), first, depth + 1);
      core::HeapState right = state;
      bool rightFeasible =
          assume(run, right, *binary->getLHS(), !first, depth + 1) &&
          assume(run, right, *binary->getRHS(), first, depth + 1);
      if (!leftFeasible && !rightFeasible)
        return false;
      if (leftFeasible && rightFeasible)
        state = run.domain().join(left, right, handleOf(binary));
      else
        state = leftFeasible ? std::move(left) : std::move(right);
      return true;
    }
    // `x != 0` and `x == 0` test `x`.
    if (op == BO_NE || op == BO_EQ)
      if (const auto *zero =
              dyn_cast<IntegerLiteral>(binary->getRHS()->IgnoreParenImpCasts());
          zero != nullptr && zero->getValue() == 0 &&
          !binary->getLHS()->getType()->isPointerType())
        return assume(run, state, *binary->getLHS(),
                      op == BO_NE ? truth : !truth, depth + 1);
    // A comparison over the operands' values here (a value carried from
    // another block has lost its condition at the join).
    if (binary->isComparisonOp() && !binary->HasSideEffects(run.ast())) {
      Transfer scratch(run, state);
      core::Sym condition = scratch.comparison(*binary);
      return Transfer::refine(run, state, condition, truth);
    }
  }
  // Any other expression: its value, evaluated in `state`.
  if (x->HasSideEffects(run.ast()))
    return true;
  Transfer scratch(run, state);
  core::Sym value = scratch.valueOf(*x);
  return Transfer::refine(run, state, value, truth);
}

bool Transfer::assumption(const CallExpr &call) {
  const FunctionDecl *callee = call.getDirectCallee();
  if (callee == nullptr || call.getNumArgs() != 1)
    return false;
  bool annotated = false;
  for (const FunctionDecl *redecl : callee->redecls())
    annotated = annotated || getAnnotations(*redecl).assume;
  if (!annotated)
    return false;
  const Expr &condition = *call.getArg(0);
  if (run.isPublishing()) {
    const SiteIndex &sites = run.ledger().siteIndex();
    std::optional<core::SiteId> id = sites.find(call, core::SiteKind::Assume);
    core::HeapState holds = state;
    bool canHold = assume(run, holds, condition, true, 0);
    core::HeapState fails = state;
    bool canFail = assume(run, fails, condition, false, 0);
    if (id && run.ledger().applies(*id, core::Facet::Assertion)) {
      core::FacetDecision decision = core::FacetDecision::checked();
      if (!canHold)
        decision = core::FacetDecision::violation();
      else if (!canFail)
        decision = core::FacetDecision::proven();
      run.ledger().decideAs(call, core::SiteKind::Assume, std::nullopt,
                            core::Facet::Assertion, decision);
    }
    if (!canHold) {
      const Expr &assumed = assumedExpression(condition);
      const SourceManager &sm = context.getSourceManager();
      CharSourceRange range = Lexer::makeFileCharRange(
          CharSourceRange::getTokenRange(assumed.getSourceRange()), sm,
          context.getLangOpts());
      std::string text =
          Lexer::getSourceText(range, sm, context.getLangOpts()).str();
      if (text.empty()) {
        llvm::raw_string_ostream os(text);
        assumed.printPretty(os, nullptr, context.getPrintingPolicy());
      }
      core::Diagnostic diagnostic;
      diagnostic.id = core::diag::ContradictedAssumption;
      diagnostic.severity = core::Severity::Error;
      diagnostic.message = "assumption '" + text + "' is false here";
      diagnostic.location = toCoreLocation(sm, call.getBeginLoc());
      // The facts that refute it, where the variable got its value.
      std::vector<const Expr *> operands{&assumed};
      std::set<const VarDecl *> noted;
      while (!operands.empty()) {
        const Expr *e = operands.back();
        operands.pop_back();
        if (const auto *ref = dyn_cast<DeclRefExpr>(e->IgnoreParenImpCasts()))
          if (const auto *var = dyn_cast<VarDecl>(ref->getDecl());
              var != nullptr && var->getType()->isIntegerType() &&
              noted.insert(var).second) {
            core::ObjectId object = run.variableObject(*var);
            if (auto held = heap.read(state, object, core::CellKey{}))
              if (auto value = state.zone.constant(*held))
                diagnostic.addNote("'" + var->getNameAsString() + "' is " +
                                       std::to_string(*value) + " here",
                                   toCoreLocation(sm, var->getLocation()));
          }
        for (const Stmt *child : e->children())
          if (const auto *operand = dyn_cast_or_null<Expr>(child))
            operands.push_back(operand);
      }
      run.report(std::move(diagnostic), core::Certainty::Definite, &call,
                 core::Facet::Assertion);
    }
  }
  // The analysis assumes `e` from here on, as on the true edge of `if (e)`:
  // sound in the enforcing modes, which trap first. A refuted assumption
  // ends the path.
  if (!assume(run, state, condition, true, 0))
    state.unreachable = true;
  return true;
}

} // namespace weavec::analysis::engine
