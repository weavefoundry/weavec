//===- EngineDecide.cpp - Site decisions in the object engine -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0031 §5.2–§5.3: each facet of each site `SiteCollector` enumerated is
// decided from the operand's value at the site, by RFC 0030 §3's tables,
// with RFC 0030's messages, and a checked facet gets a witness naming C
// places that hold the symbols its terms are over.
//
//===----------------------------------------------------------------------===//

#include "Engine.h"
#include "weavec/Analysis/ClangLocation.h"
#include "weavec/Analysis/KindTable.h"
#include "weavec/Analysis/SiteCollector.h"

#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecordLayout.h"
#include "clang/Basic/SourceManager.h"

#include <algorithm>
#include <functional>

using namespace clang;

namespace weavec::analysis::engine {

namespace {
core::Diagnostic makeDiagnostic(std::string_view id, std::string message,
                                const ASTContext &context, SourceLocation at,
                                core::Severity severity) {
  core::Diagnostic diagnostic;
  diagnostic.id = id;
  diagnostic.message = std::move(message);
  diagnostic.severity = severity;
  diagnostic.location = toCoreLocation(context.getSourceManager(), at);
  return diagnostic;
}

void addNote(core::Diagnostic &diagnostic, std::string message,
             const core::SourceLocation &at) {
  if (!at.isValid())
    return;
  diagnostic.addNote(std::move(message), at);
}

/// The object width of `type` (RFC 0030 §7.4): `sizeof`, or the offset of a
/// flexible trailing array member.
std::optional<std::int64_t> objectWidth(const ASTContext &context,
                                        QualType type) {
  if (type.isNull() || type->isIncompleteType() || type->isVoidType() ||
      type->isFunctionType())
    return type.isNull() || !type->isVoidType()
               ? std::nullopt
               : std::optional<std::int64_t>(1);
  auto size =
      static_cast<std::int64_t>(context.getTypeSizeInChars(type).getQuantity());
  if (const RecordDecl *record = type->getAsRecordDecl();
      record != nullptr && !record->isUnion() &&
      record->isCompleteDefinition()) {
    const FieldDecl *last = nullptr;
    for (const FieldDecl *field : record->fields())
      last = field;
    if (last != nullptr && (last->getType()->isIncompleteArrayType() ||
                            context.getAsConstantArrayType(last->getType()))) {
      bool flexible = last->getType()->isIncompleteArrayType();
      if (!flexible)
        flexible = context.getLangOpts().getStrictFlexArraysLevel() ==
                   LangOptions::StrictFlexArraysLevelKind::Default;
      if (flexible) {
        const ASTRecordLayout &layout = context.getASTRecordLayout(record);
        return static_cast<std::int64_t>(
            layout.getFieldOffset(last->getFieldIndex()) /
            context.getCharWidth());
      }
    }
  }
  return size;
}

std::string inQuotes(const std::string &name) {
  return "'" + name + "'";
}
} // namespace

/// The members that lead from the start of a `type` object to the integer
/// or pointer at `offset` (`inner.cap`); none through arrays, unions and
/// bit-fields (§5.3).
static std::optional<std::vector<std::string>>
memberPath(const ASTContext &context, QualType type, std::int64_t offset) {
  std::vector<std::string> names;
  type = type.getCanonicalType();
  for (int depth = 0; depth < 8; ++depth) {
    const RecordDecl *record = type->getAsRecordDecl();
    if (record == nullptr || record->isUnion() ||
        !record->isCompleteDefinition())
      return std::nullopt;
    const ASTRecordLayout &layout = context.getASTRecordLayout(record);
    const FieldDecl *found = nullptr;
    for (const FieldDecl *field : record->fields()) {
      if (field->isBitField() || field->getType()->isIncompleteType() ||
          field->getName().empty())
        continue;
      auto start = static_cast<std::int64_t>(
          layout.getFieldOffset(field->getFieldIndex()) /
          context.getCharWidth());
      auto size = static_cast<std::int64_t>(
          context.getTypeSizeInChars(field->getType()).getQuantity());
      if (offset >= start && offset < start + size) {
        found = field;
        offset -= start;
        break;
      }
    }
    if (found == nullptr)
      return std::nullopt;
    names.push_back(found->getNameAsString());
    type = found->getType().getCanonicalType();
    if (type->isRecordType())
      continue;
    if (offset == 0 && (type->isIntegerType() || type->isPointerType()))
      return names;
    return std::nullopt;
  }
  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// Witness terms (§5.3)
//===----------------------------------------------------------------------===//

std::string messageSpelling(const WitnessTerm &term) {
  if (term.kind != WitnessTerm::Kind::Place || term.decl == nullptr)
    return term.toString();
  std::string text = term.decl->getNameAsString();
  bool pendingDeref = false;
  for (const core::CheckPathStep &step : term.path) {
    if (step.kind == core::CheckPathStep::Kind::Deref) {
      if (pendingDeref)
        text = "*" + text;
      pendingDeref = true;
      continue;
    }
    text += (pendingDeref ? "->" : ".") + step.field;
    pendingDeref = false;
  }
  if (pendingDeref)
    text = "*" + text;
  return text;
}

std::optional<WitnessTerm> Transfer::nameOf(core::Sym sym, bool extent,
                                            int depth) const {
  if (sym == core::ZeroSym)
    return std::nullopt;
  if (auto c = state.zone.constant(sym))
    return WitnessTerm::ofConstant(*c);
  for (const auto &[id, object] : state.objects) {
    const core::ObjectInfo &info = run.table().info(id);
    if (info.key.kind != core::ObjectKind::Local &&
        info.key.kind != core::ObjectKind::Global)
      continue;
    if (object.life != core::Life::Live)
      continue;
    const VarDecl *var = variableOf(info);
    if (var == nullptr || var->getType().isVolatileQualified())
      continue;
    if (const core::Sym *held = object.cells.find(core::CellKey{}))
      if (*held == sym && !var->getType()->isRecordType() &&
          !var->getType()->isArrayType())
        return WitnessTerm::ofPlace(*var);
  }
  // A member of a local struct (`s.cap`), or of the object a local pointer
  // points to the start of (`b->cap`), whose cell holds the symbol. A read
  // through a pointer not known to be non-null is checked by the term
  // (RFC 0030 §10.3 rule 5).
  for (const auto &[id, object] : state.objects) {
    const core::ObjectInfo &holder = run.table().info(id);
    if (holder.key.kind != core::ObjectKind::Local ||
        object.life != core::Life::Live)
      continue;
    const VarDecl *var = variableOf(holder);
    if (var == nullptr || var->getType().isVolatileQualified() ||
        var->hasGlobalStorage())
      continue;
    QualType type = var->getType().getCanonicalType();
    if (type->isRecordType()) {
      for (const auto &[key, held] : object.cells)
        if (held == sym && !key.isSummary())
          if (auto members = memberPath(context, type, key.offset)) {
            std::vector<core::CheckPathStep> path;
            for (std::string &member : *members)
              path.push_back(core::CheckPathStep::member(std::move(member)));
            return WitnessTerm::ofPlace(*var, std::move(path));
          }
      continue;
    }
    if (!type->isPointerType())
      continue;
    const core::Sym *pointer = object.cells.find(core::CellKey{});
    if (pointer == nullptr)
      continue;
    const core::SymInfo &value = heap.info(state, *pointer);
    if (value.type != core::SymInfo::Type::Pointer || value.top ||
        value.targets.size() != 1 || !value.targets[0].offset.isConstant() ||
        value.targets[0].offset.constant != 0)
      continue;
    const core::ObjectState *target =
        heap.findObject(state, value.targets[0].object);
    if (target == nullptr || target->life != core::Life::Live)
      continue;
    for (const auto &[key, held] : target->cells)
      if (held == sym && !key.isSummary())
        if (auto members =
                memberPath(context, type->getPointeeType(), key.offset)) {
          std::vector<core::CheckPathStep> path;
          core::CheckPathStep deref = core::CheckPathStep::deref();
          // Checked by the term itself: the access's own `nonnull` check of
          // the pointer may run after the term reads through it (the
          // operands are unsequenced, RFC 0030 §10.3 amendment S6).
          deref.checked = true;
          path.push_back(deref);
          for (std::string &member : *members)
            path.push_back(core::CheckPathStep::member(std::move(member)));
          return WitnessTerm::ofPlace(*var, std::move(path));
        }
  }
  const core::SymInfo &info = heap.info(state, sym);
  if (info.linear && info.linear->var != sym && info.linear->known &&
      depth < 8) {
    auto base = nameOf(info.linear->var, extent, depth + 1);
    if (base) {
      WitnessTerm term = std::move(*base);
      if (info.linear->scale != 1)
        term = WitnessTerm::mul(std::move(term),
                                WitnessTerm::ofConstant(info.linear->scale));
      if (info.linear->constant > 0)
        term = WitnessTerm::add(std::move(term),
                                WitnessTerm::ofConstant(info.linear->constant));
      else if (info.linear->constant < 0)
        term = WitnessTerm::sub(
            std::move(term), WitnessTerm::ofConstant(-info.linear->constant));
      return term;
    }
  }
  // §5.3: the symbol's defining operation over nameable operands. A term
  // is computed in 64 bits by the prelude's helpers, which saturate an
  // extent that overflows to 0 (RFC 0030 §10.2), so a `size_t` sum or
  // product that may wrap is still a sound extent; a narrower one is spelled
  // only when it cannot wrap (§7.4 *Arithmetic*).
  if (extent && info.defined && info.intType && depth < 8) {
    const core::SymDefinition &definition = *info.defined;
    bool wide = !info.intType->isSigned && info.intType->width == 64 &&
                (definition.op == core::IntegerOp::Add ||
                 definition.op == core::IntegerOp::Subtract ||
                 definition.op == core::IntegerOp::Multiply);
    if (definition.exact || wide) {
      auto left = nameOf(definition.left, extent, depth + 1);
      std::optional<WitnessTerm> right;
      if (left)
        right =
            definition.constant
                ? std::optional(WitnessTerm::ofConstant(*definition.constant))
                : nameOf(definition.right, extent, depth + 1);
      if (left && right) {
        switch (definition.op) {
        case core::IntegerOp::Add:
          return WitnessTerm::add(std::move(*left), std::move(*right));
        case core::IntegerOp::Subtract:
          return WitnessTerm::sub(std::move(*left), std::move(*right));
        case core::IntegerOp::Multiply:
          return WitnessTerm::mul(std::move(*left), std::move(*right));
        case core::IntegerOp::Divide:
          if (right->kind == WitnessTerm::Kind::Constant && right->constant > 0)
            return WitnessTerm::div(std::move(*left), right->constant);
          break;
        default:
          break;
        }
      }
    }
  }
  return std::nullopt;
}

std::optional<std::string> Transfer::spellAmount(core::Term bytes,
                                                 bool own) const {
  if (!bytes.known)
    return std::nullopt;
  // A constant added to a named value reads as `'n' + 1` (in messages the
  // sum is the mathematical one).
  for (int depth = 0; depth < 8 && !bytes.isConstant(); ++depth) {
    const core::SymInfo &info = heap.info(state, bytes.var);
    if (!info.name.empty() || nameOf(bytes.var).has_value() || !info.defined ||
        !info.defined->constant ||
        (info.defined->op != core::IntegerOp::Add &&
         info.defined->op != core::IntegerOp::Subtract))
      break;
    std::int64_t step = info.defined->op == core::IntegerOp::Add
                            ? *info.defined->constant
                            : -*info.defined->constant;
    bytes = core::Term::ofSym(info.defined->left, bytes.scale,
                              bytes.constant + bytes.scale * step);
  }
  if (bytes.isConstant())
    return std::to_string(bytes.constant) + " bytes";
  // A need prefers the name its value was made under (`strlen(s)`), an
  // extent the C place that holds it.
  const std::string &made = heap.info(state, bytes.var).name;
  std::string spelled;
  if (own && !made.empty())
    spelled = made;
  else if (auto name = nameOf(bytes.var, /*extent=*/true))
    spelled = messageSpelling(*name);
  else
    spelled = made;
  if (spelled.empty())
    return std::nullopt;
  std::string text = "'" + spelled + "'";
  if (bytes.scale != 1)
    text += " * " + std::to_string(bytes.scale);
  if (bytes.constant > 0)
    text += " + " + std::to_string(bytes.constant);
  else if (bytes.constant < 0)
    text += " - " + std::to_string(-bytes.constant);
  return text + " bytes";
}

/// `bytes` as a C term.
static std::optional<WitnessTerm> bytesTerm(const Transfer &transfer,
                                            const core::Term &bytes) {
  if (!bytes.known)
    return std::nullopt;
  if (bytes.isConstant())
    return bytes.constant >= 0
               ? std::optional(WitnessTerm::ofConstant(bytes.constant))
               : std::nullopt;
  if (bytes.scale <= 0)
    return std::nullopt;
  auto base = transfer.nameOf(bytes.var, /*extent=*/true);
  if (!base)
    return std::nullopt;
  WitnessTerm term = std::move(*base);
  if (bytes.scale != 1)
    term =
        WitnessTerm::mul(std::move(term), WitnessTerm::ofConstant(bytes.scale));
  if (bytes.constant > 0)
    term = WitnessTerm::add(std::move(term),
                            WitnessTerm::ofConstant(bytes.constant));
  else if (bytes.constant < 0)
    term = WitnessTerm::sub(std::move(term),
                            WitnessTerm::ofConstant(-bytes.constant));
  return term;
}

/// `(bytes - skip) / unit` elements as a C term.
static std::optional<WitnessTerm> countTerm(const Transfer &transfer,
                                            const core::Term &bytes,
                                            std::int64_t skip,
                                            std::int64_t unit) {
  if (!bytes.known || unit <= 0)
    return std::nullopt;
  core::Term rest = bytes.plusConstant(-skip);
  if (rest.isConstant())
    return rest.constant >= 0
               ? std::optional(WitnessTerm::ofConstant(rest.constant / unit))
               : std::nullopt;
  if (rest.scale > 0 && rest.scale % unit == 0 && rest.constant % unit == 0) {
    core::Term elements =
        core::Term::ofSym(rest.var, rest.scale / unit, rest.constant / unit);
    return bytesTerm(transfer, elements);
  }
  auto all = bytesTerm(transfer, rest);
  if (!all)
    return std::nullopt;
  return unit == 1 ? std::move(*all) : WitnessTerm::div(std::move(*all), unit);
}

//===----------------------------------------------------------------------===//
// Accesses
//===----------------------------------------------------------------------===//

namespace {
/// The decision helpers for one site.
class Decider {
public:
  Decider(Transfer &transfer, const SiteInfo &site)
      : transfer(transfer), run(transfer.functionRun()),
        heap(transfer.domain()), state(transfer.heapState()), out(run.ledger()),
        context(run.ast()), site(site) {}

  void access();
  void release(core::Sym pointer, const std::string &family);
  void conflictingBorrow(core::Sym pointer);
  void invalidRelease(core::Sym pointer);
  void unknownCall(const CallExpr &call, bool callback);
  void temporalOf(core::Sym pointer, const Expr &operand, bool isRelease);
  /// RFC 0030 §8.2 `reads(S)`: the call uses the value slot `S` retains.
  void readsState(const std::string &slot);
  void nullOf(core::Sym pointer, const Expr &operand);

  bool applies(core::Facet facet) const { return run.applies(site.id, facet); }
  void decide(core::Facet facet, core::FacetDecision decision) {
    if (!run.isPublishing() || !applies(facet))
      return;
    // RFC 0017 §5: a requirement's count bounds an index from above only;
    // an index that may be negative lies before the object whatever the
    // callers pass, so no requirement covers its spatial facet.
    if ((site.kind == core::SiteKind::Deref ||
         site.kind == core::SiteKind::Index) &&
        !(facet == core::Facet::Spatial && mayBeNegativeIndex()))
      decision = transfer.covered(site, facet, std::move(decision));
    out.decideAs(*site.stmt, site.kind, site.boundary, facet, decision);
  }
  void report(core::Diagnostic diagnostic, core::Certainty certainty,
              core::Facet facet) {
    run.report(std::move(diagnostic), certainty, site.stmt, facet);
  }

private:
  Transfer &transfer;
  FunctionRun &run;
  core::Heap &heap;
  core::HeapState &state;
  LedgerAdapter &out;
  ASTContext &context;
  const SiteInfo &site;

  /// `beyond`: the index is above `INT64_MAX` for every value, so the
  /// access lies past the end of any object (RFC 0017).
  void spatialOf(core::Sym pointer, std::int64_t width,
                 std::optional<std::int64_t> skip, bool beyond = false);
  bool writesLiteral(core::Sym pointer, const Expr &operand);
  bool memberBound(std::int64_t width);
  std::string indexText(const Expr &index);
  /// The site's index may be negative.
  bool mayBeNegativeIndex() {
    if (site.index == nullptr || !site.index->getType()->isSignedIntegerType())
      return false;
    auto lower = state.zone.lower(transfer.valueOf(*site.index));
    return !lower || *lower < 0;
  }
};
} // namespace

void Decider::nullOf(core::Sym pointer, const Expr &operand) {
  if (!applies(core::Facet::Null))
    return;
  const core::SymInfo &value = heap.info(state, pointer);
  std::string name = transfer.spell(operand);
  if (value.uninit && value.null == core::PointerNull::Null) {
    decide(core::Facet::Null, core::FacetDecision::violation());
    report(
        makeDiagnostic(core::diag::UseOfUninitialized,
                       "use of " + inQuotes(name) + " before it was initialized",
                       context, operand.getBeginLoc(), core::Severity::Error),
        core::Certainty::Definite, core::Facet::Null);
    return;
  }
  switch (value.null) {
  case core::PointerNull::NonNull:
    decide(core::Facet::Null, core::FacetDecision::proven());
    return;
  case core::PointerNull::Null:
    if (!value.allocatorSource && !value.mayUninit) {
      decide(core::Facet::Null, core::FacetDecision::violation());
      core::Diagnostic diagnostic =
          makeDiagnostic(core::diag::NullDereference,
                         "dereference of " + inQuotes(name) + ", which is null",
                         context, operand.getBeginLoc(), core::Severity::Error);
      transfer.addNullNote(diagnostic, value, name);
      report(std::move(diagnostic), core::Certainty::Definite,
             core::Facet::Null);
      return;
    }
    decide(core::Facet::Null, core::FacetDecision::checked());
    if (applies(core::Facet::Null))
      transfer.allocationFailure(value, operand, *site.stmt);
    return;
  case core::PointerNull::Maybe:
    // RFC 0030 §11: without zero-initialisation a path that never assigned
    // the pointer leaves garbage, which a null check cannot catch; a
    // declaration a jump bypasses is not zero-initialised either.
    if ((value.uninit || value.mayUninit) &&
        (!run.unitRun().input.options.zeroInit || run.isBypassed(operand))) {
      decide(core::Facet::Null, core::FacetDecision::unresolvedFor(
                                    core::UnresolvedReason::NoZeroInit,
                                    inQuotes(name) + " may be uninitialised"));
      return;
    }
    if (site.nullSystemApi) {
      decide(core::Facet::Null,
             core::FacetDecision::trustedFor(core::TrustReason::SystemApi));
      return;
    }
    decide(core::Facet::Null, core::FacetDecision::checked());
    transfer.allocationFailure(value, operand, *site.stmt);
    return;
  }
}

void Decider::readsState(const std::string &slot) {
  if (!applies(core::Facet::Temporal))
    return;
  auto held = heap.read(state, run.stateObject(slot), core::CellKey{});
  if (!held || heap.info(state, *held).type != core::SymInfo::Type::Pointer)
    return;
  core::TemporalVerdict verdict = heap.temporal(state, *held);
  if (!verdict.record ||
      verdict.record->reason == core::ReleaseRecord::Reason::Moved ||
      (verdict.kind != core::TemporalVerdict::Kind::Violation &&
       verdict.kind != core::TemporalVerdict::Kind::MayReleased))
    return;
  bool definite = verdict.kind == core::TemporalVerdict::Kind::Violation;
  std::string name = "<" + slot + ">";
  decide(core::Facet::Temporal, definite
                                    ? core::FacetDecision::violation()
                                    : core::FacetDecision::unresolvedFor(
                                          core::UnresolvedReason::MayReleased));
  core::Diagnostic diagnostic = makeDiagnostic(
      core::diag::UseAfterFree,
      "use of " + inQuotes(name) +
          (definite ? " after it was freed" : " after it may have been freed"),
      context, site.stmt->getBeginLoc(),
      definite ? core::Severity::Error : core::Severity::Warning);
  addNote(diagnostic, definite ? "freed here" : "freed here on some paths",
          verdict.record->where);
  report(std::move(diagnostic),
         definite ? core::Certainty::Definite : core::Certainty::Possible,
         core::Facet::Temporal);
}

void Decider::temporalOf(core::Sym pointer, const Expr &operand,
                         bool isRelease) {
  if (!applies(core::Facet::Temporal))
    return;
  const core::SymInfo &value = heap.info(state, pointer);
  if (value.rawCast && !isRelease) {
    decide(core::Facet::Temporal,
           core::FacetDecision::unresolvedFor(core::UnresolvedReason::RawCast));
    return;
  }
  core::TemporalVerdict verdict = heap.temporal(state, pointer);
  std::string name = transfer.spell(operand);
  using Kind = core::TemporalVerdict::Kind;
  using Reason = core::ReleaseRecord::Reason;
  auto verbFor = [](const core::ReleaseRecord &record) -> std::string {
    switch (record.reason) {
    case Reason::Moved:
      return "moved";
    case Reason::ShareReleased:
      return "reference was released";
    default:
      return "freed";
    }
  };
  auto idFor = [&](const core::ReleaseRecord &record) {
    if (isRelease && record.reason != Reason::Moved)
      return core::diag::DoubleFree;
    return record.reason == Reason::Moved ? core::diag::UseAfterMove
                                          : core::diag::UseAfterFree;
  };
  auto noteFor = [&](const core::ReleaseRecord &record, bool some,
                     core::Diagnostic &diagnostic) {
    std::string verb = record.reason == Reason::Moved ? "moved" : "freed";
    if (record.reason == Reason::ShareReleased)
      verb = "reference released";
    if (isRelease) {
      verb = record.reason == Reason::ShareReleased ? "released" : "freed";
      addNote(diagnostic,
              "previously " + verb + " here" + (some ? " on some paths" : ""),
              record.where);
      return;
    }
    std::string text = verb + " here";
    if (some)
      text += " on some paths";
    if (!record.via.empty() && record.via != name)
      text += " (through " + inQuotes(record.via) + ")";
    addNote(diagnostic, text, record.where);
  };
  switch (verdict.kind) {
  case Kind::Proven: {
    // A borrow of a library slot's storage lives as long as the row says
    // (RFC 0030 §8.2).
    const core::SymInfo &borrow = heap.info(state, pointer);
    bool stateOnly = !borrow.top && !borrow.targets.empty();
    for (const core::Target &target : borrow.targets)
      stateOnly = stateOnly && run.isStateObject(target.object);
    decide(core::Facet::Temporal,
           stateOnly
               ? core::FacetDecision::trustedFor(core::TrustReason::LibrarySpec)
               : core::FacetDecision::proven());
    // RFC 0031 amends RFC 0030 §9.4: the proof rests on the entry
    // assumption of the places the pointer (or the value it was derived
    // from) was loaded from at entry.
    if (!stateOnly && run.isPublishing() && applies(core::Facet::Temporal)) {
      std::vector<core::Sym> origins{pointer};
      origins.insert(origins.end(), borrow.ancestors.begin(),
                     borrow.ancestors.end());
      for (core::Sym origin : origins)
        if (const core::SymInfo *held = state.syms.find(origin))
          for (const auto &[object, key] : held->entryOrigins)
            out.reliesOn(site.id, cellClass(run, object, key));
    }
    return;
  }
  case Kind::Violation: {
    decide(core::Facet::Temporal, core::FacetDecision::violation());
    if (!verdict.record) {
      // Storage whose lifetime ended (§5.2, §5.7): reported where the
      // pointer was stored.
      transfer.danglingUse(operand, verdict, site.stmt, true);
      return;
    }
    const core::ReleaseRecord &record = *verdict.record;
    std::string message;
    if (isRelease && record.reason == Reason::Moved)
      message = "use of " + inQuotes(name) + " after it was moved";
    else if (isRelease)
      message =
          inQuotes(name) + " is " +
          (record.reason == Reason::ShareReleased ? "released" : "freed") +
          " twice";
    else if (record.reason == Reason::ShareReleased)
      message = "use of " + inQuotes(name) + " after its reference was released";
    else
      message = "use of " + inQuotes(name) + " after it was " + verbFor(record);
    core::Diagnostic diagnostic = makeDiagnostic(
        idFor(record), message, context,
        isRelease ? site.stmt->getBeginLoc() : operand.getBeginLoc(),
        core::Severity::Error);
    noteFor(record, false, diagnostic);
    report(std::move(diagnostic), core::Certainty::Definite,
           core::Facet::Temporal);
    return;
  }
  case Kind::MayReleased: {
    const core::ReleaseRecord record =
        verdict.record.value_or(core::ReleaseRecord{});
    bool moved = record.reason == Reason::Moved;
    decide(core::Facet::Temporal,
           core::FacetDecision::unresolvedFor(
               moved ? core::UnresolvedReason::MayMoved
                     : core::UnresolvedReason::MayReleased));
    std::string message;
    if (isRelease && moved)
      message = "use of " + inQuotes(name) + " after it may have been moved";
    else if (isRelease)
      message = inQuotes(name) + " may be freed twice";
    else
      message = "use of " + inQuotes(name) + " after it may have been " +
                (moved ? "moved" : "freed");
    core::Diagnostic diagnostic = makeDiagnostic(
        idFor(record), message, context,
        isRelease ? site.stmt->getBeginLoc() : operand.getBeginLoc(),
        core::Severity::Warning);
    noteFor(record, true, diagnostic);
    report(std::move(diagnostic), core::Certainty::Possible,
           core::Facet::Temporal);
    return;
  }
  case Kind::MayAliasReleased:
    decide(core::Facet::Temporal,
           core::FacetDecision::unresolvedFor(
               core::UnresolvedReason::MayAliasReleased,
               inQuotes(name) + " may point into an object released earlier"));
    return;
  case Kind::UnknownCallee:
    // (The detail names the code that may have released it; at a call the
    // callee's own suggestion is the detail.)
    decide(core::Facet::Temporal,
           core::FacetDecision::unresolvedFor(
               core::UnresolvedReason::UnknownCallee,
               verdict.record && !isa<CallExpr>(site.stmt) ? verdict.record->via
                                                           : std::string()));
    return;
  case Kind::Callback:
    decide(core::Facet::Temporal, core::FacetDecision::unresolvedFor(
                                      core::UnresolvedReason::Callback));
    return;
  case Kind::MayDangle:
    decide(core::Facet::Temporal, core::FacetDecision::unresolvedFor(
                                      core::UnresolvedReason::MayDangle));
    transfer.danglingUse(operand, verdict, site.stmt, false);
    return;
  }
}

void Decider::spatialOf(core::Sym pointer, std::int64_t width,
                        std::optional<std::int64_t> skip, bool beyond) {
  if (!applies(core::Facet::Spatial))
    return;
  if (site.provenByType) {
    decide(core::Facet::Spatial, core::FacetDecision::proven());
    return;
  }
  const core::SymInfo &value = heap.info(state, pointer);
  if (value.rawCast) {
    decide(core::Facet::Spatial,
           core::FacetDecision::unresolvedFor(core::UnresolvedReason::RawCast));
    return;
  }
  core::SpatialVerdict verdict =
      heap.spatial(state, pointer, core::Term::of(0), width);
  using Kind = core::SpatialVerdict::Kind;
  if (beyond && !value.top && !value.targets.empty()) {
    verdict.kind = Kind::Violation;
    verdict.beforeStart = false;
  }
  auto fallback = [&](core::UnresolvedReason reason) {
    if (site.spatialSystemApi) {
      decide(core::Facet::Spatial,
             core::FacetDecision::trustedFor(core::TrustReason::SystemApi));
      return;
    }
    if (site.spatialCheckable()) {
      decide(core::Facet::Spatial, core::FacetDecision::checked());
      return;
    }
    decide(core::Facet::Spatial, core::FacetDecision::unresolvedFor(reason));
  };
  // The witness: an index against the elements left after the constant
  // skip, or a span from the object's start.
  auto witnessFor = [&]() -> std::optional<CheckWitness> {
    if (!verdict.extent || value.targets.empty())
      return std::nullopt;
    const core::Extent &extent = *verdict.extent;
    const core::Target &target = value.targets.front();
    WitnessTerm index = site.index != nullptr ? WitnessTerm::ofExpr(*site.index)
                                              : WitnessTerm::ofConstant(0);
    // A variable-length array: its size is the one its declaration
    // captured, `sizeof v`, whatever its count holds now (§10.3 rule 4).
    const core::ObjectInfo &targetInfo = run.table().info(target.object);
    if (targetInfo.key.kind == core::ObjectKind::Local &&
        value.targets.size() == 1)
      if (const VarDecl *var = variableOf(targetInfo);
          var != nullptr && context.getAsVariableArrayType(var->getType()) &&
          // Only storage whose byte size `sizeof` can hold (RFC 0017): a
          // product that wraps would make the check itself wrong.
          (site.index == nullptr ||
           transfer.bytesOf(var->getType(), *site.index)))
        return CheckWitness{.shape = CheckWitness::Shape::Span,
                            .extent = WitnessTerm::sizeOf(var->getType()),
                            .extentClass = extent.cls,
                            .base = WitnessTerm::ofPlace(*var),
                            .width = WitnessTerm::ofConstant(width),
                            .offset = std::move(index),
                            .unmodified = true,
                            .accessesSafe = true};
    // (Not before the object's start: the index check has no lower bound
    // but zero. So only at the object's start, or for a subscript of an
    // array, which C bounds below by its first element; a cursor into the
    // object, which may step back, gets a span, RFC 0030 §7.4.)
    bool arrayBase = false;
    if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(site.stmt))
      arrayBase =
          subscript->getBase()->IgnoreParenImpCasts()->getType()->isArrayType();
    if (skip && (*skip == 0 || (*skip > 0 && arrayBase)) && width > 0)
      if (auto count = countTerm(transfer, extent.bytes, *skip, width))
        return CheckWitness{.shape = CheckWitness::Shape::Index,
                            .extent = std::move(count),
                            .extentClass = extent.cls,
                            .offset = std::move(index),
                            .unmodified = true,
                            .accessesSafe = true};
    // A cursor: its object's start must have a name.
    std::optional<WitnessTerm> base;
    for (const auto &[sym, info] : state.syms) {
      if (info.type != core::SymInfo::Type::Pointer || info.targets.size() != 1)
        continue;
      if (info.targets[0].object != target.object ||
          !(info.targets[0].offset == core::Term::of(0)))
        continue;
      if (auto name = transfer.nameOf(sym)) {
        base = std::move(name);
        break;
      }
    }
    if (!base) {
      const core::ObjectInfo &objectInfo = run.table().info(target.object);
      if (objectInfo.key.kind == core::ObjectKind::Local ||
          objectInfo.key.kind == core::ObjectKind::Global)
        if (const VarDecl *var = variableOf(objectInfo);
            var != nullptr && var->getType()->isArrayType())
          base = WitnessTerm::ofPlace(*var);
    }
    auto bytes = bytesTerm(transfer, extent.bytes);
    if (!base || !bytes)
      return std::nullopt;
    return CheckWitness{.shape = CheckWitness::Shape::Span,
                        .extent = std::move(bytes),
                        .extentClass = extent.cls,
                        .base = std::move(base),
                        .width = WitnessTerm::ofConstant(width),
                        .offset = std::move(index),
                        .unmodified = true,
                        .accessesSafe = true};
  };
  switch (verdict.kind) {
  case Kind::Proven:
    decide(core::Facet::Spatial, core::FacetDecision::proven());
    return;
  case Kind::Violation: {
    // A lowered violation traps with the `violation` template (RFC 0030
    // §3.4).
    decide(core::Facet::Spatial, core::FacetDecision::violation());
    std::string subject = inQuotes(transfer.spell(*cast<Expr>(site.stmt)));
    // The object: an array member's is the object it is a member of.
    const Expr *objectExpr =
        site.operand != nullptr ? site.operand->IgnoreParenImpCasts() : nullptr;
    while (const auto *member = dyn_cast_or_null<MemberExpr>(objectExpr)) {
      if (!member->getType()->isArrayType())
        break;
      objectExpr = member->getBase()->IgnoreParenImpCasts();
    }
    std::string object = objectExpr != nullptr
                             ? inQuotes(transfer.spell(*objectExpr))
                             : "the object";
    std::string message;
    // An amount of bytes as the program spells it: `5 bytes`, `'n' + 2
    // bytes`.
    auto amount = [&](const core::Term &bytes) {
      return transfer.spellAmount(bytes, /*own=*/false);
    };
    std::optional<std::string> extent =
        verdict.extent ? amount(verdict.extent->bytes) : std::nullopt;
    std::optional<core::Term> end = verdict.end;
    // A member access reaches the end of its member, not of the whole
    // record the check covers (`'r->id' ... reaches 12 bytes`).
    if (const auto *member = dyn_cast<MemberExpr>(site.stmt);
        member != nullptr && end && end->isConstant() && verdict.extent &&
        verdict.extent->bytes.isConstant())
      if (const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
          field != nullptr && !field->isBitField() &&
          !field->getType()->isIncompleteType())
        if (auto fieldWidth = objectWidth(context, field->getType())) {
          std::int64_t fieldEnd =
              end->constant - width +
              static_cast<std::int64_t>(context.getFieldOffset(field) /
                                        context.getCharWidth()) +
              *fieldWidth;
          if (fieldEnd > verdict.extent->bytes.constant)
            end = core::Term::of(fieldEnd);
        }
    std::optional<std::string> reach = end ? amount(*end) : std::nullopt;
    // The index as written: a subscript's, or `k` of `*(q + k)`.
    const Expr *index = site.index;
    bool negate = false;
    if (index == nullptr)
      if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(site.stmt))
        index = subscript->getIdx();
    if (index == nullptr && site.operand != nullptr)
      if (const auto *sum =
              dyn_cast<BinaryOperator>(site.operand->IgnoreParenImpCasts());
          sum != nullptr && sum->isAdditiveOp() &&
          sum->getLHS()->getType()->isPointerType()) {
        index = sum->getRHS();
        negate = sum->getOpcode() == BO_Sub;
      }
    core::Term indexTerm = index != nullptr
                               ? transfer.termOf(transfer.valueOf(*index))
                               : core::Term::unknown();
    if (negate && indexTerm.isConstant())
      indexTerm = core::Term::of(-indexTerm.constant);
    // `p[n]` against `n` elements, or an index the zone orders against the
    // count.
    std::string countClause;
    // (The extent's bytes, or the product they are reduced from.)
    auto countOf = [&](const core::Term &bytes) {
      return bytes.known && !bytes.isConstant() && bytes.constant == 0 &&
             verdict.end->var == indexTerm.var &&
             verdict.end->scale == bytes.scale &&
             verdict.end->constant == verdict.end->scale;
    };
    const core::Term *countTerm = nullptr;
    if (verdict.extent && verdict.end && indexTerm.known &&
        !indexTerm.isConstant() && indexTerm.scale == 1 &&
        indexTerm.constant == 0) {
      if (countOf(verdict.extent->bytes))
        countTerm = &verdict.extent->bytes;
      else if (verdict.extent->unwrapped && countOf(*verdict.extent->unwrapped))
        countTerm = &*verdict.extent->unwrapped;
    }
    if (countTerm != nullptr) {
      core::Sym count = countTerm->var;
      // (Only values C places hold: a computed index reads better as the
      // bytes it reaches.)
      auto name = [&](core::Sym sym) -> std::string {
        if (auto place = transfer.nameOf(sym))
          return messageSpelling(*place);
        return heap.info(state, sym).name;
      };
      std::string countName = name(count);
      std::string indexName = name(indexTerm.var);
      if (!countName.empty() && count == indexTerm.var) {
        countClause =
            inQuotes(countName) + " is the number of elements of " + object;
      } else if (!countName.empty() && !indexName.empty()) {
        std::string relation = " is at least ";
        if (auto gap = state.zone.bound(count, indexTerm.var); gap && *gap < 0)
          relation = " is above ";
        countClause = inQuotes(indexName) + relation + inQuotes(countName) +
                      ", the number of elements of " + object;
      }
    }
    if (verdict.beforeStart)
      message = subject + " is out of bounds: " +
                (index != nullptr ? "index " + indexText(*index) + " is"
                                  : std::string("it lies")) +
                " before the start of " + object;
    else if (index != nullptr && (indexTerm.isConstant() || beyond) && extent)
      message = subject + " is out of bounds: index " + indexText(*index) +
                " of an object of " + *extent;
    else if (!countClause.empty())
      message = subject + " is out of bounds: " + countClause;
    else if (index != nullptr && indexTerm.known && !indexTerm.isConstant() &&
             indexTerm.scale == 1 && indexTerm.constant == 0 && extent &&
             verdict.extent && verdict.extent->bytes.isConstant() &&
             state.zone.lower(indexTerm.var) && transfer.nameOf(indexTerm.var))
      // RFC 0012: `'i' is at least 8 in an object of 8 bytes`.
      message = subject + " is out of bounds: " +
                inQuotes(messageSpelling(*transfer.nameOf(indexTerm.var))) +
                " is at least " +
                std::to_string(*state.zone.lower(indexTerm.var)) +
                " in an object of " + *extent;
    else if (reach && extent)
      message = subject + " is out of bounds: it reaches " + *reach + " into " +
                object + ", which has " + *extent;
    else
      message = subject + " is out of bounds of " + object + ", which has " +
                extent.value_or("fewer bytes");
    core::Diagnostic diagnostic =
        makeDiagnostic(core::diag::OutOfBounds, message, context,
                       site.stmt->getBeginLoc(), core::Severity::Error);
    // Where the object came from.
    const core::SymInfo &accessed = heap.info(state, pointer);
    if (accessed.targets.size() == 1 && objectExpr != nullptr) {
      const core::ObjectInfo &info =
          run.table().info(accessed.targets[0].object);
      std::string spelled = transfer.spell(*objectExpr);
      if (info.key.kind == core::ObjectKind::Local ||
          info.key.kind == core::ObjectKind::Global)
        addNote(diagnostic,
                spelled == info.name ? inQuotes(spelled) + " is declared here"
                                     : "the object behind " + inQuotes(spelled) +
                                           " is declared here",
                info.created);
      else if (info.key.kind == core::ObjectKind::HeapRecent ||
               info.key.kind == core::ObjectKind::HeapOld)
        addNote(diagnostic, inQuotes(spelled) + " is allocated here",
                info.created);
    }
    report(std::move(diagnostic), core::Certainty::Definite,
           core::Facet::Spatial);
    return;
  }
  case Kind::Checkable: {
    std::optional<CheckWitness> witness = witnessFor();
    decide(core::Facet::Spatial, core::FacetDecision::checked());
    if (witness && run.isPublishing())
      out.witness(*site.stmt, core::Facet::Spatial, std::move(*witness));
    return;
  }
  case Kind::UnknownExtent:
    fallback(core::UnresolvedReason::UnknownExtent);
    return;
  case Kind::UnknownIndex:
    fallback(core::UnresolvedReason::UnknownIndex);
    return;
  }
}

