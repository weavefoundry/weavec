//===- EngineAnnotations.cpp - Annotation mismatches (object engine) ------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `annotation-mismatch` (RFC 0030 §3.4: always an error) in the object
// domain: a definition checked against its own declaration.
//
// - RFC 0003 *Reconciliation*: a `WEAVEC_BORROWED` or `WEAVEC_MUT`
//   parameter's object released or moved, a `WEAVEC_BORROWED` one written
//   through, and a result annotated `WEAVEC_OWNED` that is a borrow or
//   `WEAVEC_BORROWED` that is a fresh allocation. Callers keep trusting the
//   annotation; the definition is where the error is.
// - RFC 0012 *Sized fields*, "Stores": a pointer field declared
//   `WEAVEC_SIZED_BY(n)` or `WEAVEC_COUNTED_BY(n)` given an object that the
//   sibling count says is larger than it is exactly.
//
// A parameter's object is the entry object its value points to (`param(i)*`,
// RFC 0031 §4.6), whatever copy of the pointer reaches it.
//
//===----------------------------------------------------------------------===//

#include "Engine.h"
#include "weavec/Analysis/Annotations.h"
#include "weavec/Analysis/ClangLocation.h"
#include "weavec/Analysis/KindTable.h"

#include "clang/AST/RecordLayout.h"
#include "clang/Basic/SourceManager.h"

using namespace clang;

