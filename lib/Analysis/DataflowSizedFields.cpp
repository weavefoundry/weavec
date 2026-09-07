//===- DataflowSizedFields.cpp - Counted pointer fields (RFC 0012) --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0012, *Sized fields*: a pointer field whose extent is a sibling
// integer field's value, times the element size. The pair is declared
// (`char *WEAVEC_SIZED_BY(cap) data; size_t cap;`) or inferred from the
// stores every function of the program makes into the field. A load of the
// field with no record of its own gets the record the count implies; a
// store into an annotated field is checked against the count; a store into
// any field of a named record is a witness for, or a refutation of, the
// inferred pair.
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "IntegerSupport.h"
#include "weavec/Core/Diagnostic.h"
#include "weavec/Core/Spatial.h"

#include "clang/AST/Decl.h"

#include "llvm/ADT/STLExtras.h"

#include <string>
#include <utility>

using namespace clang;

namespace weavec::analysis {

bool FunctionDataflow::recordsSizedFields() const noexcept {
  return phase == Phase::Final;
}

std::optional<FunctionDataflow::FieldPlace>
FunctionDataflow::fieldPlaceOf(core::PlaceId place) {
  if (places.isBase(place) || places.step(place) != core::PathStep::Field)
    return std::nullopt;
  const auto *field = dyn_cast_if_present<FieldDecl>(builder.declFor(place));
  if (field == nullptr)
    return std::nullopt;
  const auto parent = places.parent(place);
  if (!parent)
    return std::nullopt;
  // RFC 0012, *Annotation surface*: the annotation goes on a pointer field;
  // on anything else it is reported once, at the field, when the field is
  // first touched.
  if (!field->getType()->isPointerType() && phase == Phase::Final &&
      emitDiagnostics && !getAnnotations(*field).sizedBy.empty() &&
      summaries.noteInvalidSizedField(*field)) {
    report(core::Diagnostic{
        .severity = core::Severity::Warning,
        .id = core::diag::InvalidAnnotation,
        .message = "field '" + field->getNameAsString() +
                   "' is declared WEAVEC_SIZED_BY(" +
                   getAnnotations(*field).sizedBy + ") but is not a pointer",
        .location = locate(field->getLocation()),
        .notes = {},
        .fixits = {},
    });
  }
  return FieldPlace{
      .object = *parent, .field = field, .key = fieldKeyOf(*field, context)};
}

/// The field of `record` whose count-field key is `key`, if any.
static const FieldDecl *fieldWithKey(const RecordDecl &record,
                                     std::string_view key,
                                     const ASTContext &context) {
  for (const FieldDecl *candidate : record.fields()) {
    if (fieldKeyOf(*candidate, context) == key)
      return candidate;
  }
  return nullptr;
}

std::optional<FunctionDataflow::SizedFieldPlace>
FunctionDataflow::sizedFieldPlaceOf(core::PlaceId place) {
  const auto field = fieldPlaceOf(place);
  if (!field || !field->field->getType()->isPointerType())
    return std::nullopt;
  const FieldDecl &decl = *field->field;
  // The annotation is authoritative (RFC 0012, *Annotation surface*).
  const AnnotationSet annotations = getAnnotations(decl);
  if (!annotations.sizedBy.empty()) {
    if (const auto sized = sizedFieldOf(decl)) {
      return SizedFieldPlace{
          .count = builder.fieldPlace(field->object, *sized->count),
          .unit = sized->unit,
          .annotated = true};
    }
    if (phase == Phase::Final && emitDiagnostics &&
        summaries.noteInvalidSizedField(decl)) {
      std::string record = "the record";
      if (const RecordDecl *parent = decl.getParent();
          parent != nullptr && parent->getIdentifier() != nullptr) {
        record = "'" + std::string(parent->isUnion() ? "union " : "struct ") +
                 parent->getNameAsString() + "'";
      }
      report(core::Diagnostic{
          .severity = core::Severity::Warning,
          .id = core::diag::InvalidAnnotation,
          .message = "field '" + decl.getNameAsString() +
                     "' is declared WEAVEC_SIZED_BY(" + annotations.sizedBy +
                     ") but '" + annotations.sizedBy +
                     "' is not an integer field of " + record,
          .location = locate(decl.getLocation()),
          .notes = {},
          .fixits = {},
      });
    }
    return std::nullopt;
  }
  // Else what the program's stores agree on (RFC 0012, "Inference").
  const auto confirmed = summaries.confirmedSizedWitness(field->key);
  if (!confirmed || decl.getParent() == nullptr)
    return std::nullopt;
  const FieldDecl *count =
      fieldWithKey(*decl.getParent(), confirmed->count, context);
  if (count == nullptr || !count->getType()->isIntegerType())
    return std::nullopt;
  return SizedFieldPlace{.count = builder.fieldPlace(field->object, *count),
                         .unit = confirmed->scale,
                         .annotated = false,
                         .productType = confirmed->productType};
}

std::optional<core::SpatialRecord>
FunctionDataflow::spatialRecordAt(core::PlaceId place,
                                  const core::AnalysisState &state) {
  // What this function did is more precise than the invariant: a record
  // the state holds stands (RFC 0012, *Sized fields*, "Loads").
  if (const auto record = state.spatial.recordOf(place))
    return record;
  const auto field = fieldPlaceOf(place);
  if (!field || !field->field->getType()->isPointerType())
    return std::nullopt;
  // An unannotated field looked up here is one a pair the program confirms
  // later would decide: the unit says so in its exports.
  if (!field->key.empty() && getAnnotations(*field->field).sizedBy.empty())
    summaries.noteSizedFieldLoad(field->key);
  const auto sized = sizedFieldPlaceOf(place);
  if (!sized)
    return std::nullopt;
  auto extent = core::Affine::ofPlace(sized->count, sized->unit);
  if (sized->productType) {
    const auto *count =
        dyn_cast_or_null<ValueDecl>(builder.declFor(sized->count));
    const auto type = count ? integerTypeOf(*count, context) : std::nullopt;
    if (!type)
      return std::nullopt;
    const auto input = NumericExpression::input(sized->count, *type)
                           .converted(*sized->productType);
    const auto expression =
        input ? NumericExpression::operation(
                    core::IntegerOp::Multiply, *input,
                    NumericExpression::constant(core::IntegerValue::ofBits(
                        *sized->productType,
                        static_cast<std::uint64_t>(sized->unit))))
              : std::nullopt;
    if (!expression)
      return std::nullopt;
    // Registration names the expression only. The immutable state supplied
    // by the caller is evaluated when the spatial check consumes this record.
    auto found = expressionPlaces.find(*expression);
    if (found == expressionPlaces.end()) {
      const auto id = places.create(expression->describe(
          [&](core::PlaceId input) { return nameOf(input); }));
      found = expressionPlaces.emplace(*expression, id).first;
      numericExpressions.emplace(id, *expression);
    }
    extent = core::Affine::ofPlace(found->second);
  }
  return core::SpatialRecord{.extent = extent,
                             .offset = {},
                             .location = locate(field->field->getLocation()),
                             .declared = true};
}

std::pair<std::optional<core::Affine>, std::optional<core::IntegerType>>
FunctionDataflow::sizedFieldExtent(const core::Affine &extent,
                                   const core::AnalysisState &state) {
  if (!extent.place || extent.constant != 0)
    return {std::nullopt, std::nullopt};
  const auto expression = numericExpressions.find(*extent.place);
  if (expression == numericExpressions.end())
    return {extent, std::nullopt};
  const auto &node = expression->second.all().back();
  if (node.kind != core::IntegerNodeKind::Operation ||
      node.op != core::IntegerOp::Multiply || node.type.isSigned ||
      extent.scale != 1)
    return {std::nullopt, std::nullopt};
  const auto linear = linearIntegerExpression(expression->second, state, true);
  if (!linear || !linear->place || linear->constant != 0 || linear->scale <= 0)
    return {std::nullopt, std::nullopt};
  return {linear, node.type};
}

std::string FunctionDataflow::spellBytes(const core::Affine &amount) {
  if (amount.isConstant())
    return std::to_string(amount.constant) + " bytes";
  std::string text = "'" + nameOf(*amount.place) + "'";
  if (amount.scale != 1)
    text += " * " + std::to_string(amount.scale);
  if (amount.constant != 0)
    text += (amount.constant > 0 ? " + " : " - ") +
            std::to_string(unsignedMagnitude(amount.constant));
  return text + " bytes";
}

bool FunctionDataflow::checkSizedFieldStore(core::PlaceId dest,
                                            const SizedFieldPlace &sized,
                                            const core::SpatialRecord &record,
                                            const core::SourceLocation &at,
                                            const core::AnalysisState &state) {
  // RFC 0012, *Sized fields*, "Stores": the count's bytes are the need, the
  // stored value's extent is what there is; only a decided shortfall is a
  // mismatch (a larger object, or an undecided one, is not).
  if (!record.extent || !record.offset.isZero())
    return false;
  const core::Affine need =
      foldAffine(core::Affine::ofPlace(sized.count, sized.unit), state);
  const core::Affine have = foldAffine(*record.extent, state);
  std::optional<core::Relation> between;
  if (need.place && have.place && *need.place != *have.place)
    between = state.relations.between(*need.place, *have.place);
  const auto atMost = [&state](const core::Affine &affine) {
    return affine.place ? state.relations.atMost(*affine.place)
                        : std::optional<std::int64_t>();
  };
  const auto atLeast = [&state](const core::Affine &affine) {
    return affine.place ? state.relations.atLeast(*affine.place)
                        : std::optional<std::int64_t>();
  };
  const auto verdict =
      core::boundsVerdict(need, have, between,
                          core::KnownBounds{.needAtMost = atMost(need),
                                            .haveAtMost = atMost(have),
                                            .needAtLeast = atLeast(need),
                                            .haveAtLeast = atLeast(have)});
  if (!verdict || (verdict->kind != core::BoundsVerdict::Kind::OutOfBounds &&
                   verdict->kind != core::BoundsVerdict::Kind::AtLeastPastEnd))
    return false;
  const auto field = fieldPlaceOf(dest);
  if (!field)
    return false;
  // `says 8`, `says 'n'`, with the element size when it is not a byte.
  std::string says;
  if (need.isConstant())
    says = std::to_string(need.constant / sized.unit);
  else
    says = "'" + nameOf(*need.place) + "'";
  if (verdict->kind == core::BoundsVerdict::Kind::AtLeastPastEnd)
    says = "at least " + std::to_string(verdict->boundary);
  if (sized.unit != 1)
    says += " elements of " + std::to_string(sized.unit) + " bytes";
  core::Diagnostic diagnostic{
      .severity = core::Severity::Error,
      .id = core::diag::AnnotationMismatch,
      .message = "'" + nameOf(dest) + "' is declared WEAVEC_SIZED_BY(" +
                 std::string(places.fieldName(sized.count)) +
                 ") but is given " + spellBytes(have) + " where '" +
                 nameOf(sized.count) + "' says " + says,
      .location = at,
      .notes = {},
      .fixits = {},
  };
  diagnostic.addNote("'" + nameOf(dest) + "' is declared here",
                     locate(field->field->getLocation()));
  report(std::move(diagnostic));
  return true;
}

void FunctionDataflow::noteFieldPointerStore(core::PlaceId dest, const Expr &at,
                                             const core::AnalysisState &state) {
  const auto field = fieldPlaceOf(dest);
  if (!field || !field->field->getType()->isPointerType())
    return;
  const auto record = state.spatial.recordOf(dest);
  if (const auto sized = sizedFieldPlaceOf(dest);
      sized && sized->annotated && record)
    checkSizedFieldStore(dest, *sized, *record, locate(at), state);
  if (!recordsSizedFields() || field->key.empty())
    return;
  // RFC 0012, "Inference": what the store says, decided now when the
  // extent is already a sibling's value (`o->data = malloc(o->cap)`), or
  // later at the sibling's write (`o->data = malloc(n); o->cap = n;`).
  FieldPointerStore store{.place = dest,
                          .field = *field,
                          .location = locate(at),
                          .null = state.resources.isNull(dest),
                          .extent = std::nullopt,
                          .witnessed = std::nullopt};
  if (record && record->extent && record->offset.isZero() &&
      record->extent->place && record->extent->constant == 0) {
    const auto [extent, productType] = sizedFieldExtent(*record->extent, state);
    store.extent = extent;
    store.productType = productType;
    const core::PlaceId counted =
        extent ? *extent->place : *record->extent->place;
    for (const FieldDecl *sibling : field->field->getParent()->fields()) {
      if (sibling == field->field || !sibling->getType()->isIntegerType())
        continue;
      const auto candidate = places.child(field->object, core::PathStep::Field,
                                          sibling->getName());
      if (!candidate)
        continue;
      if (extent && (*candidate == counted ||
                     state.relations.between(counted, *candidate) ==
                         core::Relation::Equal)) {
        store.witnessed.emplace(fieldKeyOf(*sibling, context), extent->scale);
        break;
      }
    }
  }
  fieldPointerStores.push_back(std::move(store));
}

void FunctionDataflow::noteFieldScalarWrite(core::PlaceId place, const Expr *at,
                                            const core::AnalysisState &state) {
  const auto field = fieldPlaceOf(place);
  if (!field || !field->field->getType()->isIntegerType())
    return;
  const core::SourceLocation location =
      at != nullptr ? locate(*at) : core::SourceLocation{};
  const RecordDecl *record = field->field->getParent();
  if (record == nullptr)
    return;
  for (const FieldDecl *sibling : record->fields()) {
    if (sibling == field->field || !sibling->getType()->isPointerType())
      continue;
    const auto pointer =
        places.child(field->object, core::PathStep::Field, sibling->getName());
    if (!pointer)
      continue;
    const auto held = state.spatial.recordOf(*pointer);
    if (!held || !held->extent)
      continue;
    // The annotated check, at the count's store (RFC 0012, "Stores").
    if (const auto sized = sizedFieldPlaceOf(*pointer);
        sized && sized->annotated && sized->count == place)
      checkSizedFieldStore(*pointer, *sized, *held, location, state);
    // The inference: a store whose extent the new value counts.
    if (!recordsSizedFields() || !held->offset.isZero() ||
        !held->extent->place || held->extent->constant != 0)
      continue;
    const auto [extent, productType] = sizedFieldExtent(*held->extent, state);
    if (!extent)
      continue;
    const core::PlaceId counted = *extent->place;
    if (counted == place ||
        state.relations.between(counted, place) == core::Relation::Equal) {
      countWitnesses.push_back(
          CountWitness{.pointer = *pointer,
                       .extent = *extent,
                       .count = fieldKeyOf(*field->field, context),
                       .productType = productType});
    }
  }
  if (recordsSizedFields() && !field->key.empty()) {
    fieldScalarWrites.push_back(FieldScalarWrite{
        .place = place, .field = *field, .location = location});
  }
}

void FunctionDataflow::finalizeSizedFields(
    const core::AnalysisState *exitState) {
  // RFC 0012, *Sized fields*, "Inference". A store the exit finds moved
  // (`o->data = p; ... free(o->data);`) left nothing behind to be counted.
  for (const FieldPointerStore &store : fieldPointerStores) {
    if (store.null)
      continue;
    if (exitState != nullptr && findMoved(store.place, *exitState))
      continue;
    std::optional<std::pair<std::string, std::int64_t>> witnessed =
        store.witnessed;
    if (!witnessed && store.extent) {
      for (const CountWitness &witness : countWitnesses) {
        if (witness.pointer == store.place &&
            witness.extent.place == store.extent->place &&
            witness.extent.scale == store.extent->scale &&
            witness.productType == store.productType) {
          witnessed.emplace(witness.count, witness.extent.scale);
          break;
        }
      }
    }
    if (witnessed && !witnessed->first.empty())
      summaries.addSizedWitness(store.field.key, witnessed->first,
                                witnessed->second, store.productType);
    else
      summaries.refuteSizedField(store.field.key);
  }
  // A count written while its pointer sibling was not stored: the pair is
  // no invariant (`v->n++` beside `v->items`).
  for (const FieldScalarWrite &write : fieldScalarWrites) {
    const RecordDecl *record = write.field.field->getParent();
    if (record == nullptr)
      continue;
    for (const FieldDecl *sibling : record->fields()) {
      if (sibling == write.field.field || !sibling->getType()->isPointerType())
        continue;
      const auto pointer = places.child(
          write.field.object, core::PathStep::Field, sibling->getName());
      const bool stored =
          pointer && llvm::any_of(fieldPointerStores,
                                  [&pointer](const FieldPointerStore &store) {
                                    return store.place == *pointer;
                                  });
      if (stored)
        continue;
      const std::string key = fieldKeyOf(*sibling, context);
      if (!key.empty())
        summaries.refuteSizedPair(key, write.field.key);
    }
  }
}

} // namespace weavec::analysis