/// Whether the site's lvalue is stored to (`p[0] = c`, `++*p`).
static bool isWritten(ASTContext &context, const Stmt &lvalue) {
  DynTypedNodeList parents = context.getParents(lvalue);
  for (int depth = 0; depth < 4 && !parents.empty(); ++depth) {
    const Stmt *parent = parents[0].get<Stmt>();
    if (parent == nullptr)
      return false;
    if (isa<ParenExpr>(parent)) {
      parents = context.getParents(*parent);
      continue;
    }
    if (const auto *binary = dyn_cast<BinaryOperator>(parent))
      return binary->isAssignmentOp() &&
             binary->getLHS()->IgnoreParens() == &lvalue;
    if (const auto *unary = dyn_cast<UnaryOperator>(parent))
      return unary->isIncrementDecrementOp();
    return false;
  }
  return false;
}

bool Decider::writesLiteral(core::Sym pointer, const Expr &operand) {
  const core::SymInfo &value = heap.info(state, pointer);
  if (value.top || value.targets.empty())
    return false;
  bool any = false;
  bool all = true;
  for (const core::Target &target : value.targets) {
    bool literal =
        run.table().info(target.object).key.kind == core::ObjectKind::Literal;
    any = any || literal;
    all = all && literal;
  }
  if (!any || !isWritten(context, *site.stmt))
    return false;
  if (!all) {
    decide(core::Facet::Spatial, core::FacetDecision::unresolvedFor(
                                     core::UnresolvedReason::UnknownExtent,
                                     "it may point into a string literal"));
    return true;
  }
  decide(core::Facet::Spatial, core::FacetDecision::violation());
  report(makeDiagnostic(core::diag::OutOfBounds,
                        "write through " + inQuotes(transfer.spell(operand)) +
                            ", which points to a string literal",
                        context, site.stmt->getBeginLoc(),
                        core::Severity::Error),
         core::Certainty::Definite, core::Facet::Spatial);
  return true;
}

