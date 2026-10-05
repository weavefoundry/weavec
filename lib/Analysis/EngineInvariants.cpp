//===- EngineInvariants.cpp - Counted-field invariants (RFC 0031) ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §7.6, restored by RFC 0031 *Implementation amendments*
// ("Counted-field invariants"): Houdini over `KindInference`'s candidates
// `count(d) == f + c` and `bytes(d) == f + c`. Every candidate is assumed
// at entry; a checking run of each function that writes `d` or `f`
// refutes the candidates an object it hands out (at a call, or at its
// exit) breaks; the survivors are exact extents of `d`, with which the
// functions that read `d` are analysed once more.
//
//===----------------------------------------------------------------------===//

#include "Engine.h"

#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"

using namespace clang;

namespace weavec::analysis::engine {

namespace {
/// Which of `fields` a body writes (an assignment, an increment, an
/// initializer of their record) and which it reads.
class FieldUses : public RecursiveASTVisitor<FieldUses> {
public:
  FieldUses(const std::set<const FieldDecl *> &fields,
            const std::set<const RecordDecl *> &records)
      : fields(fields), records(records) {}
  bool writes = false;
  bool reads = false;

  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method):
  // RecursiveASTVisitor's CRTP hooks are found by name.
  bool VisitBinaryOperator(BinaryOperator *op) {
    if (op->isAssignmentOp())
      noteWrite(op->getLHS());
    return true;
  }
  bool VisitUnaryOperator(UnaryOperator *op) {
    if (op->isIncrementDecrementOp())
      noteWrite(op->getSubExpr());
    return true;
  }
  bool VisitInitListExpr(InitListExpr *init) {
    if (const RecordDecl *record = init->getType()->getAsRecordDecl())
      writes = writes || records.contains(record->getDefinition());
    return true;
  }
  bool VisitMemberExpr(MemberExpr *member) {
    if (const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl()))
      reads = reads || fields.contains(field);
    return true;
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)

private:
  const std::set<const FieldDecl *> &fields;
  const std::set<const RecordDecl *> &records;
  void noteWrite(const Expr *lhs) {
    if (const auto *member = dyn_cast<MemberExpr>(lhs->IgnoreParenImpCasts()))
      if (const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl()))
        writes = writes || fields.contains(field);
  }
};
} // namespace

const KindEntry *UnitRun::fieldKind(const FieldDecl &field) const {
  const KindEntry *entry = input.kinds.field(field);
  if (entry != nullptr &&
      (core::hasExtent(entry->kind.shape) || entry->hasDeclaredShape()))
    return entry;
  auto assumed = assumedFields.find(&field);
  return assumed != assumedFields.end() ? &assumed->second : entry;
}

/// The byte offset of `field` in its record.
static std::int64_t fieldOffset(const ASTContext &context,
                                const FieldDecl &field) {
  const ASTRecordLayout &layout = context.getASTRecordLayout(field.getParent());
  return static_cast<std::int64_t>(
      layout.getFieldOffset(field.getFieldIndex()) / context.getCharWidth());
}

/// The element size a `count(d)` candidate counts in, or none.
static std::optional<std::int64_t> elementSize(const ASTContext &context,
                                               const FieldDecl &pointer) {
  QualType pointee = pointer.getType()->getPointeeType();
  if (pointee.isNull() || pointee->isIncompleteType() ||
      pointee->isVoidType() || pointee->isFunctionType())
    return std::nullopt;
  return static_cast<std::int64_t>(
      context.getTypeSizeInChars(pointee).getQuantity());
}

void FunctionRun::checkInvariants(core::HeapState state,
                                  core::ObjectId object) const {
  const core::ObjectInfo &info = objects.info(object);
  QualType type = info.type != 0 ? typeOfHandle(info.type) : QualType();
  const RecordDecl *record = type.isNull() ? nullptr : type->getAsRecordDecl();
  if (record == nullptr || record->getDefinition() == nullptr)
    return;
  record = record->getDefinition();
  for (const ResolvedCandidate *candidate : unit.standing) {
    if (candidate->record != record || unit.refuted.contains(candidate))
      continue;
    // `d` null holds whatever `f` is (the extent of no object); otherwise
    // `d` points to the start of one object whose extent is `f + c`
    // elements (or bytes).
    std::int64_t scale = 1;
    if (!candidate->candidate.bytes) {
      auto size = elementSize(context, *candidate->pointer);
      if (!size || *size <= 0) {
        unit.refuted.insert(candidate);
        continue;
      }
      scale = *size;
    }
    auto read = [&](const FieldDecl &field) {
      core::CellKey key{.offset = fieldOffset(context, field)};
      core::SymInfo hint;
      hint.type = field.getType()->isPointerType()
                      ? core::SymInfo::Type::Pointer
                      : core::SymInfo::Type::Int;
      if (auto held = heap.read(state, object, key))
        return *held;
      return unwritten(state, object, key, hint);
    };
    core::Sym pointer = read(*candidate->pointer);
    core::Sym count = read(*candidate->count);
    const core::SymInfo &value = heap.info(state, pointer);
    if (value.type == core::SymInfo::Type::Pointer &&
        value.null == core::PointerNull::Null)
      continue;
    // `d` as it was at entry: its extent is the assumed candidate's, so
    // only that one is decided, and only when `f` changed; another
    // candidate over a changed `f` cannot be shown.
    if (value.entryOf) {
      if (heap.info(state, count).entryOf)
        continue;
      auto assumed = unit.assumedCandidate.find(candidate->pointer);
      if (assumed == unit.assumedCandidate.end() ||
          assumed->second != candidate) {
        unit.refuted.insert(candidate);
        continue;
      }
    }
    bool holds = false;
    if (value.type == core::SymInfo::Type::Pointer && !value.top &&
        value.targets.size() == 1 &&
        value.targets[0].offset == core::Term::of(0)) {
      const core::ObjectState *target =
          heap.findObject(state, value.targets[0].object);
      if (target != nullptr && target->extent &&
          target->extent->cls == core::ExtentClass::Exact &&
          target->extent->bytes.known) {
        core::Term expected = core::Term::ofSym(
            count, scale, candidate->candidate.offset * scale);
        // Equal, or the size type's reduction of it (`malloc(n * sizeof
        // *d)` with `f = n`): what `f * sizeof *d` computes in C.
        holds =
            (heap.lessEqual(state, target->extent->bytes, expected) == true &&
             heap.lessEqual(state, expected, target->extent->bytes) == true) ||
            (target->extent->unwrapped && target->extent->unwrapped->known &&
             target->extent->unwrapped->var == count &&
             target->extent->unwrapped->scale == scale &&
             target->extent->unwrapped->constant ==
                 candidate->candidate.offset * scale);
      }
    }
    if (!holds)
      unit.refuted.insert(candidate);
    // (A value this function stored that meets it: a witness.)
    else if (!value.entryOf)
      unit.witnessed.insert(candidate);
  }
}

