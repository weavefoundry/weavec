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
      decideIncomplete("side-effecting variable array dimensions", *bound);
      continue;
    }
    const auto actual = integerRangeOf(*bound, state);
    const auto expression = integerExpressionOf(*bound, state);
    if (!actual || !expression || actual->mayBeInvalid ||
        actual->values.empty()) {
      decideIncomplete("unsupported variable array dimension", *bound);
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
      decideIncomplete("unrepresentable variable array dimension", *bound);
      continue;
    }
    const auto converted = expression->converted(*sizeType);
    if (!converted) {
      decideIncomplete("unsupported variable array dimension", *bound);
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
        decideIncomplete("unrepresentable variable array byte extent",
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
      decideIncomplete("unsupported variable array access", access);
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
      // RFC 0030 §3.3: definite only (the dimension is an exact extent);
      // a boundary value that may be past it is a checked facet.
      if (check.outcome == core::SpatialOutcome::Violation && check.violation &&
          (check.violation->kind == core::BoundsVerdict::Kind::OutOfBounds ||
           check.violation->kind == core::BoundsVerdict::Kind::BeforeStart ||
           check.violation->kind ==
               core::BoundsVerdict::Kind::AtLeastPastEnd)) {
        auto diagnostic =
            makeError(core::diag::OutOfBounds,
                      "'" + spellIndex(&access, need) +
                          "' is out of bounds for its variable array dimension",
                      access);
        if (bound)
          diagnostic.addNote("the dimension is evaluated here", locate(*bound));
        else
          diagnostic.addNote("the array is declared here", locate(base));
        const SiteInfo *site = accessSite(access, core::Facet::Spatial);
        decide(site, core::Facet::Spatial, core::FacetDecision::violation());
        report(std::move(diagnostic), core::Certainty::Definite, site,
               core::Facet::Spatial);
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
    // §3.3 for what is not a definite violation (the extent is the exact
    // dimension, so a check against it falls back to the declarations).
    if (check.outcome != core::SpatialOutcome::Violation)
      decideSpatial(accessSite(access, core::Facet::Spatial), check, nullptr);
    else if (!check.violation ||
             (check.violation->kind != core::BoundsVerdict::Kind::OutOfBounds &&
              check.violation->kind != core::BoundsVerdict::Kind::BeforeStart &&
              check.violation->kind !=
                  core::BoundsVerdict::Kind::AtLeastPastEnd))
      decide(accessSite(access, core::Facet::Spatial), core::Facet::Spatial,
             core::FacetDecision::checked());
    return check;
  };
  visit(visit, expr);
  return true;
}

std::optional<core::SpatialRecord>
FunctionDataflow::completeStorageRecordOf(const PlaceRef &storage,
                                          const core::PointerOffset &offset) {
  // The path from the variable to the storage, outermost first.
  std::vector<core::PlaceId> path;
  for (core::PlaceId cursor = storage.place; !places.isBase(cursor);
       cursor = *places.parent(cursor))
    path.push_back(cursor);
  std::ranges::reverse(path);
  const core::PlaceId root = places.root(storage.place);
  const VarDecl *var = builder.varForPlace(root);
  if (var == nullptr || isa<ParmVarDecl>(var))
    return std::nullopt;
  QualType type = var->getType();
  std::optional<core::Affine> extent;
  if (type->isVariableArrayType()) {
    const auto record =
        currentState ? currentState->spatial.recordOf(root) : std::nullopt;
    if (!record || !record->extent)
      return std::nullopt;
    extent = record->extent;
  } else if (const auto size = byteSizeOf(type, context)) {
    extent = core::Affine::ofConstant(*size);
  } else {
    return std::nullopt;
  }
  // Where the sub-object starts, in bytes, while every step is a field; a
  // subscript on the way makes it somewhere inside.
  bool known = true;
  std::int64_t bytes = 0;
  for (const core::PlaceId step : path) {
    if (places.step(step) == core::PathStep::Field) {
      const RecordDecl *record = type->getAsRecordDecl();
      const FieldDecl *selected = nullptr;
      if (record != nullptr && record->isCompleteDefinition())
        for (const FieldDecl *field : record->fields())
          if (field->getName() == llvm::StringRef(places.fieldName(step))) {
            selected = field;
            break;
          }
      if (selected == nullptr || selected->isBitField())
        return core::SpatialRecord{.extent = extent,
                                   .offset = core::PointerOffset::inside(),
                                   .location = locate(var->getLocation()),
                                   .declared = true,
                                   .boundsOffset =
                                       core::PointerOffset::inside()};
      const std::uint64_t bits = context.getFieldOffset(selected);
      if (bits % context.getCharWidth() != 0 ||
          __builtin_add_overflow(
              bytes, static_cast<std::int64_t>(bits / context.getCharWidth()),
              &bytes))
        known = false;
      type = selected->getType();
    } else if (places.step(step) == core::PathStep::Index) {
      const auto *array = context.getAsArrayType(type);
      if (array == nullptr)
        return std::nullopt;
      // The element the storage names is not known here (a decay names
      // its first; `&a[3]` counts in `offset`).
      if (step != path.back())
        known = false;
      type = array->getElementType();
    } else {
      return std::nullopt;
    }
  }
  // What the pointer points to: the storage's element when it names an
  // array's elements, else the storage itself.
  QualType element = type;
  if (!path.empty() && places.step(path.back()) != core::PathStep::Index)
    if (const auto *array = context.getAsArrayType(type))
      element = array->getElementType();
  const auto unit = byteSizeOf(element, context);
  core::PointerOffset position = core::PointerOffset::inside();
  if (known && unit && *unit > 0 && bytes % *unit == 0) {
    position = core::PointerOffset::ofElements(bytes / *unit);
    if (offset.isElements() || offset.isZero())
      position = position.plus(offset);
    else
      position = core::PointerOffset::inside();
  }
  return core::SpatialRecord{.extent = extent,
                             .offset = position,
                             .location = locate(var->getLocation()),
                             .declared = true,
                             .boundsOffset = std::nullopt};
}

core::SpatialRecord
FunctionDataflow::subobjectRecord(const core::SpatialRecord &record,
                                  core::PlaceId source,
                                  const core::PointerOffset &step) {
  // RFC 0030 §7.4: a pointer made by `&member` or by the decay of a member
  // array has the extent of the complete object, as
  // `__builtin_object_size` mode 0 does: `memset(&s->first, 0, sizeof *s)`
  // and a copy into `(char *)&ts->contents` are in bounds, and a trailing
  // array (flexible at `-fstrict-flex-arrays=0`, whatever its bound) spans
  // the rest of the allocation. Only a direct subscript of a non-flexible
  // member array uses the member's bound (`checkBounds`). The bounds count
  // from the complete object's start: the member's byte offset, in
  // elements of what the new pointer points to.
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
  if (!last)
    return result;
  // What the new pointer points to: an array member's element (it decays),
  // or the member itself.
  QualType element = type;
  if (const auto *array = context.getAsArrayType(type))
    element = array->getElementType();
  const auto unit = byteSizeOf(element, context);
  if (!unit || *unit <= 0 || offset % *unit != 0)
    result.boundsOffset = core::PointerOffset::inside();
  else
    result.boundsOffset = core::PointerOffset::ofElements(offset / *unit);
  return result;
}

} // namespace weavec::analysis