/// RFC 0004: where a raw pointer's raw origin is, for the note: `'n' is
/// raw: cast from an integer here (through 'p')`.
static std::string rawNote(const core::SymInfo &value,
                           const std::string &name) {
  std::string origin;
  switch (value.rawOrigin) {
  case core::SymInfo::RawOrigin::Declared:
    origin = "declared WEAVEC_RAW";
    break;
  case core::SymInfo::RawOrigin::Loaded:
    origin = value.rawFrom.empty()
                 ? std::string("loaded through a raw pointer")
                 : "loaded through raw pointer '" + value.rawFrom + "'";
    break;
  case core::SymInfo::RawOrigin::Returned:
    origin = value.rawFrom.empty() ? std::string("handed out by a callee")
                                   : "handed out by '" + value.rawFrom + "'";
    break;
  case core::SymInfo::RawOrigin::Cast:
    origin = "cast from an integer";
    break;
  }
  std::string subject =
      name.empty() ? std::string("the pointer") : "'" + name + "'";
  std::string through;
  if (!name.empty() && !value.rawVia.empty() && value.rawVia != name)
    through = " (through '" + value.rawVia + "')";
  return subject + " is raw: " + origin + " here" + through;
}

/// RFC 0004: what an unsafe operation outside a region should become.
static constexpr const char *UnsafeFixIt =
    "move this operation into a WEAVEC_UNSAFE block or function, or assert "
    "the pointer's ownership first";

