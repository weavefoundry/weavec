//===- DataflowSafety.cpp - Checked operation accounting (RFC 0018) -------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"
#include "IntegerSupport.h"
#include "weavec/Analysis/ProgramDatabase.h"

#include "clang/AST/Attr.h"
#include "clang/Basic/SourceManager.h"

using namespace clang;

namespace weavec::analysis {

void FunctionDataflow::safetyObligation(
    core::SafetyProperty property, core::SafetyOutcome outcome, const Stmt &at,
    std::string subject, std::string reason,
    std::vector<core::SourceLocation> calls) {
  if (!options.checkContracts || !recording())
    return;
  inferred.checked.obligations.add(
      {.property = property,
       .outcome = inUnsafe ? core::SafetyOutcome::Trusted : outcome,
       .location = locate(at),
       .function = function.getNameAsString(),
       .subject = std::move(subject),
       .reason = inUnsafe ? "unsafe boundary: " + reason : std::move(reason),
       .calls = std::move(calls)});
}

void FunctionDataflow::safetyDiagnostic(const core::Diagnostic &diagnostic) {
  if (diagnostic.id == core::diag::CheckingIncomplete ||
      diagnostic.id == core::diag::CheckingFailed)
    return;
  const bool incomplete = diagnostic.id == core::diag::AnalysisIncomplete ||
                          diagnostic.id == core::diag::AnnotationRequired;
  if (!incomplete && diagnostic.severity != core::Severity::Error &&
      diagnostic.id != core::diag::Leak &&
      diagnostic.id != core::diag::InvalidAnnotation)
    return;
  auto outcome = incomplete ? core::SafetyOutcome::Unresolved
                            : core::SafetyOutcome::Violation;
  if (inUnsafe)
    outcome = core::SafetyOutcome::Trusted;
  core::SafetyObligation obligation{
      .property = incomplete ? core::SafetyProperty::Semantics
                             : core::SafetyProperty::Validity,
      .outcome = outcome,
      .location = diagnostic.location,
      .function = function.getNameAsString(),
      .subject = std::string(diagnostic.id),
      .reason = diagnostic.message,
      .calls = {}};
  for (const auto &note : diagnostic.notes)
    if (obligation.calls.size() < core::MaxSafetyCallDepth)
      obligation.calls.push_back(note.location);
  inferred.checked.obligations.add(std::move(obligation));
}

static bool checkedAllocationOrigin(const ValueOrigin &origin,
                                    unsigned depth = 0) {
  if (depth > 16)
    return false;
  if (origin.kind == ValueOrigin::Kind::Alloc ||
      origin.kind == ValueOrigin::Kind::Null)
    return true;
  return origin.kind == ValueOrigin::Kind::Conditional &&
         !origin.alternatives.empty() &&
         std::ranges::all_of(origin.alternatives, [&](const auto &alternative) {
           return checkedAllocationOrigin(alternative, depth + 1);
         });
}

static bool checkedTypeUnsupported(QualType type, unsigned depth = 0) {
  if (type.isNull() || depth > core::MaxHeapPathDepth)
    return true;
  if (type.isVolatileQualified() || type->isAtomicType() ||
      type->isVectorType())
    return true;
  if (const auto *array = type->getAsArrayTypeUnsafe())
    return checkedTypeUnsupported(array->getElementType(), depth + 1);
  if (const auto *record = type->getAsRecordDecl()) {
    if (record->isUnion())
      return true;
    if (record->isCompleteDefinition())
      for (const auto *field : record->fields())
        if (field->isBitField() ||
            checkedTypeUnsupported(field->getType(), depth + 1))
          return true;
  }
  // A pointer is inspected when its referent is accessed, avoiding recursive
  // type expansion and rejection of an unused opaque pointer declaration.
  return false;
}

static bool hasCheckedAssumptions(const AnnotationSet &annotations) {
  return annotations.ownership() || annotations.nullness() ||
         !annotations.family.empty() || !annotations.sizedBy.empty();
}

void FunctionDataflow::initializeChecked() {
  inferred.checked.computed = true;
  inferred.checked.signature = functionTypeKey(function.getType(), context);
  inferred.checked.selected =
      (options.checked &&
       (!options.checkedMainFileOnly ||
        context.getSourceManager().isInMainFile(function.getLocation()))) ||
      options.checkedFunctions.contains(function.getNameAsString()) ||
      getAnnotations(function).checked;
  const bool annotated =
      hasCheckedAssumptions(getAnnotations(function)) ||
      std::ranges::any_of(function.parameters(), [](const ParmVarDecl *param) {
        return hasCheckedAssumptions(getAnnotations(*param));
      });
  if (annotated)
    inferred.checked.obligations.add(
        {.property = core::SafetyProperty::Call,
         .outcome = core::SafetyOutcome::Trusted,
         .location = locate(*function.getBody()),
         .function = function.getNameAsString(),
         .subject = "declaration annotations",
         .reason = "declared ownership annotation assumptions",
         .calls = {}});
  for (const auto *param : function.parameters())
    if (getAnnotations(*param).checked || getAnnotations(*param).invalid)
      checkedUnsupported.insert(function.getBody());
  if (getAnnotations(function).invalid)
    checkedUnsupported.insert(function.getBody());
  std::vector<const Stmt *> pendingStmts{function.getBody()};
  for (std::size_t i = 0; i < pendingStmts.size(); ++i) {
    const Stmt *stmt = pendingStmts[i];
    if (!stmt)
      continue;
    if (pendingStmts.size() > 65536) {
      inferred.checked.limited = true;
      break;
    }
    if (isa<AsmStmt, IndirectGotoStmt, GotoStmt, AddrLabelExpr, VAArgExpr,
            AtomicExpr>(stmt))
      checkedUnsupported.insert(stmt);
    if (const auto *expr = dyn_cast<Expr>(stmt)) {
      if (const auto *cast = dyn_cast<CastExpr>(expr);
          cast && cast->getCastKind() == CK_FloatingToIntegral)
        checkedUnsupported.insert(stmt);
      if (const auto *cast = dyn_cast<CastExpr>(expr);
          cast && cast->getCastKind() == CK_BitCast &&
          cast->getType()->isPointerType() &&
          cast->getSubExpr()->getType()->isPointerType()) {
        const auto from = cast->getSubExpr()->getType()->getPointeeType();
        const auto to = cast->getType()->getPointeeType();
        if (!to->isVoidType() && !to->isCharType() &&
            !clang::ASTContext::hasSameUnqualifiedType(from, to) &&
            !checkedAllocationOrigin(builder.classifyValue(*cast)))
          checkedUnsupported.insert(stmt);
      }
      if (checkedTypeUnsupported(expr->getType()))
        checkedUnsupported.insert(stmt);
      // Explicit expression allowlist: a new Clang expression cannot silently
      // become checked merely because the ordinary checker ignores it.
      if (!isa<ArraySubscriptExpr, BinaryOperator, UnaryOperator, CallExpr,
               ParenExpr, CastExpr, DeclRefExpr, MemberExpr, IntegerLiteral,
               CharacterLiteral, StringLiteral, FloatingLiteral, InitListExpr,
               ImplicitValueInitExpr, CompoundLiteralExpr,
               UnaryExprOrTypeTraitExpr, AbstractConditionalOperator,
               OpaqueValueExpr, PredefinedExpr, ConstantExpr, GNUNullExpr,
               ChooseExpr, GenericSelectionExpr>(expr))
        checkedUnsupported.insert(stmt);
    }
    if (const auto *declarations = dyn_cast<DeclStmt>(stmt))
      for (const auto *decl : declarations->decls())
        if (const auto *var = dyn_cast<VarDecl>(decl);
            var && (checkedTypeUnsupported(var->getType()) ||
                    getAnnotations(*var).checked))
          checkedUnsupported.insert(stmt);
    if (const auto *loop = dyn_cast<ForStmt>(stmt)) {
      std::vector<const Stmt *> body{loop->getBody()};
      for (std::size_t j = 0; j < body.size() && body.size() < 65536; ++j) {
        if (!body[j])
          continue;
        checkedLoops[body[j]] = loop;
        for (const auto *child : body[j]->children())
          body.push_back(child);
      }
    }
    // sizeof's ordinary operand is not evaluated. VLA sizes appear separately
    // in the CFG; their unsupported capture already creates coverage evidence.
    if (const auto *trait = dyn_cast<UnaryExprOrTypeTraitExpr>(stmt);
        trait &&
        (trait->isArgumentType() ||
         !trait->getArgumentExpr()->getType()->isVariablyModifiedType()))
      continue;
    for (const auto *child : stmt->children())
      pendingStmts.push_back(child);
  }
}

void FunctionDataflow::checkedBefore(const Stmt &stmt,
                                     core::AnalysisState &state) {
  if (const auto *declarations = dyn_cast<DeclStmt>(&stmt))
    for (const auto *decl : declarations->decls())
      if (const auto *var = dyn_cast<VarDecl>(decl);
          var && hasCheckedAssumptions(getAnnotations(*var)))
        safetyObligation(core::SafetyProperty::Call,
                         core::SafetyOutcome::Trusted, stmt,
                         "annotation:" + var->getNameAsString(),
                         "declared local annotation assumptions");
  const Expr *written = nullptr;
  if (const auto *binary = dyn_cast<BinaryOperator>(&stmt);
      binary && binary->isAssignmentOp())
    written = binary->getLHS();
  if (const auto *unary = dyn_cast<UnaryOperator>(&stmt);
      unary && unary->isIncrementDecrementOp())
    written = unary->getSubExpr();
  if (written)
    if (const auto ref = builder.resolve(*written))
      for (auto &[place, ranges] : state.safety->memory) {
        (void)place;
        std::erase_if(ranges, [&](const auto &range) {
          return range.begin.place == ref->place ||
                 range.end.place == ref->place;
        });
      }

  if (const auto *ret = dyn_cast<ReturnStmt>(&stmt)) {
    (void)ret;
    if (recording())
      checkedOutputs(state);
  }
  const auto *expr = dyn_cast<Expr>(&stmt);
  if (!expr)
    return;
  if (const auto *unary = dyn_cast<UnaryOperator>(expr);
      unary && unary->isIncrementDecrementOp() &&
      expr->getType()->isIntegerType()) {
    const auto old = integerRangeOf(*unary->getSubExpr(), state);
    if (old) {
      const auto one = core::IntegerRange::singleton(
          core::IntegerValue::ofBits(old->values.type, 1));
      const auto next = core::evaluateInteger(
          unary->isIncrementOp() ? core::IntegerOp::Add
                                 : core::IntegerOp::Subtract,
          old->values, one, context.getLangOpts().isSignedOverflowDefined());
      if (next.mayBeInvalid)
        safetyObligation(core::SafetyProperty::Arithmetic,
                         core::SafetyOutcome::Unresolved, stmt, "increment",
                         "integer increment may be invalid");
    }
  }
  if (const auto *compound = dyn_cast<CompoundAssignOperator>(expr);
      compound && compound->getType()->isIntegerType()) {
    const auto lhs = integerRangeOf(*compound->getLHS(), state);
    const auto rhs = integerRangeOf(*compound->getRHS(), state);
    const auto type = integerTypeOf(compound->getComputationLHSType(), context);
    const auto op = integerOpOf(compound->getOpcode());
    if (!lhs || !rhs || !type || !op ||
        core::evaluateInteger(*op, lhs->values.converted(*type), rhs->values,
                              context.getLangOpts().isSignedOverflowDefined())
            .mayBeInvalid)
      safetyObligation(core::SafetyProperty::Arithmetic,
                       core::SafetyOutcome::Unresolved, stmt, "compound",
                       "compound integer operation may be invalid");
  }
  if (const auto *binary = dyn_cast<BinaryOperator>(expr)) {
    if (expr->getType()->isPointerType() &&
        (binary->getOpcode() == BO_Add || binary->getOpcode() == BO_Sub))
      checkedPointerFormation(*expr, *expr, core::Affine::ofConstant(0), state);
    if ((binary->getOpcode() == BO_Sub || binary->isRelationalOp()) &&
        binary->getLHS()->getType()->isPointerType() &&
        binary->getRHS()->getType()->isPointerType())
      safetyObligation(
          core::SafetyProperty::Semantics, core::SafetyOutcome::Unresolved,
          stmt, "pointer difference or ordering",
          "pointer difference or ordering needs shared-object evidence");
  }
  if (const auto *unary = dyn_cast<UnaryOperator>(expr);
      unary && unary->isIncrementDecrementOp() &&
      expr->getType()->isPointerType()) {
    const auto unit = byteSizeOf(expr->getType()->getPointeeType(), context);
    checkedPointerFormation(*expr, *unary->getSubExpr(),
                            unit ? std::optional(core::Affine::ofConstant(
                                       unary->isIncrementOp() ? *unit : -*unit))
                                 : std::nullopt,
                            state);
  }
  if (const auto *compound = dyn_cast<CompoundAssignOperator>(expr);
      compound && expr->getType()->isPointerType()) {
    const auto unit = byteSizeOf(expr->getType()->getPointeeType(), context);
    const auto count = builder.affineOf(*compound->getRHS());
    checkedPointerFormation(
        *expr, *compound->getLHS(),
        unit && count
            ? count->times(compound->getOpcode() == BO_AddAssign ? *unit
                                                                 : -*unit)
            : std::nullopt,
        state);
  }
  if (expr->getType()->isIntegerType() &&
      (isa<BinaryOperator>(expr) ||
       (isa<UnaryOperator>(expr) &&
        !cast<UnaryOperator>(expr)->isIncrementDecrementOp()))) {
    const auto *binary = dyn_cast<BinaryOperator>(expr);
    if (!binary || !binary->isAssignmentOp()) {
      const auto value = integerRangeOf(*expr, state);
      if (value && value->mayBeInvalid)
        safetyObligation(core::SafetyProperty::Arithmetic,
                         core::SafetyOutcome::Unresolved, stmt, "operation",
                         "integer operation may be invalid");
    }
  }
}

void FunctionDataflow::checkedAfter(const Stmt &stmt,
                                    core::AnalysisState &state) {
  if (inUnsafe && checkedUnsupported.contains(&stmt)) {
    state.safety->initialized.clear();
    state.safety->pointers.clear();
    state.safety->memory.clear();
  }
  if (const auto *decl = dyn_cast<DeclStmt>(&stmt)) {
    for (const auto *d : decl->decls()) {
      const auto *var = dyn_cast<VarDecl>(d);
      if (!var)
        continue;
      const auto place = builder.placeForVar(*var);
      if (var->hasInit() || var->hasGlobalStorage()) {
        state.safety->initialized.insert(place);
        if (!var->getType()->isPointerType())
          if (const auto bytes = byteSizeOf(var->getType(), context))
            state.safety->initialize(place,
                                     {.begin = core::Affine::ofConstant(0),
                                      .end = core::Affine::ofConstant(*bytes)});
      }
    }
  }
  const Expr *written = nullptr;
  if (const auto *binary = dyn_cast<BinaryOperator>(&stmt);
      binary && binary->isAssignmentOp())
    written = binary->getLHS();
  if (const auto *unary = dyn_cast<UnaryOperator>(&stmt);
      unary && unary->isIncrementDecrementOp())
    written = unary->getSubExpr();
  if (written) {
    if (const auto ref = builder.resolve(*written))
      if (ref->element.isWhole())
        state.safety->initialized.insert(ref->place);
    if (!written->getType()->isPointerType())
      if (const auto memory = checkedLvalue(*written, state))
        state.safety->initialize(memory->storage,
                                 {.begin = foldAffine(memory->begin, state),
                                  .end = foldAffine(memory->end, state)});
  }
}

FunctionDataflow::CheckedPointer
FunctionDataflow::captureCheckedPointer(const ValueOrigin &given,
                                        core::AnalysisState &state) {
  ValueOrigin origin = given;
  if (!pruneOrigin(origin, state))
    return {};
  CheckedPointer result;
  switch (origin.kind) {
  case ValueOrigin::Kind::Null:
    result.known = true;
    break;
  case ValueOrigin::Kind::Alloc:
    result.known = !origin.family.empty();
    if (origin.call && origin.call->getDirectCallee())
      result.zeroed = origin.call->getDirectCallee()->getName() == "calloc";
    break;
  case ValueOrigin::Kind::Borrow:
    result.known = origin.place.has_value() || origin.literalLength.has_value();
    break;
  case ValueOrigin::Kind::Copy:
    if (origin.place) {
      result.known = state.safety->pointers.contains(origin.place->place);
      if (const auto it = state.safety->memory.find(origin.place->place);
          it != state.safety->memory.end())
        result.initialized = it->second;
    }
    break;
  case ValueOrigin::Kind::Conditional: {
    bool first = true;
    for (const auto &alternative : origin.alternatives) {
      if (alternative.kind == ValueOrigin::Kind::Null)
        continue;
      const auto value = captureCheckedPointer(alternative, state);
      if (first) {
        result = value;
        first = false;
      } else {
        result.known &= value.known;
        result.zeroed &= value.zeroed;
        std::erase_if(result.initialized, [&](const auto &range) {
          return std::ranges::find(value.initialized, range) ==
                 value.initialized.end();
        });
      }
    }
    if (first)
      result.known = true;
    break;
  }
  case ValueOrigin::Kind::Opaque:
  case ValueOrigin::Kind::Raw:
    break;
  }
  return result;
}

void FunctionDataflow::installCheckedPointer(core::PlaceId dest,
                                             const CheckedPointer &value,
                                             core::AnalysisState &state) {
  state.safety->initialized.insert(dest);
  state.safety->pointers.erase(dest);
  state.safety->memory.erase(dest);
  if (value.known)
    state.safety->pointers.insert(dest);
  for (const auto &range : value.initialized)
    state.safety->initialize(dest, range);
  if (value.zeroed)
    if (const auto spatial = state.spatial.recordOf(dest);
        spatial && spatial->extent)
      state.safety->initialize(dest, {.begin = core::Affine::ofConstant(0),
                                      .end = *spatial->extent});
}

void FunctionDataflow::checkedOutputs(const core::AnalysisState &state) {
  core::CheckedContract outputs;
  for (const auto &[place, ranges] : state.safety->memory) {
    const auto path = stableSummaryPathOf(place);
    if (!path || !path->isParam())
      continue;
    for (const auto &range : ranges) {
      const auto first = summaryAffineOf(range.begin);
      const auto last = summaryAffineOf(range.end);
      if (first && last)
        outputs.establish({.kind = core::CheckedRequirementKind::Initialized,
                           .path = *path,
                           .other = {},
                           .begin = *first,
                           .end = *last,
                           .family = {}});
    }
  }
  if (!checkedOutputSeen) {
    inferred.checked.establishes = std::move(outputs.establishes);
    checkedOutputSeen = true;
  } else {
    std::erase_if(inferred.checked.establishes, [&](const auto &requirement) {
      return !outputs.establishes.contains(requirement);
    });
  }
  inferred.checked.limited |= outputs.limited;
}

void FunctionDataflow::checkedFinish(const core::AnalysisState *exitState) {
  if (!checkedOutputSeen && exitState)
    checkedOutputs(*exitState);
  for (const Stmt *stmt : checkedUnsupported) {
    const bool previous = inUnsafe;
    inUnsafe = unsafeBody || unsafeStmts.contains(stmt);
    safetyObligation(core::SafetyProperty::Semantics,
                     core::SafetyOutcome::Unresolved, *stmt, "unsupported",
                     "unsupported checked C construct or storage type");
    inUnsafe = previous;
  }
  if (context.getLangOpts().CPlusPlus)
    safetyObligation(core::SafetyProperty::Semantics,
                     core::SafetyOutcome::Unresolved, *function.getBody(),
                     "language", "checked code requires C semantics");
  // Existing summary-only incompleteness must not disappear. These projection
  // reasons have independent checked obligations; numeric outputs may be
  // unknown without invalidating a pointer operation that never uses them.
  for (const auto &reason : inferred.incomplete) {
    if (reason == "unsupported extent requirement projection" ||
        reason == "unsupported extent requirement condition" ||
        reason == "unsupported extent lower bound projection" ||
        reason == "unsupported numeric output projection")
      continue;
    safetyObligation(core::SafetyProperty::Semantics,
                     core::SafetyOutcome::Unresolved, *function.getBody(),
                     reason, reason);
  }
  const bool interacts =
      std::ranges::any_of(inferred.effects, [](const auto &entry) {
        return entry.second.written || entry.second.consumed() ||
               entry.second.escaped;
      });
  if (interacts && memoryContext.empty()) {
    for (unsigned i = 0; i < function.getNumParams(); ++i)
      for (unsigned j = i + 1; j < function.getNumParams(); ++j)
        if (function.getParamDecl(i)->getType()->isPointerType() &&
            function.getParamDecl(j)->getType()->isPointerType())
          inferred.checked.require(
              {.kind = core::CheckedRequirementKind::Separated,
               .path = core::SummaryPath::param(i),
               .other = core::SummaryPath::param(j),
               .begin = {},
               .end = {},
               .family = {}});
  }
  const auto annotations = getAnnotations(function);
  if (annotations.unsafe)
    safetyObligation(core::SafetyProperty::Semantics,
                     core::SafetyOutcome::Trusted, *function.getBody(),
                     "unsafe-function", "unsafe function contract");
  if (inferred.checked.selected && emitDiagnostics &&
      (!inferred.checked.obligations.complete() || inferred.checked.limited)) {
    for (const auto &[key, obligation] :
         inferred.checked.obligations.entries()) {
      (void)key;
      if (obligation.outcome < core::SafetyOutcome::Unresolved)
        continue;
      core::Diagnostic diagnostic{
          .severity = core::Severity::Error,
          .id = obligation.outcome == core::SafetyOutcome::Violation
                    ? core::diag::CheckingFailed
                    : core::diag::CheckingIncomplete,
          .message = (obligation.outcome == core::SafetyOutcome::Violation
                          ? "checked safety failed: "
                          : "cannot establish checked safety: ") +
                     obligation.reason,
          .location = obligation.location,
          .notes = {},
          .fixits = {}};
      for (const auto &call : obligation.calls)
        diagnostic.addNote("checked obligation originates here", call);
      pending.push_back(std::move(diagnostic));
    }
    if (inferred.checked.limited || inferred.checked.obligations.limited())
      pending.push_back(
          {.severity = core::Severity::Error,
           .id = core::diag::CheckingIncomplete,
           .message =
               "cannot establish checked safety: safety contract limit reached",
           .location = locate(function.getLocation()),
           .notes = {},
           .fixits = {}});
  }
}

} // namespace weavec::analysis
