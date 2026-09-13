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
#include "weavec/Core/ObjectType.h"

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
       .calls = core::SafetyCallPath(std::move(calls))});
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
      obligation.calls.pushBack(note.location);
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
    if (record->isUnion()) {
      if (!record->isCompleteDefinition() ||
          std::cmp_greater(std::ranges::distance(record->fields()),
                           core::MaxUnionMembers))
        return true;
      for (const auto *field : record->fields())
        if (field->getName().empty() || !field->getType()->isScalarType() ||
            field->getType()->isComplexType())
          return true;
    }
    if (record->isCompleteDefinition())
      for (const auto *field : record->fields())
        if (field->isBitField() || field->isAnonymousStructOrUnion() ||
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
  discoverCheckedCases();
  for (const auto *block : *cfg)
    for (const auto &element : *block)
      if (const auto operation = element.getAs<CFGStmt>())
        checkedCFGOperations.insert(operation->getStmt());
  discoverContainers();
  collectCheckedStrings(*function.getBody());
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
    if (isa<AsmStmt, IndirectGotoStmt, AddrLabelExpr, VAArgExpr, AtomicExpr>(
            stmt))
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
            !from->isVoidType() && !from->isCharType() &&
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
  if (checkedUnsupported.contains(&stmt))
    safetyObligation(core::SafetyProperty::Semantics,
                     core::SafetyOutcome::Unresolved, stmt, "unsupported",
                     "unsupported checked C construct or storage type");
  if (const auto *cast = dyn_cast<CastExpr>(&stmt))
    checkedObjectCast(*cast, state);
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
  if (written && !written->getType()->isIntegerType())
    if (const auto ref = builder.resolve(*written)) {
      snapshotScalar(ref->place, dyn_cast<Expr>(&stmt), state);
      state.safety->forgetDependency(ref->place);
      if (auto list = state.safety->argumentLists.find(ref->place);
          list != state.safety->argumentLists.end())
        list->second.phase = core::ArgumentListPhase::Unknown;
    }

  if (const auto *ret = dyn_cast<ReturnStmt>(&stmt); ret && recording()) {
    runtimeListReturns(stmt, state);
    checkedOutputs(state, ret->getRetValue());
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
        binary->getRHS()->getType()->isPointerType()) {
      const bool related = checkedPointerComparable(*binary, state);
      const bool proved = related && (binary->isRelationalOp() ||
                                      checkedPointerRange(*binary, state));
      safetyObligation(
          core::SafetyProperty::Semantics,
          proved ? core::SafetyOutcome::Proven
                 : core::SafetyOutcome::Unresolved,
          stmt, "pointer difference or ordering",
          related && binary->getOpcode() == BO_Sub
              ? "pointer difference must fit target ptrdiff_t and element size"
              : "pointer difference or ordering needs shared-object evidence");
    }
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
  if (const auto *expr = dyn_cast<Expr>(&stmt))
    checkedAdvancePointer(*expr, state);
  if (inUnsafe && checkedUnsupported.contains(&stmt)) {
    for (auto &[storage, type] : state.safety->objectTypes) {
      (void)storage;
      type = "?";
    }
    state.safety->unions.invalidateAll();
    state.safety->containers.clear();
    state.safety->initialized.clear();
    state.safety->pointers.clear();
    state.safety->memory.clear();
    state.safety->positions.clear();
    state.safety->accessible.clear();
    state.safety->termination.clear();
    state.safety->boundedTermination.clear();
    for (auto &[place, list] : state.safety->argumentLists) {
      (void)place;
      list.phase = core::ArgumentListPhase::Unknown;
    }
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
    const auto writtenMemory = checkedLvalue(*written, state);
    const auto *assignment = dyn_cast<BinaryOperator>(&stmt);
    const bool zeroed = assignment != nullptr &&
                        assignment->getOpcode() == BO_Assign &&
                        byteSizeOf(written->getType(), context) == 1 &&
                        integerConstant(*assignment->getRHS(), context) == 0;
    checkedStringWrite(writtenMemory, zeroed, state);
    checkedUnionWrite(*written, writtenMemory, state);
    if (const auto ref = builder.resolve(*written))
      if (ref->element.isWhole())
        state.safety->initialized.insert(ref->place);
    if (const auto &memory = writtenMemory) {
      state.safety->initialize(memory->storage,
                               {.begin = foldAffine(memory->begin, state),
                                .end = foldAffine(memory->end, state),
                                .zeroed = zeroed});
      // Keep a symbolic right endpoint as well as the folded interval. A
      // subsequent value-preserving advance can carry this must-write fact
      // around a CFG back edge without pretending that a visit was a store.
      state.safety->initialize(memory->storage,
                               {.begin = checkedStableAffine(
                                    foldAffine(memory->begin, state), state),
                                .end = checkedStableAffine(memory->end, state),
                                .zeroed = zeroed});
      if (const auto *subscript =
              dyn_cast<ArraySubscriptExpr>(written->IgnoreParenImpCasts())) {
        const Expr *indexExpr = subscript->getIdx()->IgnoreParenImpCasts();
        int displacement = 0;
        if (const auto *adjust = dyn_cast<UnaryOperator>(indexExpr);
            adjust && adjust->isIncrementDecrementOp()) {
          if (adjust->isPostfix())
            displacement = adjust->isIncrementOp() ? -1 : 1;
          indexExpr = adjust->getSubExpr()->IgnoreParenImpCasts();
        }
        if (const auto *index = dyn_cast<DeclRefExpr>(indexExpr))
          if (const auto *var = dyn_cast<VarDecl>(index->getDecl())) {
            const auto unit = byteSizeOf(written->getType(), context);
            const auto first =
                unit ? core::Affine::ofPlace(builder.placeForVar(*var), 1,
                                             displacement)
                           .times(*unit)
                     : std::nullopt;
            const auto end =
                first && unit ? first->shifted(*unit) : std::nullopt;
            if (first && end)
              if (const auto symbolic = checkedMemory(*subscript->getBase(),
                                                      *first, *end, state)) {
                state.safety->initialize(
                    symbolic->storage,
                    {.begin = foldAffine(symbolic->begin, state),
                     .end = checkedStableAffine(symbolic->end, state),
                     .zeroed = zeroed});
                state.safety->initialize(
                    symbolic->storage,
                    {.begin = symbolic->begin,
                     .end = checkedStableAffine(symbolic->end, state),
                     .zeroed = zeroed});
              }
          }
      }
    }
  }
  checkedContainerStore(stmt, state);
}