void Decider::access() {
  const auto *expr = dyn_cast<Expr>(site.stmt);
  if (expr == nullptr)
    return;
  const Expr *operand = site.operand;
  core::Sym pointer = core::ZeroSym;
  if (operand != nullptr && operand->getType()->isPointerType())
    pointer = transfer.valueOf(*operand);
  // A pointer with an RFC 0004 raw origin (declared `WEAVEC_RAW`, from an
  // integer, loaded through a raw pointer, handed out as raw).
  const bool raw = site.kind == core::SiteKind::Raw ||
                   (pointer != core::ZeroSym && heap.info(state, pointer).raw);
  // RFC 0030 §6.1: inside a region a raw access is trusted for every facet,
  // the temporal one included.
  if (raw && site.inUnsafe) {
    for (core::Facet facet :
         {core::Facet::Null, core::Facet::Spatial, core::Facet::Temporal})
      if (applies(facet))
        decide(facet,
               core::FacetDecision::trustedFor(core::TrustReason::Unsafe));
    return;
  }
  // Raw through some of a call's functions only (RFC 0031 *Implementation
  // amendments*): no error, and nothing about it proven.
  if (raw && site.kind != core::SiteKind::Raw && pointer != core::ZeroSym &&
      heap.info(state, pointer).rawSome) {
    nullOf(pointer, *operand);
    for (core::Facet facet : {core::Facet::Spatial, core::Facet::Temporal})
      if (applies(facet))
        decide(facet, core::FacetDecision::unresolvedFor(
                          core::UnresolvedReason::RawCast));
    return;
  }
  // Outside a region it is an error (RFC 0004), on the spatial facet; its
  // object is none the analysis tracks, so the temporal one is unresolved.
  if (raw && run.isPublishing()) {
    // (A conversion written in place is no name: `((T *)x)->v`.)
    std::string spelled =
        operand != nullptr &&
                !isa<ExplicitCastExpr>(operand->IgnoreParenImpCasts())
            ? transfer.spell(*operand)
            : std::string();
    core::Diagnostic diagnostic = makeDiagnostic(
        core::diag::UnsafeOperation,
        "dereference of raw pointer " +
            (spelled.empty() ? std::string() : inQuotes(spelled) + " ") +
            "outside an unsafe region",
        context,
        operand != nullptr ? operand->getBeginLoc() : site.stmt->getBeginLoc(),
        core::Severity::Error);
    if (pointer != core::ZeroSym && heap.info(state, pointer).raw)
      addNote(diagnostic, rawNote(heap.info(state, pointer), spelled),
              heap.info(state, pointer).rawAt);
    diagnostic.addNote(UnsafeFixIt, core::SourceLocation{});
    report(std::move(diagnostic), core::Certainty::Definite,
           core::Facet::Spatial);
  }
  if (raw) {
    if (pointer != core::ZeroSym)
      nullOf(pointer, *operand);
    if (applies(core::Facet::Temporal))
      decide(core::Facet::Temporal, core::FacetDecision::unresolvedFor(
                                        core::UnresolvedReason::RawCast));
    return;
  }
  if (pointer != core::ZeroSym) {
    nullOf(pointer, *operand);
    temporalOf(pointer, *operand, false);
  }
  if (!applies(core::Facet::Spatial))
    return;
  // A string literal has no writable byte (RFC 0030 *Diagnostics*).
  if (pointer != core::ZeroSym && writesLiteral(pointer, *operand))
    return;
  if (site.kind == core::SiteKind::Deref && pointer != core::ZeroSym) {
    QualType pointee = operand->getType()->getPointeeType();
    auto width = objectWidth(context, pointee);
    if (!width || *width <= 0) {
      decide(core::Facet::Spatial, core::FacetDecision::unresolvedFor(
                                       core::UnresolvedReason::UnknownExtent));
      return;
    }
    const core::SymInfo &value = heap.info(state, pointer);
    std::optional<std::int64_t> skip;
    if (value.targets.size() == 1 && value.targets[0].offset.isConstant())
      skip = value.targets[0].offset.constant;
    spatialOf(pointer, *width, skip);
    return;
  }
  // A variable-length array whose byte size may not be representable (or
  // whose dimension may not be positive): C defines no storage, and the
  // type's `sizeof` would be no bound to check against (RFC 0017).
  if (site.operand != nullptr)
    if (const auto *ref =
            dyn_cast<DeclRefExpr>(site.operand->IgnoreParenImpCasts());
        ref != nullptr && ref->getType()->isVariablyModifiedType() &&
        ref->getType()->isArrayType() &&
        !transfer.bytesOf(ref->getType(), *ref)) {
      // A dimension's own bound still shows a violation.
      if (memberBound(transfer.sizeOf(expr->getType()).value_or(0)))
        return;
      decide(core::Facet::Spatial, core::FacetDecision::unresolvedFor(
                                       core::UnresolvedReason::UnknownExtent));
      return;
    }
  // An index: the element's address.
  Address address = transfer.addressOf(*expr);
  core::SymInfo element;
  element.type = core::SymInfo::Type::Pointer;
  element.targets = address.targets;
  element.top = address.top;
  core::Sym at = heap.fresh(state, element);
  auto width = transfer.sizeOf(expr->getType());
  if (!width || *width <= 0) {
    // A row of a variable-length array: its own dimension still bounds it.
    if (memberBound(0))
      return;
    decide(core::Facet::Spatial, core::FacetDecision::unresolvedFor(
                                     core::UnresolvedReason::Unanalysed));
    return;
  }
  // The constant part of the element's offset, when the index accounts
  // for the rest.
  std::optional<std::int64_t> skip;
  if (site.index != nullptr && address.targets.size() == 1) {
    core::Term offset = address.targets[0].offset;
    core::Term index = transfer.termOf(transfer.valueOf(*site.index));
    if (offset.known && index.known) {
      core::Term scaled = index;
      scaled.scale *= *width;
      scaled.constant *= *width;
      if (scaled.isConstant())
        scaled.scale = 0;
      core::Term negated = scaled;
      negated.scale = -negated.scale;
      negated.constant = -negated.constant;
      if (auto rest = offset.plus(negated); rest && rest->isConstant())
        skip = rest->constant;
    }
  }
  // RFC 0017: an unsigned index above `INT64_MAX` for every value reaches
  // past the end of any object (the zone's bounds cannot say so).
  bool beyond = false;
  if (site.index != nullptr) {
    const core::SymInfo &index =
        heap.info(state, transfer.valueOf(*site.index));
    beyond = index.values && !index.values->empty() &&
             !index.values->type.isSigned &&
             !index.values->minimum()->signedValue();
  }
  if (!beyond && memberBound(*width))
    return;
  spatialOf(at, *width, skip, beyond);
}