namespace weavec::analysis::engine {

/// The parameter whose object `object` is (`param(i)*`), or the one it is
/// reached from through that object (`param(i)*.data*`, `below`).
static const ParmVarDecl *parameterOf(const FunctionDecl &function,
                                      const core::ObjectInfo &info,
                                      bool &below) {
  below = false;
  if (info.key.kind != core::ObjectKind::Entry || !info.key.path.isParam() ||
      info.key.path.steps.empty() ||
      info.key.path.steps.front().step != core::PathStep::Deref ||
      info.key.path.index >= function.getNumParams())
    return nullptr;
  below = info.key.path.steps.size() > 1;
  return function.getParamDecl(info.key.path.index);
}

static std::string_view macroOf(const AnnotationSet &set) {
  return set.borrowed ? "WEAVEC_BORROWED" : "WEAVEC_MUT";
}

void Transfer::reportMismatch(std::string message, SourceLocation at,
                              std::string note, SourceLocation noteAt,
                              std::string copy, const Stmt &site,
                              std::optional<core::Facet> facet) {
  core::Diagnostic diagnostic;
  diagnostic.id = core::diag::AnnotationMismatch;
  diagnostic.severity = core::Severity::Error;
  diagnostic.message = std::move(message);
  const SourceManager &sm = context.getSourceManager();
  diagnostic.location = toCoreLocation(sm, at);
  if (noteAt.isValid())
    diagnostic.addNote(std::move(note), toCoreLocation(sm, noteAt));
  if (!copy.empty())
    diagnostic.addNote(std::move(copy), toCoreLocation(sm, at));
  run.report(std::move(diagnostic), core::Certainty::Definite, &site, facet);
}

void Transfer::checkConsumeAnnotation(core::Sym value, const Expr &operand,
                                      const Stmt &site, bool moved) {
  if (!run.isPublishing())
    return;
  const core::SymInfo &info = heap.info(state, value);
  if (info.type != core::SymInfo::Type::Pointer || info.top ||
      info.targets.size() != 1 || info.null == core::PointerNull::Null)
    return;
  bool below = false;
  const ParmVarDecl *param =
      parameterOf(run.decl(), run.table().info(info.targets[0].object), below);
  if (param == nullptr)
    return;
  SignatureAnnotations signature = collectAnnotations(run.decl());
  unsigned index = param->getFunctionScopeIndex();
  if (index >= signature.params.size())
    return;
  const AnnotationSet &set = signature.params[index];
  if (!set.borrowed && !set.mutBorrowed)
    return;
  std::string name = param->getNameAsString();
  std::string verb = moved ? "moved" : "freed";
  std::string spelled = spell(operand);
  if (below) {
    // Releasing what the borrowed object owns mutates it.
    if (!set.borrowed)
      return;
    reportMismatch("'" + name + "' is annotated " + std::string(macroOf(set)) +
                       " but '" + spelled + "' is " + verb + " here",
                   site.getBeginLoc(), "'" + name + "' is annotated here",
                   param->getLocation(), "", site);
    return;
  }
  if (!info.targets[0].offset.isConstant() ||
      info.targets[0].offset.constant != 0)
    return;
  reportMismatch("'" + name + "' is annotated " + std::string(macroOf(set)) +
                     " but is " + verb + " here",
                 site.getBeginLoc(), "'" + name + "' is annotated here",
                 param->getLocation(),
                 spelled != name
                     ? "'" + spelled + "' is a copy of '" + name + "'"
                     : std::string(),
                 site);
}

void Transfer::checkWriteAnnotation(const Address &address, const Stmt &site) {
  if (!run.isPublishing() || address.top || address.targets.size() != 1)
    return;
  bool below = false;
  const ParmVarDecl *param = parameterOf(
      run.decl(), run.table().info(address.targets[0].object), below);
  if (param == nullptr || below)
    return;
  SignatureAnnotations signature = collectAnnotations(run.decl());
  unsigned index = param->getFunctionScopeIndex();
  if (index >= signature.params.size() || !signature.params[index].borrowed)
    return;
  std::string name = param->getNameAsString();
  reportMismatch("'" + name +
                     "' is annotated WEAVEC_BORROWED but is written "
                     "through here",
                 site.getBeginLoc(), "'" + name + "' is annotated here",
                 param->getLocation(), "", site);
}

void Transfer::checkCallAnnotations(const CallExpr &call,
                                    const std::vector<core::Sym> &args) {
  const FunctionDecl *callee = call.getDirectCallee();
  if (callee == nullptr || !run.isPublishing())
    return;
  SignatureAnnotations signature = collectAnnotations(*callee);
  for (unsigned i = 0;
       i < call.getNumArgs() && i < args.size() && i < signature.params.size();
       ++i) {
    const AnnotationSet &set = signature.params[i];
    if (!call.getArg(i)->getType()->isPointerType())
      continue;
    if (set.owned) {
      checkConsumeAnnotation(args[i], *call.getArg(i), call, /*moved=*/true);
      continue;
    }
    if (set.mutBorrowed) {
      const core::SymInfo &value = heap.info(state, args[i]);
      if (value.type != core::SymInfo::Type::Pointer || value.top)
        continue;
      Address address;
      address.targets = value.targets;
      checkWriteAnnotation(address, call);
    }
  }
}

void Transfer::checkReturnAnnotation(core::Sym value, const Expr &returned) {
  if (!run.isPublishing() || value == core::ZeroSym)
    return;
  SignatureAnnotations signature = collectAnnotations(run.decl());
  const AnnotationSet &result = signature.result;
  bool promisesBorrow = result.borrowed || result.mutBorrowed;
  if (!result.owned && !promisesBorrow)
    return;
  const core::SymInfo &info = heap.info(state, value);
  if (info.type != core::SymInfo::Type::Pointer || info.top ||
      info.targets.empty() || info.null == core::PointerNull::Null)
    return;
  bool borrow = true;
  bool fresh = true;
  for (const core::Target &target : info.targets) {
    const core::ObjectInfo &object = run.table().info(target.object);
    const core::ObjectState *objectState =
        heap.findObject(state, target.object);
    // A borrow: the object of a parameter the caller keeps (`b`,
    // `&b->data`), a global or a literal.
    bool below = false;
    const ParmVarDecl *param = parameterOf(run.decl(), object, below);
    bool interior = param != nullptr && !below &&
                    param->getFunctionScopeIndex() < signature.params.size() &&
                    !signature.params[param->getFunctionScopeIndex()].owned;
    borrow =
        borrow && (interior || object.key.kind == core::ObjectKind::Global ||
                   object.key.kind == core::ObjectKind::Literal);
    fresh = fresh &&
            (object.key.kind == core::ObjectKind::HeapRecent ||
             object.key.kind == core::ObjectKind::HeapOld) &&
            objectState != nullptr && objectState->owned &&
            !objectState->escaped;
  }
  std::string message;
  if (result.owned && borrow)
    message = "function returns a borrow but its return type is annotated "
              "WEAVEC_OWNED";
  else if (promisesBorrow && fresh)
    message = std::string("function returns a fresh allocation but its return "
                          "type is annotated ") +
              (result.borrowed ? "WEAVEC_BORROWED" : "WEAVEC_MUT");
  else
    return;
  reportMismatch(std::move(message), returned.getBeginLoc(), "annotated here",
                 run.decl().getLocation(), "", returned);
}

//===----------------------------------------------------------------------===//
// Malformed annotations (RFC 0003, RFC 0010, RFC 0012)
//===----------------------------------------------------------------------===//

void FunctionRun::validateAnnotations() {
  const SourceManager &sm = context.getSourceManager();
  auto warn = [&](std::string message, SourceLocation at) {
    core::Diagnostic diagnostic;
    diagnostic.id = core::diag::InvalidAnnotation;
    diagnostic.severity = core::Severity::Warning;
    diagnostic.message = std::move(message);
    diagnostic.location = toCoreLocation(sm, at);
    out.report(std::move(diagnostic), core::Certainty::Possible);
  };
  // `WEAVEC_OWNED_BY` needs `WEAVEC_OWNED`; retaining and releasing one
  // argument contradict each other.
  auto contradictions = [&](const NamedDecl &decl, const AnnotationSet &set) {
    if (set.retains && set.releases)
      warn("'" + decl.getNameAsString() +
               "' is declared both WEAVEC_RETAINS and WEAVEC_RELEASES",
           decl.getLocation());
    if (!set.family.empty() && !set.owned)
      warn("'" + decl.getNameAsString() + "' is declared WEAVEC_OWNED_BY(" +
               set.family + ") without WEAVEC_OWNED",
           decl.getLocation());
  };
  const AnnotationSet annotations = getAnnotations(function);
  if (annotations.invalid)
    warn("unrecognised weavec annotation on '" + function.getNameAsString() +
             "'",
         function.getLocation());
  contradictions(function, annotations);
  // `weavec.assume` belongs to the header's `weavec_assume_` alone.
  if (annotations.assume && function.getName() != "weavec_assume_")
    warn("'weavec.assume' is not an annotation for '" +
             function.getNameAsString() + "'",
         function.getLocation());
  for (const ParmVarDecl *param : function.parameters()) {
    const AnnotationSet onParam = getAnnotations(*param);
    contradictions(*param, onParam);
    if (onParam.invalid)
      warn("unrecognised weavec annotation on '" + param->getNameAsString() +
               "'",
           param->getLocation());
  }
}

//===----------------------------------------------------------------------===//
// Sized fields (RFC 0012)
//===----------------------------------------------------------------------===//

void Transfer::checkSizedFieldStore(const MemberExpr &lhs,
                                    const Address &address) {
  if (!run.isPublishing() || address.top || address.targets.size() != 1 ||
      !address.targets[0].offset.isConstant())
    return;
  const auto *stored = dyn_cast<FieldDecl>(lhs.getMemberDecl());
  if (stored == nullptr)
    return;
  const RecordDecl *record = stored->getParent();
  if (record == nullptr || !record->isCompleteDefinition() || record->isUnion())
    return;
  const KindTable &kinds = run.unitRun().input.kinds;
  // The annotated pointer field and its count: the stored field is one.
  const FieldDecl *pointerField = nullptr;
  const FieldDecl *countField = nullptr;
  const KindEntry *kind = nullptr;
  for (const FieldDecl *field : record->fields()) {
    const KindEntry *entry = kinds.field(*field);
    if (entry == nullptr || !entry->hasDeclaredShape() ||
        entry->shapeFromSystemHeader() ||
        (entry->kind.shape != core::PointerShape::Sized &&
         entry->kind.shape != core::PointerShape::Counted) ||
        entry->kind.extent.isConstant() ||
        entry->kind.extent.path->root != core::ExtentPath::Root::Field)
      continue;
    const FieldDecl *count = nullptr;
    for (const FieldDecl *sibling : record->fields())
      if (sibling->getName() == entry->kind.extent.path->field)
        count = sibling;
    if (count == nullptr || !count->getType()->isIntegerType())
      continue;
    if (field == stored || count == stored) {
      pointerField = field;
      countField = count;
      kind = entry;
      break;
    }
  }
  if (kind == nullptr)
    return;
  const ASTRecordLayout &layout = context.getASTRecordLayout(record);
  auto offsetOf = [&](const FieldDecl &field) {
    return static_cast<std::int64_t>(
        layout.getFieldOffset(field.getFieldIndex()) / context.getCharWidth());
  };
  core::ObjectId object = address.targets[0].object;
  std::int64_t base = address.targets[0].offset.constant - offsetOf(*stored);
  auto pointer = heap.read(
      state, object, core::CellKey{.offset = base + offsetOf(*pointerField)});
  auto count = heap.read(state, object,
                         core::CellKey{.offset = base + offsetOf(*countField)});
  if (!pointer || !count)
    return;
  const core::SymInfo &value = heap.info(state, *pointer);
  if (value.type != core::SymInfo::Type::Pointer || value.top ||
      value.null == core::PointerNull::Null || value.targets.size() != 1 ||
      !(value.targets[0].offset == core::Term::of(0)))
    return;
  const core::ObjectState *target =
      heap.findObject(state, value.targets[0].object);
  if (target == nullptr || !target->extent || !target->extent->bytes.known ||
      target->extent->cls != core::ExtentClass::Exact)
    return;
  std::int64_t unit = 1;
  if (kind->kind.shape == core::PointerShape::Counted) {
    QualType pointee = pointerField->getType()->getPointeeType();
    if (pointee.isNull() || pointee->isIncompleteType() ||
        pointee->isVoidType())
      return;
    unit = static_cast<std::int64_t>(
        context.getTypeSizeInChars(pointee).getQuantity());
  }
  core::Term says = termOf(*count);
  if (!says.known)
    return;
  says.scale *= kind->kind.extent.scale;
  says.constant =
      (says.constant * kind->kind.extent.scale) + kind->kind.extent.offset;
  core::Term need = says;
  need.scale *= unit;
  need.constant *= unit;
  if (need.isConstant())
    need.scale = 0;
  // Only a decided shortfall against an exact extent is a mismatch.
  if (heap.lessEqual(state, need, target->extent->bytes) != false)
    return;
  std::string baseName = spell(*lhs.getBase());
  std::string arrow = lhs.isArrow() ? "->" : ".";
  std::string fieldName = baseName + arrow + pointerField->getNameAsString();
  std::string countName = baseName + arrow + countField->getNameAsString();
  std::string saysText;
  if (says.isConstant())
    saysText = std::to_string(says.constant);
  else if (auto amount = spellAmount(says, /*own=*/false))
    saysText = amount->substr(0, amount->size() - std::string(" bytes").size());
  else
    saysText = "more";
  if (unit != 1)
    saysText += " elements of " + std::to_string(unit) + " bytes";
  std::string macro = kind->kind.shape == core::PointerShape::Sized
                          ? "WEAVEC_SIZED_BY"
                          : "WEAVEC_COUNTED_BY";
  reportMismatch(
      "'" + fieldName + "' is declared " + macro + "(" +
          countField->getNameAsString() + ") but is given " +
          spellAmount(target->extent->bytes, false).value_or("fewer bytes") +
          " where '" + countName + "' says " + saysText,
      lhs.getBeginLoc(), "'" + fieldName + "' is declared here",
      pointerField->getLocation(), "", lhs, std::nullopt);
}

} // namespace weavec::analysis::engine