FunctionDataflow::CheckedPointer FunctionDataflow::captureCheckedPointer(
    core::PlaceId dest, const ValueOrigin &given, core::AnalysisState &state) {
  ValueOrigin origin = given;
  if (!pruneOrigin(origin, state))
    return {};
  CheckedPointer result;
  result.container = captureContainer(dest, origin, state);
  if (origin.place) {
    auto source = origin.place->place;
    result.invalidated = state.safety->invalidatedPointers.contains(source);
    if (result.container && result.container->tailOf &&
        !state.safety->containers.find(source))
      source = *result.container->tailOf;
    result.containerSeparated = state.safety->containers.separatedFrom(source);
    if (result.container)
      result.container->ancestors.insert(source);
  }
  switch (origin.kind) {
  case ValueOrigin::Kind::Null:
    result.known = true;
    break;
  case ValueOrigin::Kind::Alloc:
    result.known = !origin.family.empty();
    result.fresh = true;
    if (origin.call) {
      const auto [object, inserted] =
          checkedObjects.try_emplace(std::pair{origin.call, dest});
      if (inserted)
        object->second = places.create("checked allocation");
      result.storage = object->second;
    }
    if (origin.call)
      result.zeroed = resolvedLibraryName(*origin.call) == "calloc";
    break;
  case ValueOrigin::Kind::Borrow:
    result.known = origin.place.has_value() || origin.literalLength.has_value();
    if (origin.place) {
      auto storage = origin.place->place;
      if (places.step(storage) == core::PathStep::Index)
        if (const auto parent = places.parent(storage))
          storage = *parent;
      result.storage = storage;
    }
    break;
  case ValueOrigin::Kind::Copy:
    if (origin.place) {
      result.known = state.safety->pointers.contains(origin.place->place);
      const auto memory = checkedMemoryAt(origin.place->place, {}, {}, state);
      const auto storage =
          memory ? memory->storage : places.deref(origin.place->place);
      result.storage = storage;
      result.deferred = state.safety->deferred.contains(origin.place->place) ||
                        state.safety->deferred.contains(storage);
      if (memory && origin.offset.isZero())
        result.position = core::PointerPosition{.storage = storage,
                                                .offset = memory->begin,
                                                .extent = memory->extent,
                                                .input = memory->inputPlace};
      if (const auto it = state.safety->memory.find(storage);
          it != state.safety->memory.end())
        result.initialized = it->second;
    }
    break;
  case ValueOrigin::Kind::Conditional: {
    bool first = true;
    for (const auto &alternative : origin.alternatives) {
      if (alternative.kind == ValueOrigin::Kind::Null)
        continue;
      const auto value = captureCheckedPointer(dest, alternative, state);
      if (first) {
        result = value;
        first = false;
      } else {
        if (result.container != value.container)
          result.container.reset();
        std::erase_if(result.containerSeparated, [&](core::PlaceId place) {
          return !value.containerSeparated.contains(place);
        });
        if (result.storage != value.storage)
          result.storage.reset();
        if (result.position != value.position)
          result.position.reset();
        result.known &= value.known;
        result.nonNull &= value.nonNull;
        result.fresh &= value.fresh;
        result.invalidated |= value.invalidated;
        result.zeroed &= value.zeroed;
        result.deferred |= value.deferred;
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
  result.deferred |=
      (origin.call != nullptr) && checkedDeferredCalls.contains(origin.call);
  result.known |= result.container.has_value();
  return result;
}

void FunctionDataflow::installCheckedPointer(core::PlaceId dest,
                                             const CheckedPointer &value,
                                             core::AnalysisState &state) {
  // Capture precedes the ordinary assignment. Its resulting nullness remains
  // authoritative, including chained assignments and related input contexts.
  const bool consistentContainer =
      !value.container || !value.container->empty ||
      state.nulls.stateOf(dest) != core::Nullness::NonNull;
  state.safety->containers.replace(dest);
  state.safety->invalidatedPointers.erase(dest);
  if (value.invalidated)
    state.safety->invalidatedPointers.insert(dest);
  if (value.container && consistentContainer) {
    auto fact = *value.container;
    fact.ancestors.erase(dest);
    if (fact.tailOf == dest)
      fact.tailOf.reset();
    state.safety->containers.set(dest, std::move(fact));
  }
  if (consistentContainer)
    for (const auto other : value.containerSeparated)
      state.safety->containers.separate(dest, other);
  if (value.fresh)
    state.safety->containers.markFresh(dest);
  snapshotContainerOutput(dest, state);
  checkedCallAssignedPointers.insert(dest);
  state.safety->replacedPointers.insert(dest);
  state.safety->initialized.insert(dest);
  state.safety->pointers.erase(dest);
  state.safety->objects.erase(dest);
  state.safety->positions.erase(dest);
  state.safety->deferred.erase(dest);
  if (value.deferred)
    state.safety->deferred.insert(dest);
  const auto storage = value.storage.value_or(places.deref(dest));
  if (value.fresh) {
    if (state.safety->objectTypes.size() < core::MaxSafetyRequirements ||
        state.safety->objectTypes.contains(storage)) {
      std::string type;
      if (const auto *decl = dyn_cast_or_null<ValueDecl>(builder.declFor(dest));
          decl && decl->getType()->isPointerType()) {
        const auto pointee = decl->getType()->getPointeeType();
        if (!pointee->isVoidType() && !pointee->isCharType())
          type = checkedObjectType(pointee);
      }
      state.safety->objectTypes[storage] = std::move(type);
    } else {
      if (recording())
        inferred.checked.limited = true;
    }
    state.safety->writtenStorage.erase(storage);
    state.safety->memory.erase(storage);
    state.safety->accessible.erase(storage);
    std::erase_if(state.safety->positions, [&](const auto &entry) {
      return entry.second.storage == storage;
    });
    std::erase_if(state.safety->objects,
                  [&](const auto &entry) { return entry.second == storage; });
  }
  if (value.storage)
    state.safety->objects[dest] = storage;
  if (value.position)
    installCheckedPosition(dest, *value.position, state);
  if (!value.storage)
    state.safety->memory.erase(storage);
  if ((value.known && consistentContainer) || value.nonNull)
    state.safety->pointers.insert(dest);
  if (value.nonNull)
    state.nulls.set(dest, {.state = core::Nullness::NonNull,
                           .location = {},
                           .reason = core::NullReason::Declared});
  for (const auto &range : value.initialized)
    state.safety->initialize(storage, range);
  if (value.zeroed)
    if (const auto spatial = state.spatial.recordOf(dest);
        spatial && spatial->extent)
      state.safety->initialize(storage, {.begin = core::Affine::ofConstant(0),
                                         .end = *spatial->extent,
                                         .zeroed = true});
}

void FunctionDataflow::checkedOutputs(const core::AnalysisState &incoming,
                                      const Expr *value) {
  std::optional<core::AnalysisState> materialized;
  std::optional<core::PlaceId> returned;
  if (value && value->getType()->isPointerType()) {
    const auto *expression = value->IgnoreParenImpCasts();
    const auto direct = PlaceBuilder::isPlaceExpr(*expression)
                            ? builder.resolvePointerValue(*expression)
                            : std::nullopt;
    if (direct) {
      // The original holder ties return-outcome guards and nested objects
      // to the result. Only derived expressions need a synthetic position.
      returned = direct->place;
      if (!incoming.safety->containers.find(*returned)) {
        materialized = incoming;
        if (const auto fact = captureContainer(
                *returned, builder.classifyValue(*value), *materialized))
          materialized->safety->containers.set(*returned, *fact);
      }
    } else if (const auto *call =
                   dyn_cast<CallExpr>(value->IgnoreParenCasts())) {
      materialized = incoming;
      const auto [slot, inserted] = checkedReturnPlaces.try_emplace(call);
      if (inserted)
        slot->second = places.create("checked returned pointer");
      returned = slot->second;
      const auto pointer = captureCheckedPointer(
          *returned, builder.classifyValue(*value), *materialized);
      installCheckedPointer(*returned, pointer, *materialized);
      applyCheckedResult(*returned, *call, *materialized);
    } else if (const auto memory = checkedMemory(*value, {}, {}, incoming)) {
      materialized = incoming;
      const auto [slot, inserted] = checkedReturnPlaces.try_emplace(value);
      if (inserted)
        slot->second = places.create("checked returned pointer");
      returned = slot->second;
      auto pointer = captureCheckedPointer(
          *returned, builder.classifyValue(*value), *materialized);
      pointer.storage = memory->storage;
      pointer.position = core::PointerPosition{.storage = memory->storage,
                                               .offset = memory->begin,
                                               .extent = memory->extent,
                                               .input = memory->inputPlace};
      pointer.nonNull = checkedValid(*memory, incoming);
      installCheckedPointer(*returned, pointer, *materialized);
    }
  }
  const auto &state = materialized ? *materialized : incoming;
  const auto returnedNumber = value && value->getType()->isIntegerType()
                                  ? integerExpressionOf(*value, state)
                                  : std::nullopt;
  const auto returnedIdentity =
      returnedNumber ? returnedNumber->inputKey() : std::nullopt;
  const auto endpoint =
      [&](const core::Affine &point) -> std::optional<core::PathAffine> {
    if (point.place && point.place == returnedIdentity)
      return core::PathAffine::ofPath(core::SummaryPath::result(), point.scale,
                                      point.constant);
    if (value && value->getType()->isIntegerType()) {
      const auto returnedValue = builder.affineOf(*value);
      if (returnedValue && returnedValue->place &&
          returnedValue->place == point.place && returnedValue->scale == 1) {
        std::int64_t offset = 0;
        if (!__builtin_mul_overflow(returnedValue->constant, point.scale,
                                    &offset) &&
            !__builtin_sub_overflow(point.constant, offset, &offset))
          return core::PathAffine::ofPath(core::SummaryPath::result(),
                                          point.scale, offset);
      }
    }
    return summaryAffineOf(point);
  };
  std::set<std::optional<core::Outcome>> classes;
  if (value && value->getType()->isPointerType()) {
    const auto origin = builder.classifyValue(*value);
    if (origin.kind == ValueOrigin::Kind::Null) {
      classes.insert(core::Outcome::Null);
    } else if (origin.kind == ValueOrigin::Kind::Borrow) {
      classes.insert(core::Outcome::NonNull);
    } else {
      classes = {core::Outcome::Null, core::Outcome::NonNull};
      if (origin.place && origin.offset.isZero()) {
        if (!returned)
          returned = origin.place->place;
        if (const auto nullness = nullnessAt(*returned, state)) {
          if (nullness->state == core::Nullness::Null)
            classes.erase(core::Outcome::NonNull);
          if (nullness->state == core::Nullness::NonNull)
            classes.erase(core::Outcome::Null);
        }
      }
      if (returned && state.nulls.stateOf(*returned) == core::Nullness::NonNull)
        classes.erase(core::Outcome::Null);
    }
  } else if (value && value->getType()->isIntegerType()) {
    const auto fact = scalarFactOf(*value, state);
    if (fact) {
      for (const auto outcome : fact->classes)
        classes.insert(outcome);
    } else {
      classes = {core::Outcome::Zero, core::Outcome::Positive};
      if (value->getType()->isSignedIntegerType())
        classes.insert(core::Outcome::Negative);
    }
  } else {
    classes.insert(std::nullopt);
  }
  std::map<core::PlaceId, std::set<core::SummaryPath>> paths;
  for (const auto &[storage, ranges] : state.safety->memory) {
    (void)ranges;
    const bool indirect =
        std::ranges::any_of(checkedInputObjects, [&](const auto &entry) {
          return entry.second == storage;
        });
    if (!indirect)
      if (const auto path = builder.summaryPathOf(storage);
          path && (path->isParam() || path->isGlobal()))
        paths[storage].insert(*path);
  }
  for (const auto &[holder, storage] : checkedInputObjects)
    if (const auto path = builder.summaryPathOf(holder);
        path && (path->isParam() || path->isGlobal()))
      paths[storage].insert(*path);
  for (const auto &[holder, storage] : state.safety->objects)
    if (const auto path = builder.summaryPathOf(holder);
        path && (path->isParam() || path->isGlobal()))
      paths[storage].insert(*path);
  std::optional<core::PlaceId> returnedStorage;
  if (returned) {
    const auto object = state.safety->objects.find(*returned);
    returnedStorage =
        object == state.safety->objects.end() ? *returned : object->second;
  }
  std::set<core::SummaryPath> nullOutputs;
  for (const auto &[holder, nullness] : state.nulls.all()) {
    const auto path = builder.summaryPathOf(holder);
    if (path && !(path->isParam() && path->isRoot()) &&
        nullness.state == core::Nullness::Null &&
        nullOutputs.size() < core::MaxSafetyRequirements)
      nullOutputs.insert(*path);
  }
  for (const auto outcome : classes) {
    core::CheckedContract outputs;
    checkedUnionOutputs(outputs, value, outcome, state);
    containerOutputs(outputs, state, returned, outcome);
    for (const auto &[holder, position] : state.safety->positions) {
      if (!position.input)
        continue;
      const auto origin = builder.summaryPathOf(*position.input);
      if (!origin)
        continue;
      auto destination = builder.summaryPathOf(holder);
      if (returned == holder && outcome == core::Outcome::NonNull)
        destination = core::SummaryPath::result();
      if (!destination || (destination->isParam() && destination->isRoot()))
        continue;
      const auto establish = [&](const core::PathAffine &first,
                                 const core::PathAffine &last) {
        outputs.establish({.kind = core::CheckedRequirementKind::Position,
                           .path = *destination,
                           .other = *origin,
                           .begin = first,
                           .end = last,
                           .family = {},
                           .on = outcome});
      };
      if (const auto offset = summaryAffineOf(position.offset))
        establish(*offset, *offset);
      if (checkedAtMost({}, position.offset, state))
        if (const auto bound =
                checkedRequirementEnvelope(position.offset, state))
          establish(core::PathAffine::ofConstant(0), *bound);
      // Common entry bounds survive alternative return sites, including a
      // zero-length early return. Each bound is proved on this return edge.
      if (checkedAtMost({}, position.offset, state))
        for (const auto &[zero, input] : checkedTerminatorInputs) {
          (void)input;
          const auto bound = core::Affine::ofPlace(zero);
          const auto exported = summaryAffineOf(bound);
          if (exported && checkedAtMost(position.offset, bound, state))
            establish(core::PathAffine::ofConstant(0), *exported);
        }
      if (checkedAtMost({}, position.offset, state))
        for (const auto *parameter : function.parameters()) {
          if (!parameter->getType()->isIntegerType())
            continue;
          const auto input = builder.placeForVar(*parameter);
          const auto saved = numericEntryValues.find(input);
          const auto entry =
              saved == numericEntryValues.end() ? input : saved->second;
          const auto bound = core::Affine::ofPlace(entry);
          const auto exported = summaryAffineOf(bound);
          if (exported && checkedAtMost(position.offset, bound, state))
            establish(core::PathAffine::ofConstant(0), *exported);
        }
    }
    auto memory = *state.safety;
    for (const auto &[holder, position] : state.safety->positions) {
      const auto path = builder.summaryPathOf(holder);
      if (!path || (path->isParam() && path->isRoot()) ||
          position.input != holder)
        continue;
      for (const auto &[otherHolder, otherPosition] : state.safety->positions) {
        const auto other = builder.summaryPathOf(otherHolder);
        if (otherHolder == holder || !other ||
            (other->isParam() && other->isRoot()) ||
            otherPosition.input != otherHolder ||
            !checkedAtMost(position.offset, otherPosition.offset, state))
          continue;
        outputs.establish({.kind = core::CheckedRequirementKind::Progress,
                           .path = *path,
                           .other = *other,
                           .begin = {},
                           .end = {},
                           .family = {},
                           .on = outcome});
      }
    }
    const auto *call =
        value ? dyn_cast<CallExpr>(value->IgnoreParenImpCasts()) : nullptr;
    if (outcome && call &&
        ASTContext::hasSameType(value->getType(), call->getType()) &&
        lastCall && lastCall->call == call) {
      auto narrowed = lastCall->pending;
      narrowed.select({*outcome});
      for (const auto &[storage, range] : narrowed.initializedInAll())
        memory.initialize(storage, range);
    }
    auto destinations = paths;
    const auto returnedPosition = returned
                                      ? state.safety->positions.find(*returned)
                                      : state.safety->positions.end();
    const bool returnedAtStart =
        returnedPosition == state.safety->positions.end() ||
        (foldAffine(returnedPosition->second.offset, state).isConstant() &&
         foldAffine(returnedPosition->second.offset, state).constant == 0);
    if (outcome == core::Outcome::NonNull && returnedStorage &&
        returnedAtStart) {
      destinations[*returnedStorage].insert(core::SummaryPath::result());
      for (const auto &[holder, storage] : state.safety->objects) {
        if (!returned || !places.isDescendantOf(holder, *returned))
          continue;
        auto path = core::SummaryPath::result();
        auto child = holder;
        while (child != *returned &&
               path.steps.size() <= core::MaxHeapPathDepth) {
          path.steps.insert(path.steps.begin(),
                            {.step = places.step(child),
                             .field = std::string(places.fieldName(child))});
          child = *places.parent(child);
        }
        if (child == *returned && path.steps.size() <= core::MaxHeapPathDepth)
          destinations[storage].insert(std::move(path));
      }
    }
    for (const auto &[storage, exported] : destinations) {
      if (const auto type = memory.objectTypes.find(storage);
          type != memory.objectTypes.end() &&
          core::ObjectType::parse(type->second))
        for (const auto &path : exported) {
          if (path.isParam() && path.isRoot() && state.isOverwritten(path))
            continue;
          outputs.establish({.kind = core::CheckedRequirementKind::ObjectType,
                             .path = path,
                             .other = {},
                             .family = type->second,
                             .on = outcome});
        }
      if (const auto witnesses = memory.termination.find(storage);
          witnesses != memory.termination.end())
        for (const auto &witness : witnesses->second) {
          auto condition = witness.when;
          if (!pruneGuard(condition, state))
            continue;
          const auto guard = summaryGuardOf(condition);
          const auto first = endpoint(witness.begin);
          const auto zero = endpoint(witness.zero);
          if (!first || !zero || !summaryGuardComplete(condition, guard))
            continue;
          for (const auto &path : exported)
            outputs.establish({.kind = core::CheckedRequirementKind::Terminated,
                               .path = path,
                               .other = {},
                               .begin = *first,
                               .end = *zero,
                               .family = {},
                               .when = guard,
                               .on = outcome});
        }
      auto ranges = memory.memory[storage];
      if (const auto bounded = memory.boundedTermination.find(storage);
          bounded != memory.boundedTermination.end())
        ranges.insert(ranges.end(), bounded->second.begin(),
                      bounded->second.end());
      for (const auto &range : ranges) {
        auto condition = range.when;
        if (!pruneGuard(condition, state))
          continue;
        // A result class supplies precisely this premise, never an unrelated
        // local condition. Any remaining unexportable premise loses the fact.
        if (returned && outcome)
          if (const auto entry = condition.conditions.find(*returned);
              entry != condition.conditions.end() &&
              core::ValueFact::of(*outcome).implies(entry->second))
            condition.conditions.erase(entry);
        if (returnedIdentity && outcome)
          if (const auto entry = condition.conditions.find(*returnedIdentity);
              entry != condition.conditions.end() &&
              core::ValueFact::of(*outcome).implies(entry->second))
            condition.conditions.erase(entry);
        const auto first = endpoint(range.begin);
        const auto last = endpoint(range.end);
        if (!first || !last)
          continue;
        std::optional<core::SummaryPath> source;
        if (range.source) {
          for (const auto &[holder, object] : checkedInputObjects)
            if (object == *range.source)
              source = builder.summaryPathOf(holder);
          if (!source)
            continue;
        }
        auto kind = core::CheckedRequirementKind::Initialized;
        if (source)
          kind = core::CheckedRequirementKind::Copied;
        else if (range.zeroed)
          kind = core::CheckedRequirementKind::Zeroed;
        else if (range.terminatedWithin)
          kind = core::CheckedRequirementKind::TerminatedWithin;
        for (const auto &path : exported) {
          auto inputCondition = condition;
          bool ifNonNull =
              !(path.isParam() && path.isRoot()) &&
              std::ranges::any_of(memory.objects, [&](const auto &entry) {
                return entry.second == storage &&
                       builder.summaryPathOf(entry.first) == path;
              });
          for (auto it = inputCondition.conditions.begin();
               it != inputCondition.conditions.end();) {
            const auto object = memory.objects.find(it->first);
            if (object != memory.objects.end() && object->second == storage &&
                builder.summaryPathOf(it->first) == path &&
                core::ValueFact::of(core::Outcome::NonNull)
                    .implies(it->second)) {
              ifNonNull = true;
              it = inputCondition.conditions.erase(it);
            } else {
              ++it;
            }
          }
          const auto guard = summaryGuardOf(inputCondition);
          if (!summaryGuardComplete(inputCondition, guard))
            continue;
          outputs.establish({.kind = kind,
                             .path = path,
                             .other = source.value_or(core::SummaryPath{}),
                             .begin = *first,
                             .end = *last,
                             .family = {},
                             .when = guard,
                             .on = outcome,
                             .ifNonNull = ifNonNull});
        }
      }
    }
    const auto [existing, inserted] =
        checkedOutputClasses.try_emplace(outcome, outputs.establishes);
    if (inserted) {
      checkedNullOutputClasses[outcome] = nullOutputs;
    } else {
      core::CheckedRequirements::Set joined;
      for (const auto &post : existing->second)
        if (outputs.establishes.contains(post) ||
            (post.ifNonNull && nullOutputs.contains(post.path)))
          joined.insert(post);
      for (const auto &first : existing->second)
        if (first.kind == core::CheckedRequirementKind::ContainerDerived)
          for (const auto &second : outputs.establishes)
            if (const auto generalized =
                    core::joinContainerOutput(first, second))
              joined.insert(*generalized);
      auto &previousNull = checkedNullOutputClasses[outcome];
      for (const auto &post : outputs.establishes)
        if (post.ifNonNull && previousNull.contains(post.path))
          joined.insert(post);
      if (joined.size() > core::MaxSafetyRequirements) {
        joined.erase(std::next(joined.begin(), core::MaxSafetyRequirements),
                     joined.end());
        outputs.limited = true;
      }
      existing->second.assign(std::move(joined));
      std::erase_if(previousNull, [&](const auto &path) {
        return !nullOutputs.contains(path);
      });
    }
    inferred.checked.limited |= outputs.limited;
  }
  checkedOutputSeen = true;
  inferred.checked.establishes.clear();
  for (const auto &path : runtimeConsumedLists)
    inferred.checked.establish(
        {.kind = core::CheckedRequirementKind::ArgumentListConsumed,
         .path = path,
         .other = {},
         .family = {}});
  for (const auto &[outcome, outputs] : checkedOutputClasses) {
    (void)outcome;
    for (auto post : outputs) {
      const bool everyOutcome =
          std::ranges::all_of(checkedOutputClasses, [&](const auto &entry) {
            auto selected = post;
            selected.on = entry.first;
            return entry.second.contains(selected);
          });
      if (everyOutcome)
        post.on.reset();
      inferred.checked.establish(std::move(post));
    }
  }
}

void FunctionDataflow::checkedFinish(const core::AnalysisState *exitState) {
  if (!checkedOutputSeen && exitState) {
    runtimeListReturns(*function.getBody(), *exitState);
    checkedOutputs(*exitState);
  }
  for (const Stmt *stmt : checkedUnsupported) {
    // RFC 0025: evaluated exclusions belong to their reachable proof case.
    // Unmapped exclusions remain unconditional; absence is not reachability.
    if (checkedCFGOperations.contains(stmt))
      continue;
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
            !function.getParamDecl(i)->getType()->isFunctionPointerType() &&
            function.getParamDecl(j)->getType()->isPointerType() &&
            !function.getParamDecl(j)->getType()->isFunctionPointerType())
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
    // RFC 0020: flushDiagnostics keeps the first equal rendered diagnostic.
    // Borrow immutable ledger strings to reject duplicates before allocating
    // their messages and call notes; report entries remain independent.
    using DiagnosticKey =
        std::tuple<std::string_view, std::uint32_t, std::uint32_t,
                   core::SafetyOutcome, std::string_view>;
    std::set<DiagnosticKey> seen;
    for (const auto &[key, obligation] :
         inferred.checked.obligations.entries()) {
      (void)key;
      if (obligation.outcome < core::SafetyOutcome::Unresolved)
        continue;
      if (!seen.emplace(obligation.location.file, obligation.location.line,
                        obligation.location.column, obligation.outcome,
                        obligation.reason)
               .second)
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