bool Decider::memberBound(std::int64_t width) {
  // RFC 0030 §7.4: a direct subscript of a non-flexible array-typed lvalue
  // uses the array's own bound, as C does (`r->name[8]` for `char
  // name[8]`, `m[i][j]` needs `j < 4` for `int m[3][4]`).
  if (site.index == nullptr || site.operand == nullptr)
    return false;
  const Expr *member = site.operand->IgnoreParenImpCasts();
  const ConstantArrayType *array =
      context.getAsConstantArrayType(member->getType());
  // RFC 0017: a variable-length dimension, with the count its declaration
  // captured.
  const VariableArrayType *vla =
      array == nullptr ? context.getAsVariableArrayType(member->getType())
                       : nullptr;
  if (array == nullptr && vla == nullptr)
    return false;
  const FieldDecl *field = nullptr;
  if (const auto *access = dyn_cast<MemberExpr>(member)) {
    field = dyn_cast<FieldDecl>(access->getMemberDecl());
    if (field == nullptr)
      return false;
    // A trailing array is flexible at the default level (§7.4).
    const RecordDecl *record = field->getParent();
    const FieldDecl *last = nullptr;
    for (const FieldDecl *each : record->fields())
      last = each;
    if (record->isUnion() ||
        (last == field && context.getLangOpts().getStrictFlexArraysLevel() ==
                              LangOptions::StrictFlexArraysLevelKind::Default))
      return false;
  } else if (const auto *ref = dyn_cast<DeclRefExpr>(member)) {
    // A variable whose rows are variable-length arrays: the outer
    // dimension is the variable's own (a parameter is a pointer). A
    // variable of fixed rows is bounded by its extent alone.
    const ArrayType *element =
        array != nullptr ? context.getAsArrayType(array->getElementType())
                         : nullptr;
    if (vla == nullptr &&
        (element == nullptr || !element->isVariableArrayType()))
      return false;
    if (!isa<VarDecl>(ref->getDecl()))
      return false;
  } else if (!isa<ArraySubscriptExpr>(member)) {
    return false;
  }
  // Whether the array lies inside storage the analysis knows: the chain of
  // subscripts leads to an array variable whose byte size is representable
  // (every dimension positive, RFC 0017), to a member the outer access
  // reached, or to a pointer whose rows have a constant size (its own site
  // checks the whole row). Only then does the array's own bound prove an
  // access; a violation of it is one regardless.
  bool storageKnown = true;
  {
    const Expr *root = member;
    while (const auto *subscript = dyn_cast<ArraySubscriptExpr>(root))
      root = subscript->getBase()->IgnoreParenImpCasts();
    if (const auto *ref = dyn_cast<DeclRefExpr>(root);
        ref != nullptr && ref->getType()->isArrayType())
      storageKnown = transfer.bytesOf(ref->getType(), *ref).has_value();
    else if (root->getType()->isPointerType())
      storageKnown =
          !root->getType()->getPointeeType()->isVariablyModifiedType();
  }
  core::Term index = transfer.termOf(transfer.valueOf(*site.index));
  if (!index.known)
    return false;
  std::int64_t count = 0;
  core::Term bound = core::Term::unknown();
  if (array != nullptr) {
    count = static_cast<std::int64_t>(array->getSize().getZExtValue());
    bound = core::Term::of(count);
  } else {
    core::Sym captured = transfer.vlaCount(*vla);
    auto hi = state.zone.upper(captured);
    // A dimension zero or negative for every value is the declaration's
    // error (`invalid-integer-operation`), not the access's.
    if (hi && *hi <= 0) {
      decide(core::Facet::Spatial, core::FacetDecision::unresolvedFor(
                                       core::UnresolvedReason::UnknownExtent));
      return true;
    }
    // One that may be is no storage to prove an access in, but an index
    // past it is out of bounds whatever it is.
    if (auto lo = state.zone.lower(captured); !lo || *lo < 1)
      storageKnown = false;
    bound = transfer.termOf(captured);
    if (auto known = state.zone.constant(captured))
      count = *known;
  }
  std::optional<bool> lower = heap.lessEqual(state, core::Term::of(0), index);
  std::optional<bool> upper =
      bound.known ? heap.lessEqual(state, index.plusConstant(1), bound)
                  : std::nullopt;
  std::string subject = inQuotes(transfer.spell(*cast<Expr>(site.stmt)));
  std::string spelled = transfer.spell(*member);
  if (lower == false || upper == false) {
    decide(core::Facet::Spatial, core::FacetDecision::violation());
    core::Diagnostic diagnostic = makeDiagnostic(
        core::diag::OutOfBounds,
        subject + " is out of bounds: index " + indexText(*site.index) +
            (lower == false ? " is before the start of " + inQuotes(spelled)
             : array == nullptr && !state.zone.constant(transfer.vlaCount(*vla))
                 ? " is past the end of " + inQuotes(spelled)
             : width > 0
                 ? " of an object of " + std::to_string(count * width) +
                       " bytes"
                 : " of an array of " + std::to_string(count) + " elements"),
        context, site.stmt->getBeginLoc(), core::Severity::Error);
    if (field != nullptr)
      addNote(diagnostic, inQuotes(spelled) + " is declared here",
              toCoreLocation(context.getSourceManager(), field->getLocation()));
    report(std::move(diagnostic), core::Certainty::Definite,
           core::Facet::Spatial);
    return true;
  }
  // The array lies inside its object by the access that reaches it (the
  // member's `r->` dereference, the outer subscript's own site), so its
  // bound decides.
  // Not violated, over storage the analysis does not know: the object's
  // extent decides, as for any access (a variable-length array's own site
  // without a representable size is left unresolved there).
  if (!storageKnown)
    return false;
  if (lower == true && upper == true) {
    decide(core::Facet::Spatial, core::FacetDecision::proven());
    return true;
  }
  if (array == nullptr) {
    // A variable-length array variable itself: its extent decides, with
    // `sizeof v` to check against (§10.3 rule 4).
    if (isa<DeclRefExpr>(member))
      return false;
    // An inner dimension's captured count has no C spelling at the site to
    // check against.
    decide(core::Facet::Spatial, core::FacetDecision::unresolvedFor(
                                     core::UnresolvedReason::UnknownIndex));
    return true;
  }
  decide(core::Facet::Spatial, core::FacetDecision::checked());
  if (run.isPublishing() && applies(core::Facet::Spatial))
    out.witness(*site.stmt, core::Facet::Spatial,
                CheckWitness{.shape = CheckWitness::Shape::Index,
                             .extent = WitnessTerm::ofConstant(count),
                             .extentClass = core::ExtentClass::Exact,
                             .offset = WitnessTerm::ofExpr(*site.index),
                             .unmodified = true,
                             .accessesSafe = true});
  return true;
}

std::string Decider::indexText(const Expr &index) {
  // The index as written, with its value when that is a folded constant
  // the text does not show: `index 'data' (10)`.
  std::string text = transfer.spell(index);
  core::Sym sym = transfer.valueOf(index);
  core::Term value = transfer.termOf(sym);
  std::optional<std::string> shown;
  if (value.isConstant())
    shown = std::to_string(value.constant);
  else if (const core::SymInfo &info = heap.info(state, sym); info.values)
    // (A value beyond the zone's 64-bit bounds, RFC 0017.)
    if (auto constant = info.values->constant())
      shown = constant->toString();
  if (shown && text != *shown)
    return "'" + text + "' (" + *shown + ")";
  return text;
}

void Decider::release(core::Sym pointer, const std::string &family) {
  const Expr *operand = site.operand;
  if (operand == nullptr)
    return;
  std::string name = transfer.spell(*operand);
  const core::SymInfo value = heap.info(state, pointer);
  // A pointer with an RFC 0004 raw origin (declared `WEAVEC_RAW`, from an
  // integer, loaded through a raw pointer, handed out as raw).
  const bool raw = site.kind == core::SiteKind::Raw || value.raw;
  // RFC 0030 §6.1: inside a region a raw release is trusted for the
  // facets of the pointer's memory; its temporal state is tracked as
  // outside, so a definite second release is still an error (probe 45).
  if (raw && site.inUnsafe) {
    for (core::Facet facet : {core::Facet::Null, core::Facet::Spatial})
      if (applies(facet))
        decide(facet,
               core::FacetDecision::trustedFor(core::TrustReason::Unsafe));
    temporalOf(pointer, *operand, true);
    return;
  }
  temporalOf(pointer, *operand, true);
  // Family (RFC 0007).
  if (applies(core::Facet::Temporal) && value.targets.size() == 1) {
    const core::ObjectState *object =
        heap.findObject(state, value.targets[0].object);
    if (object != nullptr && !object->family.empty() && !family.empty() &&
        object->family != family && object->life == core::Life::Live) {
      decide(core::Facet::Temporal, core::FacetDecision::violation());
      core::Diagnostic diagnostic = makeDiagnostic(
          core::diag::MismatchedRelease,
          inQuotes(name) + " is released with " + inQuotes(family) +
              " but must be released with " + inQuotes(object->family),
          context, site.stmt->getBeginLoc(), core::Severity::Error);
      addNote(diagnostic, "allocated here",
              run.table().info(value.targets[0].object).created);
      report(std::move(diagnostic), core::Certainty::Definite,
             core::Facet::Temporal);
    }
  }
  conflictingBorrow(pointer);
  // Raw pointers (RFC 0004): a release asserts ownership, which needs an
  // unsafe region (not for one raw through some of a call's functions).
  if (raw && !value.rawSome && run.isPublishing()) {
    std::string callee =
        site.library
            ? inQuotes(site.library->entry->name)
            : (site.callee != nullptr ? inQuotes(site.callee->getNameAsString())
                                      : std::string("a function pointer"));
    core::Diagnostic diagnostic =
        makeDiagnostic(core::diag::UnsafeOperation,
                       callee + " releases raw pointer " + inQuotes(name) +
                           " outside an unsafe region",
                       context, operand->getBeginLoc(), core::Severity::Error);
    if (value.raw)
      addNote(diagnostic, rawNote(value, name), value.rawAt);
    diagnostic.addNote(UnsafeFixIt, core::SourceLocation{});
    report(std::move(diagnostic), core::Certainty::Definite,
           core::Facet::Spatial);
  }
  invalidRelease(pointer);
}

void Decider::conflictingBorrow(core::Sym pointer) {
  // §5.5 *Conflicting borrows* (RFC 0002, RFC 0006): a pointer derived
  // from a released target and held where it outlives the release — a
  // cell reachable from a parameter, a global or an address-taken local.
  if (!applies(core::Facet::Temporal) || !run.isPublishing())
    return;
  const core::SymInfo value = heap.info(state, pointer);
  if (value.type != core::SymInfo::Type::Pointer || value.top ||
      value.null == core::PointerNull::Null)
    return;
  std::set<core::ObjectId> released;
  for (const core::Target &target : value.targets)
    released.insert(target.object);
  std::vector<core::ObjectId> roots;
  for (const auto &[id, object] : state.objects) {
    const core::ObjectInfo &info = run.table().info(id);
    if (info.key.dead || released.contains(id))
      continue;
    switch (info.key.kind) {
    case core::ObjectKind::Global:
    case core::ObjectKind::Entry:
    case core::ObjectKind::EntrySummary:
      roots.push_back(id);
      break;
    case core::ObjectKind::Local:
      if (const VarDecl *var = run.localVariable(id);
          var != nullptr && run.isAddressTaken(*var) &&
          !var->getType()->isArrayType() && !var->getType()->isRecordType())
        roots.push_back(id);
      break;
    default:
      break;
    }
  }
  std::set<core::ObjectId> seen(roots.begin(), roots.end());
  std::vector<core::ObjectId> work = roots;
  while (!work.empty()) {
    core::ObjectId id = work.back();
    work.pop_back();
    const core::ObjectState *object = heap.findObject(state, id);
    if (object == nullptr || object->life != core::Life::Live)
      continue;
    for (const auto &[key, sym] : object->cells) {
      const core::SymInfo &held = heap.info(state, sym);
      if (held.type != core::SymInfo::Type::Pointer)
        continue;
      bool borrows = false;
      bool only = !held.targets.empty() && !held.top;
      for (const core::Target &target : held.targets) {
        if (released.contains(target.object))
          borrows = true;
        else
          only = false;
        if (!released.contains(target.object) &&
            seen.insert(target.object).second)
          work.push_back(target.object);
      }
      if (!borrows || !held.derived || sym == pointer)
        continue;
      // An error when the loan holds on every path and the release is
      // certain (RFC 0030 §3.1).
      bool definite = only && value.targets.size() == 1 &&
                      run.table().info(value.targets[0].object).singular;
      decide(core::Facet::Temporal,
             definite ? core::FacetDecision::violation()
                      : core::FacetDecision::unresolvedFor(
                            core::UnresolvedReason::MayConflict));
      std::string name = transfer.spell(*site.operand);
      core::Diagnostic diagnostic = makeDiagnostic(
          core::diag::ConflictingBorrow,
          "cannot free " + inQuotes(name) + " while it is borrowed", context,
          site.stmt->getBeginLoc(),
          definite ? core::Severity::Error : core::Severity::Warning);
      const FunctionRun::FrameStore *stored = run.borrowStore(id, key);
      if (stored != nullptr && !stored->holder.empty())
        addNote(diagnostic, "borrowed by " + inQuotes(stored->holder) + " here",
                toCoreLocation(context.getSourceManager(),
                               stored->at->getBeginLoc()));
      report(std::move(diagnostic),
             definite ? core::Certainty::Definite : core::Certainty::Possible,
             core::Facet::Temporal);
      return;
    }
  }
}

/// The pointer a released operand was derived from, which the messages
/// name (RFC 0011): `p` for `p + 1` and `p - 2`, `o` for `&o->in`.
static const Expr &releasedBase(const Expr &operand) {
  const Expr *e = operand.IgnoreParenCasts();
  while (true) {
    if (const auto *binary = dyn_cast<BinaryOperator>(e);
        binary != nullptr &&
        (binary->getOpcode() == BO_Add || binary->getOpcode() == BO_Sub)) {
      if (binary->getLHS()->getType()->isPointerType()) {
        e = binary->getLHS()->IgnoreParenCasts();
        continue;
      }
      if (binary->getRHS()->getType()->isPointerType()) {
        e = binary->getRHS()->IgnoreParenCasts();
        continue;
      }
    }
    if (const auto *unary = dyn_cast<UnaryOperator>(e);
        unary != nullptr && unary->getOpcode() == UO_AddrOf)
      if (const auto *member =
              dyn_cast<MemberExpr>(unary->getSubExpr()->IgnoreParens());
          member != nullptr && member->isArrow()) {
        e = member->getBase()->IgnoreParenCasts();
        continue;
      }
    return *e;
  }
}

