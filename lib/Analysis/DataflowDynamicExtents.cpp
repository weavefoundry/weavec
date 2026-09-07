//===- DataflowDynamicExtents.cpp - Runtime subobject bounds (RFC 0017) ---===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"
#include "IntegerSupport.h"

#include "clang/AST/TypeLoc.h"

using namespace clang;

namespace weavec::analysis {

std::optional<FunctionDataflow::NumericExpression>
FunctionDataflow::variableArraySize(QualType type,
                                    const core::AnalysisState &state) {
  const auto sizeType = integerTypeOf(context.getSizeType(), context);
  if (!sizeType)
    return std::nullopt;
  auto product =
      NumericExpression::constant(core::IntegerValue::ofBits(*sizeType, 1));
  while (const auto *array = context.getAsArrayType(type)) {
    std::optional<NumericExpression> count;
    if (const auto *variable = dyn_cast<VariableArrayType>(array)) {
      const auto found = variableArrayCounts.find(variable);
      if (found == variableArrayCounts.end() ||
          !state.numericValues.contains(found->second))
        return std::nullopt;
      count = NumericExpression::input(found->second, *sizeType);
    } else if (const auto *fixed = dyn_cast<ConstantArrayType>(array)) {
      if (fixed->getSize().getActiveBits() > sizeType->width)
        return std::nullopt;
      count = NumericExpression::constant(core::IntegerValue::ofBits(
          *sizeType, fixed->getSize().getZExtValue()));
    }
    if (!count)
      return std::nullopt;
    if (!operationDoesNotOverflow(core::IntegerOp::Multiply, product, *count,
                                  *sizeType, state))
      return std::nullopt;
    const auto next = NumericExpression::operation(core::IntegerOp::Multiply,
                                                   product, *count);
    if (!next)
      return std::nullopt;
    product = *next;
    type = array->getElementType();
  }
  const auto bytes = byteSizeOf(type, context);
  if (!bytes)
    return std::nullopt;
  const auto unit = NumericExpression::constant(core::IntegerValue::ofBits(
      *sizeType, static_cast<std::uint64_t>(*bytes)));
  if (!operationDoesNotOverflow(core::IntegerOp::Multiply, product, unit,
                                *sizeType, state))
    return std::nullopt;
  return NumericExpression::operation(core::IntegerOp::Multiply, product, unit);
}

void FunctionDataflow::captureVariableArrayType(TypeSourceInfo *info,
                                                core::AnalysisState &state,
                                                const Expr *initializer) {
  if (!info || !info->getType()->isVariablyModifiedType())
    return;
  const auto sizeType = integerTypeOf(context.getSizeType(), context);
  if (!sizeType)
    return;

  // TypeLoc visits only the dimensions evaluated by this declaration. In
  // particular, a typedef use ends the walk: its dimensions were evaluated
  // by the typedef declaration, and must not be recaptured from today's n.
  std::vector<const VariableArrayType *> dimensions;
  bool sideEffects = false;
  std::set<const VarDecl *> initializerReferences;
  const auto collectReferences = [&](const auto &self,
                                     const Stmt *stmt) -> void {
    if (!stmt)
      return;
    if (const auto *ref = dyn_cast<DeclRefExpr>(stmt))
      if (const auto *var = dyn_cast<VarDecl>(ref->getDecl()))
        initializerReferences.insert(var->getCanonicalDecl());
    for (const auto *child : stmt->children())
      self(self, child);
  };
  const bool initializerEffects =
      initializer != nullptr && initializer->HasSideEffects(context);
  if (initializerEffects)
    collectReferences(collectReferences, initializer);
  const auto affectedByInitializer = [&](const auto &self,
                                         const Stmt *stmt) -> bool {
    if (!stmt || !initializerEffects)
      return false;
    if (const auto *ref = dyn_cast<DeclRefExpr>(stmt))
      if (const auto *var = dyn_cast<VarDecl>(ref->getDecl());
          var && (var->hasGlobalStorage() ||
                  addressTaken.contains(var->getCanonicalDecl()) ||
                  initializerReferences.contains(var->getCanonicalDecl())))
        return true;
    return std::ranges::any_of(
        stmt->children(), [&](const Stmt *child) { return self(self, child); });
  };
  for (TypeLoc loc = info->getTypeLoc(); !loc.isNull();
       loc = loc.getNextTypeLoc()) {
    if (const auto array = loc.getAs<VariableArrayTypeLoc>()) {
      dimensions.push_back(array.getTypePtr());
      if (const auto *bound = array.getSizeExpr())
        sideEffects |= bound->HasSideEffects(context) ||
                       affectedByInitializer(affectedByInitializer, bound);
    }
  }

  for (const auto *variable : dimensions) {
    const auto *bound = variable->getSizeExpr();
    auto count = variableArrayCounts.find(variable);
    if (count == variableArrayCounts.end()) {
      const auto id = places.create(
          "vla-count(" + std::to_string(variableArrayCounts.size()) + ")");
      count = variableArrayCounts.emplace(variable, id).first;
      snapshotPlaces.insert(id);
    }
    // Even a failed evaluation starts a new generation. Do not leave an old
    // positive count or byte extent behind when a loop revisits this site.
    snapshotIntegerDependencies(count->second, bound, state);
    snapshotScalar(count->second, bound, state);
    state.dropGuardsOn(count->second);
    state.relations.forget(count->second);
    state.scalars.forget(count->second);
    state.numericValues.erase(count->second);
    if (!bound)
      continue;
    // The CFG has already executed the dimension expressions. Re-reading
    // even an ordinary bound can be wrong if another dimension changed it.
    // Until individual evaluated results are available, forget the entire
    // declaration's dimensions rather than re-evaluating their side effects.
    if (sideEffects) {
      reportIncomplete("side-effecting variable array dimensions", *bound);
      continue;
    }
    const auto actual = integerRangeOf(*bound, state);
    const auto expression = integerExpressionOf(*bound, state);
    if (!actual || !expression || actual->mayBeInvalid ||
        actual->values.empty()) {
      reportIncomplete("unsupported variable array dimension", *bound);
      continue;
    }
    if (const auto maximum = actual->values.maximum();
        maximum && (maximum->negative() || maximum->bits == 0)) {
      report(makeError(
          core::diag::InvalidIntegerOperation,
          "invalid integer operation: nonpositive variable array dimension",
          *bound));
      continue;
    }
    const auto minimum = actual->values.minimum();
    if (!minimum || minimum->negative() || minimum->bits == 0 ||
        !conversionPreserves(actual->values, *sizeType)) {
      reportIncomplete("unrepresentable variable array dimension", *bound);
      continue;
    }
    const auto converted = expression->converted(*sizeType);
    if (!converted) {
      reportIncomplete("unsupported variable array dimension", *bound);
      continue;
    }
    state.scalars.set(count->second, core::ValueFact::ofInteger(
                                         actual->values.converted(*sizeType)));
    state.numericValues.insert_or_assign(count->second, *converted);
    if (const auto affine = integerAffineOf(*bound, state);
        affine && affine->place && affine->scale == 1)
      state.relations.learn(count->second, core::Relation::Equal,
                            *affine->place, affine->constant);
  }
}

void FunctionDataflow::captureVariableArray(core::PlaceId place,
                                            const VarDecl &var,
                                            core::AnalysisState &state) {
  if (!var.getType()->isVariablyModifiedType())
    return;
  captureVariableArrayType(var.getTypeSourceInfo(), state, var.getInit());
  if (!var.getType()->isArrayType())
    return;
  const auto expression = variableArraySize(var.getType(), state);
  if (!expression) {
    state.spatial.forget(place);
    for (QualType type = var.getType();
         const auto *array = context.getAsArrayType(type);
         type = array->getElementType()) {
      const auto *variable = dyn_cast<VariableArrayType>(array);
      if (variable && variable->getSizeExpr()) {
        reportIncomplete("unrepresentable variable array byte extent",
                         *variable->getSizeExpr());
        break;
      }
    }
    return;
  }
  const auto extent = internIntegerExpression(*expression, state);
  state.spatial.set(place,
                    core::SpatialRecord{.extent = extent,
                                        .location = locate(var.getLocation()),
                                        .declared = true});
}

bool FunctionDataflow::checkVariableArray(const Expr &expr,
                                          core::AnalysisState &state) {
  if (!isa<ArraySubscriptExpr>(expr))
    return false;

  const Expr *root = &expr;
  bool dynamic = false;
  while (const auto *subscript = dyn_cast<ArraySubscriptExpr>(root)) {
    root = &PlaceBuilder::stripTransparent(*subscript->getBase());
    dynamic |= root->getType()->isVariablyModifiedType();
  }
  if (!dynamic)
    return false;

  // A dimension fitting does not prove that its enclosing storage exists.
  // Require the declaration-time byte product, including fixed dimensions,
  // to have been representable. Pointer-to-VLA types only describe rows;
  // without a represented byte offset they cannot prove backing storage.
  bool storageKnown = false;
  if (const auto *ref = dyn_cast<DeclRefExpr>(root)) {
    if (const auto *var = dyn_cast<VarDecl>(ref->getDecl());
        var && var->getType()->isArrayType()) {
      const auto record = state.spatial.recordOf(builder.placeForVar(*var));
      storageKnown = record && record->extent.has_value();
    }
  }

  const auto boundsOf = [&](const core::Affine &value) {
    return value.place ? integerBounds(*value.place, state)
                       : std::pair<std::optional<std::int64_t>,
                                   std::optional<std::int64_t>>{};
  };
  const auto visit = [&](const auto &self,
                         const Expr &access) -> core::SpatialCheck {
    const auto *subscript = dyn_cast<ArraySubscriptExpr>(&access);
    if (!subscript)
      return {.outcome = core::SpatialOutcome::Proven,
              .reason = core::SpatialReason::None};
    const auto &base = PlaceBuilder::stripTransparent(*subscript->getBase());
    const auto enclosing = self(self, base);
    const auto *array = context.getAsArrayType(base.getType());
    std::optional<core::Affine> dimension;
    const Expr *bound = nullptr;
    if (const auto *variable = dyn_cast_or_null<VariableArrayType>(array)) {
      bound = variable->getSizeExpr();
      const auto count = variableArrayCounts.find(variable);
      if (count != variableArrayCounts.end() &&
          state.numericValues.contains(count->second))
        dimension = core::Affine::ofPlace(count->second);
    } else if (const auto *fixed = dyn_cast_or_null<ConstantArrayType>(array);
               fixed && fixed->getSize().getActiveBits() <= 63) {
      dimension = core::Affine::ofConstant(
          static_cast<std::int64_t>(fixed->getSize().getZExtValue()));
    }
    core::SpatialCheck check{.reason = core::SpatialReason::UnknownExtent};
    const auto index = integerAffineOf(*subscript->getIdx(), state);
    const auto end = index ? index->shifted(1) : std::nullopt;
    if (!index || !end) {
      check.reason = core::SpatialReason::Arithmetic;
      reportIncomplete("unsupported variable array access", access);
    } else if (dimension) {
      const auto start = foldAffine(*index, state);
      const auto need = foldAffine(*end, state);
      const auto have = foldAffine(*dimension, state);
      const auto [lo, hi] = boundsOf(need);
      const auto [haveLo, haveHi] = boundsOf(have);
      const auto relation =
          need.place && have.place
              ? state.relations.between(*need.place, *have.place)
              : std::nullopt;
      check = core::checkSpatialBounds(start, need, have, relation,
                                       {.needAtMost = hi,
                                        .haveAtMost = haveHi,
                                        .needAtLeast = lo,
                                        .haveAtLeast = haveLo},
                                       boundsOf(start).first);
      if (check.outcome == core::SpatialOutcome::Violation) {
        auto diagnostic =
            makeError(core::diag::OutOfBounds,
                      "'" + spellIndex(&access, need) +
                          "' is out of bounds for its variable array dimension",
                      access);
        if (bound)
          diagnostic.addNote("the dimension is evaluated here", locate(*bound));
        else
          diagnostic.addNote("the array is declared here", locate(base));
        report(std::move(diagnostic));
      }
    }
    // Every enclosing subscript and the full byte product must fit. Keep
    // independently established dimension violations even when another
    // dimension or the byte extent is unrepresentable.
    if (check.outcome != core::SpatialOutcome::Violation) {
      if (enclosing.outcome != core::SpatialOutcome::Proven)
        check = enclosing;
      else if (check.outcome == core::SpatialOutcome::Proven && !storageKnown)
        check = {.reason = core::SpatialReason::UnknownExtent};
    }
    recordSpatialCheck(access, check);
    return check;
  };
  visit(visit, expr);
  return true;
}

core::SpatialRecord
FunctionDataflow::subobjectRecord(const core::SpatialRecord &record,
                                  core::PlaceId source,
                                  const core::PointerOffset &step) {
  auto result = record.derived(step);
  if (!step.isField() || step.negative || !record.extent ||
      !record.boundsOffset.value_or(record.offset).isZero())
    return result;
  const auto *decl = dyn_cast_or_null<ValueDecl>(builder.declFor(source));
  if (!decl || !decl->getType()->isPointerType())
    return result;
  QualType type = decl->getType()->getPointeeType();
  const auto names = PlaceBuilder::fieldsOfOffset(step);
  std::vector<core::PathElem> fields;
  fields.reserve(names.size());
  for (const auto &name : names)
    fields.push_back({.step = core::PathStep::Field, .field = name});
  // The key names the record whose layout was used by the source expression.
  // A cast to an unrelated record must not substitute the old layout.
  if (builder.fieldKeyFor(type, fields) != step.field)
    return result;
  std::int64_t offset = 0;
  const FieldDecl *last = nullptr;
  for (const auto &name : names) {
    const auto *recordDecl = type->getAsRecordDecl();
    if (!recordDecl || !recordDecl->isCompleteDefinition())
      return result;
    const FieldDecl *selected = nullptr;
    for (const auto *field : recordDecl->fields())
      if (field->getName() == llvm::StringRef(name)) {
        selected = field;
        break;
      }
    if (!selected || selected->isBitField())
      return result;
    const auto bits = context.getFieldOffset(selected);
    const auto byteWidth = context.getCharWidth();
    if (bits % byteWidth != 0 ||
        bits / byteWidth > static_cast<std::uint64_t>(INT64_MAX))
      return result;
    if (__builtin_add_overflow(
            offset, static_cast<std::int64_t>(bits / byteWidth), &offset))
      return result;
    type = selected->getType();
    last = selected;
  }
  if (!last || !type->isArrayType())
    return result;
  const auto tail = record.extent->shifted(-offset);
  if (!tail)
    return result;
  if (type->isIncompleteArrayType()) {
    if (!last->getParent()->hasFlexibleArrayMember())
      return result;
    result.extent = tail;
  } else {
    const auto fixed = byteSizeOf(type, context);
    // A fixed subobject cannot widen a backing allocation that is too small.
    if (!fixed || !tail->isConstant())
      return result;
    result.extent = core::Affine::ofConstant(std::min(*fixed, tail->constant));
  }
  result.boundsOffset = core::PointerOffset::zero();
  return result;
}

} // namespace weavec::analysis