void UnitRun::inferInvariants(
    const std::vector<const FunctionDecl *> &definitions,
    const std::function<bool(const FunctionDecl &)> &shouldReport) {
  static constexpr int MaxRounds = 4;
  if (standing.empty())
    return;
  std::set<const FieldDecl *> fields;
  std::set<const FieldDecl *> pointers;
  std::set<const RecordDecl *> records;
  for (const ResolvedCandidate *candidate : standing) {
    fields.insert(candidate->pointer);
    fields.insert(candidate->count);
    pointers.insert(candidate->pointer);
    records.insert(candidate->record);
  }
  std::vector<const FunctionDecl *> writers;
  std::vector<const FunctionDecl *> readers;
  for (const FunctionDecl *fn : definitions) {
    FieldUses writes(fields, records);
    writes.TraverseStmt(fn->getBody());
    if (writes.writes)
      writers.push_back(fn);
    FieldUses reads(pointers, records);
    reads.TraverseStmt(fn->getBody());
    if (reads.reads)
      readers.push_back(fn);
  }
  if (readers.empty()) {
    standing.clear();
    return;
  }
  auto assume = [&] {
    assumedFields.clear();
    assumedCandidate.clear();
    for (const ResolvedCandidate *candidate : standing) {
      if (assumedFields.contains(candidate->pointer))
        continue;
      assumedCandidate.emplace(candidate->pointer, candidate);
      KindEntry entry;
      core::ExtentTerm extent = core::ExtentTerm::of(
          core::ExtentPath::ofField(candidate->count->getNameAsString()), 1,
          candidate->candidate.offset);
      entry.kind = candidate->candidate.bytes
                       ? core::PointerKind::sized(extent)
                       : core::PointerKind::counted(extent);
      entry.kind.source = core::KindSource::Inferred;
      entry.extentClass = core::ExtentClass::Exact;
      assumedFields.emplace(candidate->pointer, std::move(entry));
    }
  };
  bool settled = false;
  for (int round = 0; round < MaxRounds && !settled; ++round) {
    assume();
    refuted.clear();
    witnessed.clear();
    for (const FunctionDecl *fn : writers) {
      // (Every candidate refuted: the rest of the round decides nothing.)
      if (refuted.size() == standing.size())
        break;
      FunctionRun run(*this, *fn, discarding, RunMode::Summary);
      run.checkingInvariants = true;
      (void)run.run();
    }
    settled = refuted.empty();
    // A witness is a value the writer stored itself; with fewer
    // candidates assumed a later round finds no more of them, so a round
    // that witnesses none it keeps decides that none stands.
    if (std::ranges::none_of(standing, [&](const ResolvedCandidate *c) {
          return witnessed.contains(c) && !refuted.contains(c);
        })) {
      standing.clear();
      break;
    }
    std::erase_if(standing, [&](const ResolvedCandidate *candidate) {
      return refuted.contains(candidate);
    });
  }
  refuted.clear();
  // (Still refuting after the last round: none stands. One no function
  // establishes would only replace what the kinds give.)
  if (!settled)
    standing.clear();
  std::erase_if(standing, [&](const ResolvedCandidate *candidate) {
    return !witnessed.contains(candidate);
  });
  assume();
  if (assumedFields.empty())
    return;
  // The readers once more, their rows replacing the first run's; their
  // summaries stay what callers already used.
  for (const FunctionDecl *fn : readers) {
    if (!shouldReport(*fn))
      continue;
    authoritative.beginFunction(*fn);
    FunctionRun run(*this, *fn, authoritative, RunMode::Authoritative);
    if (run.run().overBudget)
      authoritative.overBudget(*fn);
  }
  assumedFields.clear();
}

} // namespace weavec::analysis::engine