void Decider::invalidRelease(core::Sym pointer) {
  // Invalid releases (RFC 0008, RFC 0031 §5.5): not a heap object, or not
  // its start.
  if (!applies(core::Facet::Spatial) || site.operand == nullptr)
    return;
  const Expr *operand = site.operand;
  std::string name = transfer.spell(releasedBase(*operand));
  Transfer::ReleaseCheck check =
      transfer.releaseCheck(pointer, name, *operand, "released");
  switch (check.kind) {
  case Transfer::ReleaseCheck::Kind::Proven:
    decide(core::Facet::Spatial, core::FacetDecision::proven());
    return;
  case Transfer::ReleaseCheck::Kind::UnknownIndex:
    decide(core::Facet::Spatial, core::FacetDecision::unresolvedFor(
                                     core::UnresolvedReason::UnknownIndex));
    return;
  case Transfer::ReleaseCheck::Kind::Possible:
  case Transfer::ReleaseCheck::Kind::Violation: {
    bool definite = check.kind == Transfer::ReleaseCheck::Kind::Violation;
    decide(core::Facet::Spatial,
           definite ? core::FacetDecision::violation()
                    : core::FacetDecision::unresolvedFor(
                          core::UnresolvedReason::MayInvalidRelease));
    core::Diagnostic diagnostic = makeDiagnostic(
        core::diag::InvalidRelease, check.message, context,
        site.stmt->getBeginLoc(),
        definite ? core::Severity::Error : core::Severity::Warning);
    addNote(diagnostic, check.note, check.noteAt);
    report(std::move(diagnostic),
           definite ? core::Certainty::Definite : core::Certainty::Possible,
           core::Facet::Spatial);
    return;
  }
  }
}

Transfer::ReleaseCheck Transfer::releaseCheck(core::Sym pointer,
                                              const std::string &subject,
                                              const Expr &operand,
                                              const std::string &verb) const {
  ReleaseCheck out;
  const core::SymInfo &value = heap.info(state, pointer);
  if (value.null == core::PointerNull::Null)
    return out;
  if (value.top || value.targets.empty()) {
    out.kind = ReleaseCheck::Kind::UnknownIndex;
    return out;
  }
  QualType pointee = operand.IgnoreParenImpCasts()->getType();
  if (pointee->isPointerType())
    pointee = pointee->getPointeeType();
  std::optional<std::int64_t> element;
  if (!pointee.isNull() && !pointee->isVoidType() &&
      !pointee->isIncompleteType())
    element = static_cast<std::int64_t>(
        context.getTypeSizeInChars(pointee).getQuantity());
  // `free(buf)`, `free(&x)`, `free("abc")`: the argument is the storage.
  const Expr *stripped = operand.IgnoreParenCasts();
  bool storageItself = isa<StringLiteral>(stripped);
  // `free(&b.len)`: the storage as written.
  std::string storageSpelled;
  // `free(&o->in)`: the field the pointer points to (RFC 0011).
  std::string fieldSpelled;
  if (const auto *unary = dyn_cast<UnaryOperator>(stripped)) {
    storageItself = unary->getOpcode() == UO_AddrOf;
    if (storageItself) {
      storageSpelled = spell(*unary->getSubExpr());
      if (const auto *member =
              dyn_cast<MemberExpr>(unary->getSubExpr()->IgnoreParens()))
        fieldSpelled = member->getMemberDecl()->getNameAsString();
    }
  }
  if (const auto *ref = dyn_cast<DeclRefExpr>(stripped))
    storageItself = ref->getType()->isArrayType();
  bool anyInvalid = false;
  bool allInvalid = true;
  bool anyUnknown = false;
  for (const core::Target &target : value.targets) {
    const core::ObjectInfo &info = run.table().info(target.object);
    const core::ObjectState *object = heap.findObject(state, target.object);
    bool stack = object != nullptr && object->family == core::StackFamily;
    bool literal = info.key.kind == core::ObjectKind::Literal;
    bool nonHeap = info.key.kind == core::ObjectKind::Local ||
                   info.key.kind == core::ObjectKind::Global || literal ||
                   info.key.kind == core::ObjectKind::Function || stack;
    // Only an allocation of this function (or one a callee handed it fresh)
    // is known to start where it points; where a caller's pointer points in
    // its object is not known here.
    bool heapObject = info.key.kind == core::ObjectKind::HeapRecent ||
                      info.key.kind == core::ObjectKind::HeapOld;
    std::optional<std::int64_t> offset;
    if (target.offset.isConstant())
      offset = target.offset.constant;
    else if (target.offset.known)
      if (auto c = state.zone.constant(target.offset.var))
        offset = target.offset.scale * *c + target.offset.constant;
    // A parameter some caller passes a cursor (its kind is Unknown, §7.3)
    // may point anywhere into its allocation.
    bool cursor = false;
    if (info.key.kind == core::ObjectKind::Entry && info.key.path.isParam() &&
        info.key.path.steps.size() == 1 &&
        info.key.path.steps.front().step == core::PathStep::Deref)
      if (const KindEntry *kind =
              run.unitRun().input.kinds.param(run.decl(), info.key.path.index);
          kind != nullptr && !kind->hasShape() &&
          !kind->shapeFromSystemHeader())
        cursor = true;
    if (nonHeap) {
      anyInvalid = true;
      if (out.message.empty()) {
        std::string storage = info.key.kind == core::ObjectKind::Global
                                  ? info.name
                                  : run.frameName(target.object);
        if (storageItself) {
          out.message =
              literal
                  ? "a string literal is " + verb
                  : "'" + (storageSpelled.empty() ? storage : storageSpelled) +
                        "' is " + verb + " but is not a heap object";
          if (!literal) {
            out.note = "'" + storage + "' is declared here";
            out.noteAt = info.created;
          }
        } else if (literal) {
          out.message = "'" + subject + "' is " + verb +
                        " but points to a string literal";
        } else {
          out.message = "'" + subject + "' is " + verb + " but points to '" +
                        storage + "', which is not a heap object";
          out.note = "'" + storage + "' is declared here";
          out.noteAt = info.created;
        }
      }
    } else if (offset && *offset != 0 && !cursor &&
               (heapObject ||
                (info.key.kind == core::ObjectKind::Entry && *offset > 0))) {
      // A positive offset from any valid pointer is never the start of an
      // allocation; a negative one from a caller's pointer may be (`p - 1`).
      anyInvalid = true;
      if (out.message.empty()) {
        std::string where = "may not point to the start of its allocation";
        if (!fieldSpelled.empty()) {
          where = "points to field '" + fieldSpelled + "' of its allocation";
        } else if (element && *offset % *element == 0) {
          std::int64_t count = *offset / *element;
          std::int64_t magnitude = count < 0 ? -count : count;
          where = "points " + std::to_string(magnitude) +
                  (magnitude == 1 ? " element" : " elements") +
                  (count > 0 ? " past" : " before") +
                  " the start of its allocation";
        } else if (auto field = [&]() -> std::optional<std::string> {
                     QualType type =
                         info.type != 0 ? typeOfHandle(info.type) : QualType();
                     if (type.isNull() || !type->isRecordType())
                       return std::nullopt;
                     const RecordDecl *record = type->getAsRecordDecl();
                     if (record == nullptr || !record->isCompleteDefinition())
                       return std::nullopt;
                     const ASTRecordLayout &layout =
                         context.getASTRecordLayout(record);
                     for (const FieldDecl *f : record->fields())
                       if (static_cast<std::int64_t>(
                               layout.getFieldOffset(f->getFieldIndex()) /
                               context.getCharWidth()) == *offset)
                         return f->getNameAsString();
                     return std::nullopt;
                   }()) {
          where = "points to field '" + *field + "' of its allocation";
        }
        out.message = "'" + subject + "' is " + verb + " but " + where;
        out.note = "allocated here";
        out.noteAt = info.created;
      }
    } else if (cursor || !heapObject) {
      allInvalid = false;
      anyUnknown = true;
    } else if (!offset) {
      // Somewhere in its object (`strchr(p, 'x')`): possibly not its start.
      allInvalid = false;
      anyInvalid = true;
      if (out.message.empty())
        out.message = "'" + subject + "' is " + verb +
                      " but may not point to the start of its allocation";
    } else {
      allInvalid = false;
    }
  }
  if (!anyInvalid) {
    out.kind = anyUnknown ? ReleaseCheck::Kind::UnknownIndex
                          : ReleaseCheck::Kind::Proven;
    return out;
  }
  out.kind =
      allInvalid ? ReleaseCheck::Kind::Violation : ReleaseCheck::Kind::Possible;
  if (!allInvalid) {
    // `may point`.
    std::size_t at = out.message.find(" but points");
    if (at != std::string::npos)
      out.message.replace(at, 11, " but may point");
  }
  return out;
}

void Transfer::addNullNote(core::Diagnostic &diagnostic,
                           const core::SymInfo &value,
                           const std::string &name) const {
  if (!value.nullOrigin || name.empty())
    return;
  const core::NullOrigin &origin = *value.nullOrigin;
  switch (origin.reason) {
  case core::NullOrigin::Reason::Assigned:
    addNote(diagnostic, inQuotes(name) + " is assigned NULL here", origin.where);
    return;
  case core::NullOrigin::Reason::Tested:
    addNote(diagnostic,
            inQuotes(name) + " may be null: it is compared with NULL here",
            origin.where);
    return;
  case core::NullOrigin::Reason::Allocated:
    return;
  }
}

core::Diagnostic Transfer::nullArgument(const Expr &arg,
                                        const core::SymInfo &value,
                                        const std::string &callee,
                                        const FunctionDecl *declared) const {
  // A null constant has no name (`get(NULL)`).
  std::string name =
      arg.IgnoreParenCasts()->isNullPointerConstant(
          context, Expr::NPC_ValueDependentIsNotNull) != Expr::NPCK_NotNull
          ? std::string()
          : spell(arg);
  core::Diagnostic diagnostic =
      makeDiagnostic(core::diag::NullDereference,
                     (name.empty() ? std::string("a null pointer")
                                   : inQuotes(name) + ", which is null,") +
                         " is passed to " + callee + ", which dereferences it",
                     context, arg.getBeginLoc(), core::Severity::Error);
  addNullNote(diagnostic, value, name);
  // (Not for an implicitly declared builtin: its "declaration" is here.)
  if (declared != nullptr && !declared->isImplicit())
    addNote(
        diagnostic, callee + " is declared here",
        toCoreLocation(context.getSourceManager(), declared->getLocation()));
  return diagnostic;
}

void Transfer::allocationFailure(const core::SymInfo &value, const Expr &at,
                                 const Stmt &site) {
  if (!value.allocatorSource || !run.isPublishing())
    return;
  std::string callee = "an allocation";
  core::SourceLocation allocated;
  if (value.nullOrigin &&
      value.nullOrigin->reason == core::NullOrigin::Reason::Allocated) {
    if (!value.nullOrigin->detail.empty())
      callee = "the result of " + inQuotes(value.nullOrigin->detail);
    allocated = value.nullOrigin->where;
  }
  core::Diagnostic diagnostic = makeDiagnostic(
      core::diag::AllocationFailure,
      callee + " is used without a null test; it is null when allocation "
               "fails",
      context, at.getBeginLoc(), core::Severity::Warning);
  addNote(diagnostic, "allocated here", allocated);
  run.report(std::move(diagnostic), core::Certainty::Possible, &site,
             core::Facet::Null);
}

/// The name of parameter `index` of `callee` for messages.
static std::string parameterName(const FunctionDecl &callee, unsigned index) {
  for (const FunctionDecl *redecl : callee.redecls())
    if (index < redecl->getNumParams() &&
        !redecl->getParamDecl(index)->getName().empty())
      return "'" + redecl->getParamDecl(index)->getNameAsString() + "'";
  return "parameter " + std::to_string(index + 1);
}

void Decider::unknownCall(const CallExpr &call, bool callback) {
  const FunctionDecl *callee = call.getDirectCallee();
  std::string detail;
  std::optional<core::FixItHint> fixit;
  const SourceManager &sm = context.getSourceManager();
  std::optional<unsigned> uncovered;
  for (unsigned i = 0; i < call.getNumArgs(); ++i)
    if (call.getArg(i)->getType()->isPointerType()) {
      uncovered = i;
      break;
    }
  if (callee == nullptr) {
    detail = "the target of " + inQuotes(transfer.spell(*call.getCallee())) +
             " is unknown; annotate the parameters of its function type";
  } else if (uncovered && *uncovered < callee->getNumParams()) {
    std::string name = callee->getNameAsString();
    detail = "declare '" + name + "' with WEAVEC_BORROWED on " +
             parameterName(*callee, *uncovered) +
             " if it neither keeps nor frees it";
    const ParmVarDecl *param = callee->getFirstDecl()->getParamDecl(*uncovered);
    SourceLocation at = sm.getFileLoc(param->getLocation());
    if (at.isValid() && !sm.isInSystemHeader(at))
      fixit = core::FixItHint{.location = toCoreLocation(sm, at),
                              .insertion = "WEAVEC_BORROWED "};
  } else if (callee->getReturnType()->isPointerType()) {
    std::string name = callee->getNameAsString();
    detail = "declare the result of '" + name +
             "' WEAVEC_OWNED or WEAVEC_BORROWED, or define '" + name +
             "' in this program";
    SourceLocation at = sm.getFileLoc(callee->getFirstDecl()->getLocation());
    if (at.isValid() && !sm.isInSystemHeader(at))
      fixit = core::FixItHint{.location = toCoreLocation(sm, at),
                              .insertion = "WEAVEC_BORROWED "};
  } else {
    detail = "define '" + callee->getNameAsString() +
             "' in this program, or link a unit that has its WeaveC record";
  }
  decide(core::Facet::Temporal,
         core::FacetDecision::unresolvedFor(
             callback ? core::UnresolvedReason::Callback
                      : core::UnresolvedReason::UnknownCallee,
             std::move(detail)));
  if (fixit && run.isPublishing() && applies(core::Facet::Temporal))
    out.suggest(*site.stmt, site.kind, site.boundary, core::Facet::Temporal,
                std::move(*fixit));
}

//===----------------------------------------------------------------------===//
// Transfer entry points
//===----------------------------------------------------------------------===//

void Transfer::decideSites(const Stmt &stmt) {
  if (isa<CallExpr>(stmt))
    return;
  const SiteIndex &sites = run.sites();
  for (core::SiteId id : sites.sitesOf(stmt)) {
    const SiteInfo *info = sites.info(id);
    if (info == nullptr)
      continue;
    switch (info->kind) {
    case core::SiteKind::Raw:
      // (A raw release is the call's, below.)
      if (isa<CallExpr>(info->stmt))
        break;
      Decider(*this, *info).access();
      break;
    case core::SiteKind::Deref:
    case core::SiteKind::Index:
      Decider(*this, *info).access();
      break;
    case core::SiteKind::Call:
      if (info->boundary == core::Boundary::Exit)
        decideExitSite(stmt, isa<ReturnStmt>(stmt));
      break;
    default:
      break;
    }
  }
}

void Transfer::accessed(const Stmt &stmt) {
  const SiteIndex &sites = run.sites();
  // After a proven or checked dereference the pointer is non-null (RFC
  // 0030 §3.2), in every pass, so the blocks after it start from that.
  const Expr *base = nullptr;
  if (const auto *unary = dyn_cast<UnaryOperator>(&stmt);
      unary != nullptr && unary->getOpcode() == UO_Deref)
    base = unary->getSubExpr();
  else if (const auto *member = dyn_cast<MemberExpr>(&stmt);
           member != nullptr && member->isArrow())
    base = member->getBase();
  else if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(&stmt);
           subscript != nullptr &&
           subscript->getBase()->getType()->isPointerType())
    base = subscript->getBase();
  if (base != nullptr) {
    bool unsafeSite = false;
    for (core::SiteId id : sites.sitesOf(stmt))
      if (const SiteInfo *info = sites.info(id))
        unsafeSite = unsafeSite || info->inUnsafe;
    if (!unsafeSite) {
      core::Sym pointer = valueOf(*base);
      core::SymInfo &info = heap.infoMut(state, pointer);
      if (info.null != core::PointerNull::Null || run.isPublishing()) {
        if (info.type == core::SymInfo::Type::Pointer)
          info.null = core::PointerNull::NonNull;
        // (What arithmetic made from it, even from a value of no known
        // type: an untyped union cell.)
        heap.markNonNull(state, pointer);
      }
    }
  }
}

void Transfer::decideExitSite(const Stmt &stmt, bool isReturn) {
  const SiteIndex &sites = run.sites();
  auto id = sites.findExit(stmt);
  if (!id)
    return;
  const SiteInfo *info = sites.info(*id);
  if (info == nullptr)
    return;
  Decider decider(*this, *info);
  // A returned pointer to this activation's own storage (RFC 0002).
  if (isReturn && state.result != core::ZeroSym) {
    const core::SymInfo &value = heap.info(state, state.result);
    bool anyLocal = false;
    bool allLocal = !value.targets.empty();
    std::string local;
    core::SourceLocation declared;
    for (const core::Target &target : value.targets) {
      const core::ObjectInfo &objectInfo = run.table().info(target.object);
      // Storage of this frame (§5.7): a local, a parameter, a compound
      // literal, an `alloca` block.
      if (run.isFrameObject(state, target.object)) {
        anyLocal = true;
        if (local.empty()) {
          local = run.frameName(target.object);
          declared = objectInfo.created;
        }
      } else {
        allLocal = false;
      }
    }
    // A returned pointer into a released object is a use of it.
    if (!anyLocal && value.type == core::SymInfo::Type::Pointer &&
        value.null != core::PointerNull::Null) {
      const auto &ret = cast<ReturnStmt>(stmt);
      if (ret.getRetValue() != nullptr &&
          heap.temporal(state, state.result).kind !=
              core::TemporalVerdict::Kind::Proven &&
          heap.temporal(state, state.result).kind !=
              core::TemporalVerdict::Kind::MayAliasReleased)
        decider.temporalOf(state.result, *ret.getRetValue(), false);
    }
    if (anyLocal) {
      bool definite = allLocal && value.null == core::PointerNull::NonNull;
      decider.decide(core::Facet::Temporal,
                     definite ? core::FacetDecision::violation()
                              : core::FacetDecision::unresolvedFor(
                                    core::UnresolvedReason::MayDangle));
      const auto &ret = cast<ReturnStmt>(stmt);
      // RFC 0002: a returned variable is named; `return &x` is the
      // returned pointer.
      std::string holder = "returned pointer";
      if (ret.getRetValue() != nullptr)
        if (const auto *ref =
                dyn_cast<DeclRefExpr>(ret.getRetValue()->IgnoreParenImpCasts());
            ref != nullptr && isa<VarDecl>(ref->getDecl()))
          holder = inQuotes(ref->getDecl()->getNameAsString());
      core::Diagnostic diagnostic = makeDiagnostic(
          core::diag::LifetimeTooShort,
          holder + " may outlive " + inQuotes(local) + ", which it points to",
          run.ast(),
          ret.getRetValue() != nullptr ? ret.getRetValue()->getBeginLoc()
                                       : ret.getBeginLoc(),
          definite ? core::Severity::Error : core::Severity::Warning);
      addNote(diagnostic, inQuotes(local) + " is declared here", declared);
      run.report(std::move(diagnostic),
                 definite ? core::Certainty::Definite
                          : core::Certainty::Possible,
                 &stmt, core::Facet::Temporal);
      return;
    }
  }
  decider.decide(core::Facet::Temporal, core::FacetDecision::proven());
}

std::vector<const FunctionDecl *>
Transfer::callTargets(const CallExpr &call, core::Sym calleeValue) const {
  std::vector<const FunctionDecl *> targets;
  if (const FunctionDecl *direct = call.getDirectCallee())
    targets.push_back(direct);
  if (targets.empty() && calleeValue != core::ZeroSym) {
    const core::SymInfo &value = heap.info(state, calleeValue);
    if (value.type == core::SymInfo::Type::Function && value.functionsKnown)
      for (core::Handle handle : value.functions)
        if (const auto *fn = fromHandle<FunctionDecl>(handle))
          targets.push_back(fn);
  }
  if (targets.empty())
    if (auto resolution = slotResolution(call))
      targets = slotTargets(*resolution);
  return targets;
}

bool Transfer::calleeTouches(const CallExpr &call, core::Sym calleeValue,
                             unsigned index) const {
  std::vector<const FunctionDecl *> targets = callTargets(call, calleeValue);
  if (targets.empty())
    return true;
  for (const FunctionDecl *target : targets) {
    const core::FunctionEffects *effects =
        run.unitRun().summaryOf(*target->getCanonicalDecl());
    // (A library row names what it reads: `free` reads nothing.)
    if (effects == nullptr) {
      if (governingLibraryEntry(*target, run.unitRun().library()))
        continue;
      return true;
    }
    for (const auto *paths : {&effects->reads, &effects->writes})
      for (const core::SummaryPath &path : *paths)
        if (path.isParam() && path.index == index && !path.steps.empty() &&
            path.steps.front().step == core::PathStep::Deref)
          return true;
  }
  return false;
}

bool Transfer::calleeReleases(const CallExpr &call, core::Sym calleeValue,
                              const std::vector<core::Sym> &args,
                              unsigned index, bool possibly) const {
  std::vector<const FunctionDecl *> targets = callTargets(call, calleeValue);
  if (targets.empty())
    return false;
  // Whether one function releases the argument (`possibly`: on some
  // outcome or under some test is enough).
  auto releases = [&](const FunctionDecl &target) {
    const core::FunctionEffects *effects =
        run.unitRun().summaryOf(*target.getCanonicalDecl());
    if (effects == nullptr) {
      if (run.unitRun().hasBody(*target.getCanonicalDecl()))
        return false;
      // A library row that releases the argument (`free`).
      if (auto match = governingLibraryEntry(target, run.unitRun().library()))
        return index < match->entry->params.size() &&
               match->entry->params[index].effect ==
                   core::LibraryParam::Effect::Release;
      // RFC 0031 §5.4 (a declared contract), RFC 0010: a declaration whose
      // parameter is `WEAVEC_OWNED` or `WEAVEC_RELEASES` releases it.
      for (const FunctionDecl *redecl : target.redecls())
        if (index < redecl->getNumParams()) {
          AnnotationSet set = getAnnotations(*redecl->getParamDecl(index));
          if (set.owned || set.frees || set.releases)
            return true;
        }
      return false;
    }
    const core::SummaryPath released = core::SummaryPath::param(index).deref();
    for (const core::PathEffect &effect : effects->effects) {
      if (possibly && effect.kind == core::PathEffect::Kind::Release &&
          effect.path == released)
        return true;
      if (effect.kind != core::PathEffect::Kind::Release || effect.may ||
          !effect.when.classes.empty() || !(effect.path == released))
        continue;
      if (effect.when.paramZero) {
        auto [param, zero] = *effect.when.paramZero;
        auto argument = param < args.size() ? state.zone.constant(args[param])
                                            : std::nullopt;
        if (!argument || (*argument == 0) != zero)
          continue;
      }
      if (effect.when.paramsEqual &&
          argumentsEqual(args, *effect.when.paramsEqual) !=
              effect.when.paramsEqual->equal)
        continue;
      // (A case on a value at entry: not decided here.)
      if (effect.when.entryZero)
        continue;
      return true;
    }
    return false;
  };
  // Some target may release it; every target must, to say it does.
  if (possibly)
    return std::any_of(targets.begin(), targets.end(),
                       [&](const FunctionDecl *fn) { return releases(*fn); });
  return std::all_of(targets.begin(), targets.end(),
                     [&](const FunctionDecl *fn) { return releases(*fn); });
}

/// Whether ownership annotations on `callee`'s declarations cover every
/// pointer argument of `call` (and no library row governs it first).
static bool declaresEveryPointer(const FunctionDecl &callee,
                                 const CallExpr &call,
                                 const core::LibrarySpec &library) {
  if (governingLibraryEntry(callee, library))
    return false;
  SignatureAnnotations signature = collectAnnotations(callee);
  if (!signature.anyOwnership())
    return false;
  for (unsigned i = 0; i < call.getNumArgs(); ++i)
    if (call.getArg(i)->getType()->isPointerType() &&
        (i >= signature.params.size() || !signature.params[i].ownership()))
      return false;
  return true;
}

void Transfer::decideCall(const CallExpr &call,
                          const std::vector<core::Sym> &args,
                          core::Sym calleeValue) {
  const SiteIndex &sites = run.sites();
  const FunctionDecl *direct = call.getDirectCallee();
  UnitRun &unit = run.unitRun();
  // RFC 0003: what the callee's annotations do to this function's own.
  if (run.isPublishing())
    checkCallAnnotations(call, args);
  for (core::SiteId id : sites.sitesOf(call)) {
    const SiteInfo *info = sites.info(id);
    if (info == nullptr)
      continue;
    Decider decider(*this, *info);
    // Arguments that must not be null (the library row, declared or
    // inferred requirements).
    for (const ArgumentNeed &need : info->arguments) {
      if (need.argument >= args.size() || !need.nonnull || need.inferred)
        continue;
      const core::SymInfo &value = heap.info(state, args[need.argument]);
      core::FacetDecision decision;
      // RFC 0031 §5.4: a length that is zero accepts null (`memcpy(p, x,
      // 0)`): nothing to check.
      std::function<bool(const WitnessTerm &)> isZero =
          [&](const WitnessTerm &term) -> bool {
        switch (term.kind) {
        case WitnessTerm::Kind::Constant:
          return term.constant == 0;
        case WitnessTerm::Kind::Expr: {
          if (term.expr == nullptr)
            return false;
          auto c = state.zone.constant(valueOf(*term.expr));
          return c && *c == 0;
        }
        case WitnessTerm::Kind::Mul:
          return std::any_of(term.operands.begin(), term.operands.end(),
                             isZero);
        case WitnessTerm::Kind::Add:
          return !term.operands.empty() &&
                 std::all_of(term.operands.begin(), term.operands.end(),
                             isZero);
        default:
          return false;
        }
      };
      // RFC 0030 §8.3: a length that is non-zero for every value makes a
      // `null-if-zero` argument's null requirement definite.
      std::function<bool(const WitnessTerm &)> isNonZero =
          [&](const WitnessTerm &term) -> bool {
        switch (term.kind) {
        case WitnessTerm::Kind::Constant:
          return term.constant != 0;
        case WitnessTerm::Kind::SizeOf:
          return true;
        case WitnessTerm::Kind::Expr: {
          if (term.expr == nullptr)
            return false;
          core::Sym length = valueOf(*term.expr);
          auto lo = state.zone.lower(length);
          return (lo && *lo > 0) ||
                 (heap.info(state, length).nonZero && lo && *lo >= 0);
        }
        case WitnessTerm::Kind::Mul:
          return !term.operands.empty() &&
                 std::all_of(term.operands.begin(), term.operands.end(),
                             isNonZero);
        default:
          return false;
        }
      };
      bool nonZeroLength = !need.allowedIfZero ||
                           (need.unlessZero && isNonZero(*need.unlessZero));
      if (value.null == core::PointerNull::NonNull ||
          (need.allowedIfZero && need.unlessZero && isZero(*need.unlessZero)))
        decision = core::FacetDecision::proven();
      else if (need.systemApi)
        decision =
            core::FacetDecision::trustedFor(core::TrustReason::SystemApi);
      else if (value.null == core::PointerNull::Null && nonZeroLength &&
               !value.allocatorSource) {
        decision = core::FacetDecision::violation();
        std::string callee =
            direct != nullptr
                ? inQuotes(info->library ? info->library->entry->name
                                       : direct->getNameAsString())
                : "a function pointer";
        run.report(
            nullArgument(*call.getArg(need.argument), value, callee, direct),
            core::Certainty::Definite, &call, core::Facet::Null);
      } else {
        decision = core::FacetDecision::checked();
        if (run.applies(info->id, core::Facet::Null))
          allocationFailure(value, *call.getArg(need.argument), call);
      }
      if (run.isPublishing() && run.applies(info->id, core::Facet::Null)) {
        core::Requirement record;
        record.argument = need.argument;
        record.need = "nonnull";
        record.decision = decision;
        run.ledger().requirement(call, core::Facet::Null, record);
      }
    }
    // A library call's temporal facet is its arguments' uses: proven unless
    // one of them says otherwise (decisions keep the worst).
    if (info->kind == core::SiteKind::LibCall) {
      std::optional<core::FacetDecision> open;
      if (direct == nullptr)
        if (auto resolution = slotResolution(call))
          open = core::openCallTemporalDecision(*resolution);
      // RFC 0030 §8.2, §5.3: a row's statement about hidden behaviour (a
      // callback clause, a static slot) is trusted; a `sync` callback whose
      // target is unknown is the unknown-callee default.
      if (!open && info->library) {
        const core::LibraryMatch &match = *info->library;
        bool unknownTarget = false;
        for (unsigned i = 0; i < call.getNumArgs() && i < args.size(); ++i)
          if (const core::LibraryParam *param = match.param(i);
              param != nullptr && param->callback &&
              param->callback->kind == core::LibCallback::Kind::Sync &&
              syncTargets(args[i]).empty())
            unknownTarget = true;
        if (unknownTarget)
          open = core::FacetDecision::unresolvedFor(
              core::UnresolvedReason::Callback, "the callback of " +
                                                    inQuotes(match.entry->name) +
                                                    " is not known here");
        else if (match.entry->trustsLibrarySpec())
          open =
              core::FacetDecision::trustedFor(core::TrustReason::LibrarySpec);
      }
      decider.decide(core::Facet::Temporal,
                     open ? *open : core::FacetDecision::proven());
      if (info->library)
        for (const std::string &slot : info->library->entry->reads)
          decider.readsState(slot);
    }
    // Every pointer argument is a use of its object (the callee may read
    // it), except the one a release consumes, which the release decides
    // (a raw one too, RFC 0004).
    const bool releaseSite =
        info->kind == core::SiteKind::Release ||
        (info->kind == core::SiteKind::Raw && isa<CallExpr>(info->stmt));
    if (!releaseSite && !(info->kind == core::SiteKind::Call &&
                          info->boundary == core::Boundary::Exit))
      for (unsigned i = 0; i < call.getNumArgs() && i < args.size(); ++i) {
        const Expr &arg = *call.getArg(i);
        if (!arg.getType()->isPointerType() || args[i] == core::ZeroSym)
          continue;
        const core::SymInfo &value = heap.info(state, args[i]);
        if (value.type != core::SymInfo::Type::Pointer ||
            value.null == core::PointerNull::Null)
          continue;
        core::TemporalVerdict verdict = heap.temporal(state, args[i]);
        // A second release when the callee releases it, or may where the
        // argument is only possibly released already, or may and reads
        // nothing through it first; otherwise the first invalid operation
        // is the callee's use of it.
        if (verdict.kind != core::TemporalVerdict::Kind::Proven)
          decider.temporalOf(
              args[i], arg,
              calleeReleases(call, calleeValue, args, i) ||
                  ((verdict.kind == core::TemporalVerdict::Kind::MayReleased ||
                    (verdict.kind == core::TemporalVerdict::Kind::Violation &&
                     !calleeTouches(call, calleeValue, i))) &&
                   calleeReleases(call, calleeValue, args, i, true)));
      }
    if (releaseSite && info->operand != nullptr)
      for (unsigned i = 0; i < call.getNumArgs() && i < args.size(); ++i) {
        const Expr &arg = *call.getArg(i);
        if (&arg == info->operand ||
            arg.IgnoreParenImpCasts() == info->operand->IgnoreParenImpCasts())
          continue;
        if (!arg.getType()->isPointerType() || args[i] == core::ZeroSym)
          continue;
        const core::SymInfo &value = heap.info(state, args[i]);
        if (value.type != core::SymInfo::Type::Pointer ||
            value.null == core::PointerNull::Null)
          continue;
        if (heap.temporal(state, args[i]).kind !=
            core::TemporalVerdict::Kind::Proven)
          decider.temporalOf(args[i], arg, false);
      }
    switch (info->kind) {
    case core::SiteKind::Raw:
    case core::SiteKind::Release: {
      if (info->operand == nullptr)
        break;
      core::Sym pointer = valueOf(*info->operand);
      std::string family;
      if (info->library)
        for (const core::LibraryParam &param : info->library->entry->params)
          if (param.effect == core::LibraryParam::Effect::Release ||
              param.effect == core::LibraryParam::Effect::Realloc)
            family = param.family.empty() ? std::string(core::HeapFamily)
                                          : param.family;
      decider.release(pointer, family);
      if (run.isPublishing())
        checkConsumeAnnotation(pointer, *info->operand, call, /*moved=*/false);
      break;
    }
    case core::SiteKind::LibCall:
      // Requirement records are decided by the library rules.
      decideLibraryCall(call, *info, args);
      break;
    case core::SiteKind::Call: {
      if (info->boundary == core::Boundary::Exit)
        break;
      // The callee operand of an indirect call.
      if (direct == nullptr) {
        const core::SymInfo &target = heap.info(state, calleeValue);
        if (target.type == core::SymInfo::Type::Pointer &&
            target.null == core::PointerNull::Null && !target.allocatorSource &&
            !target.mayUninit) {
          // A call through a null function pointer (RFC 0030 §3.2).
          decider.decide(core::Facet::Null, core::FacetDecision::violation());
          const Expr &callee = *call.getCallee();
          std::string name = spell(callee);
          core::Diagnostic diagnostic = makeDiagnostic(
              core::diag::NullDereference,
              "dereference of " + inQuotes(name) + ", which is null", run.ast(),
              callee.getBeginLoc(), core::Severity::Error);
          addNullNote(diagnostic, target, name);
          decider.report(std::move(diagnostic), core::Certainty::Definite,
                         core::Facet::Null);
        } else {
          decider.decide(core::Facet::Null,
                         target.type == core::SymInfo::Type::Function &&
                                 target.functionsKnown &&
                                 !target.functions.empty()
                             ? core::FacetDecision::proven()
                             : core::FacetDecision::checked());
        }
      }
      // RFC 0004: a raw pointer handed to a callee that takes a tracked
      // pointer there (or its ownership), outside an unsafe region.
      if (direct != nullptr && !info->inUnsafe && run.isPublishing())
        for (unsigned i = 0; i < call.getNumArgs() && i < args.size() &&
                             i < direct->getNumParams();
             ++i) {
          const core::SymInfo value = heap.info(state, args[i]);
          if (value.type != core::SymInfo::Type::Pointer || !value.raw ||
              value.rawSome)
            continue;
          AnnotationSet set;
          for (const FunctionDecl *redecl : direct->redecls())
            if (i < redecl->getNumParams())
              set.merge(getAnnotations(*redecl->getParamDecl(i)));
          if (set.raw || set.unsafe)
            continue;
          const Expr &argument = *call.getArg(i);
          std::string name =
              !isa<ExplicitCastExpr>(argument.IgnoreParenImpCasts())
                  ? spell(argument)
                  : std::string();
          core::Diagnostic diagnostic = makeDiagnostic(
              core::diag::UnsafeOperation,
              inQuotes(direct->getNameAsString()) +
                  (set.owned ? " takes ownership of raw pointer "
                             : " dereferences raw pointer ") +
                  (name.empty() ? std::string() : inQuotes(name) + " ") +
                  "outside an unsafe region",
              run.ast(), argument.getBeginLoc(), core::Severity::Error);
          addNote(diagnostic, rawNote(value, name), value.rawAt);
          diagnostic.addNote(UnsafeFixIt, core::SourceLocation{});
          decider.report(std::move(diagnostic), core::Certainty::Definite,
                         core::Facet::Spatial);
        }
      decideCallKinds(call, *info, args);
      // The temporal facet by what the callee is.
      if (direct != nullptr) {
        const FunctionDecl *canonical = direct->getCanonicalDecl();
        if (unit.summaryOf(*canonical) != nullptr || unit.hasBody(*canonical)) {
          decider.decide(core::Facet::Temporal, core::FacetDecision::proven());
        } else if (const OwnershipContract *contract =
                       unit.input.kinds.ownership(*direct);
                   contract != nullptr && !contract->empty()) {
          decider.decide(core::Facet::Temporal,
                         core::FacetDecision::trustedFor(
                             core::TrustReason::ExternContract));
        } else if (declaresEveryPointer(*direct, call, unit.library())) {
          // RFC 0003, RFC 0030 §5.1: ownership annotations on every pointer
          // parameter are the callee's contract.
          decider.decide(core::Facet::Temporal,
                         core::FacetDecision::trustedFor(
                             core::TrustReason::ExternContract));
        } else if (isPlatformDeclaration(*direct, unit.library(),
                                         run.ast().getSourceManager())) {
          decider.decide(
              core::Facet::Temporal,
              core::FacetDecision::trustedFor(core::TrustReason::SystemApi));
        } else if (governingLibraryEntry(*direct, unit.library())) {
          decider.decide(
              core::Facet::Temporal,
              core::FacetDecision::trustedFor(core::TrustReason::LibrarySpec));
        } else {
          bool anyPointer = false;
          for (const Expr *arg : call.arguments())
            anyPointer = anyPointer || arg->getType()->isPointerType();
          if (anyPointer || call.getType()->isPointerType())
            decider.unknownCall(call, false);
          else
            decider.decide(core::Facet::Temporal,
                           core::FacetDecision::unresolvedFor(
                               core::UnresolvedReason::UnknownCallee,
                               "define '" + direct->getNameAsString() +
                                   "' in this program, or link a unit that "
                                   "has its WeaveC record"));
        }
      } else {
        const core::SymInfo &target = heap.info(state, calleeValue);
        std::optional<core::CallResolution> resolution;
        if (!(target.type == core::SymInfo::Type::Function &&
              target.functionsKnown && !target.functions.empty()))
          resolution = slotResolution(call);
        if (target.type == core::SymInfo::Type::Function &&
            target.functionsKnown && !target.functions.empty())
          decider.decide(core::Facet::Temporal, core::FacetDecision::proven());
        else if (resolution &&
                 (resolution->kind == core::IndirectCallKind::ClosedSingle ||
                  resolution->kind == core::IndirectCallKind::ClosedJoin))
          decider.decide(core::Facet::Temporal, core::FacetDecision::proven());
        else if (resolution)
          if (auto decision = core::openCallTemporalDecision(*resolution))
            decider.decide(core::Facet::Temporal, *decision);
          else
            decider.unknownCall(call, true);
        else
          decider.unknownCall(call, true);
      }
      break;
    }
    default:
      break;
    }
  }
}

} // namespace weavec::analysis::engine
