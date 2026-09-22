//===- DataflowMemory.cpp - Complete object-representation copies --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"
#include "weavec/Analysis/Allocators.h"

#include "clang/AST/Expr.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/Type.h"

using namespace clang;

namespace weavec::analysis {

/// The facet an incompleteness leaves undecidable: what the construct the
/// engine could not model feeds. Integer, numeric, extent and variable-array
/// modelling decides spatial facets; the rest (array elements, ranges and
/// cleanups, call and callback contexts, object views, copies of
/// pointer-containing storage) is about which objects are live.
core::Facet FunctionDataflow::incompleteFacet(llvm::StringRef reason) {
  return reason.contains("integer") || reason.contains("numeric") ||
                 reason.contains("extent") || reason.contains("variable array")
             ? core::Facet::Spatial
             : core::Facet::Temporal;
}

void FunctionDataflow::decideIncomplete(const std::string &reason,
                                        const Stmt &at) {
  if (!recording())
    return;
  // A decision-only pass over a path (`decidePathBounds`) leaves the
  // summary as it is.
  if (!boundsDecisionOnly)
    inferred.incomplete.insert(reason);
  // RFC 0030 §15 item 3: no diagnostic; the facet of the site `at` stands
  // for, or of the innermost site around it, is unresolved, with the text
  // as the detail.
  const core::Facet facet = incompleteFacet(reason);
  if (!publishing())
    return;
  const SiteInfo *site = nullptr;
  if (const auto *expr = dyn_cast<Expr>(&at))
    site = accessSite(*expr, facet);
  if (site == nullptr) {
    // Not an operand's site: the construct is inside the operation.
    if (!parentMap)
      parentMap = std::make_unique<ParentMap>(function.getBody());
    for (const Stmt *cursor = &at; cursor != nullptr && site == nullptr;
         cursor = parentMap->getParent(cursor)) {
      if (cursor != &at && !isa<Expr>(cursor))
        break;
      for (const core::SiteId id : ledger.siteIndex().sitesOf(*cursor))
        if (ledger.applies(id, facet)) {
          site = ledger.siteIndex().info(id);
          break;
        }
    }
  }
  // A store the engine could not follow (`a[i] = 0` in a fill loop): the
  // element it writes.
  if (site == nullptr)
    if (const auto *assign = dyn_cast<BinaryOperator>(&at);
        assign != nullptr && assign->isAssignmentOp())
      site =
          accessSite(PlaceBuilder::stripTransparent(*assign->getLHS()), facet);
  decide(site, facet,
         core::FacetDecision::unresolvedFor(core::incompletenessReason(reason),
                                            reason));
}

static bool containsPointer(QualType type, unsigned depth = 0) {
  if (type.isNull() || depth > core::MaxHeapPathDepth)
    return false;
  if (type->isPointerType())
    return true;
  if (const auto *array = type->getAsArrayTypeUnsafe())
    return containsPointer(array->getElementType(), depth + 1);
  if (const RecordDecl *record = type->getAsRecordDecl()) {
    if (!record->isCompleteDefinition())
      return false;
    for (const FieldDecl *field : record->fields())
      if (containsPointer(field->getType(), depth + 1))
        return true;
  }
  return false;
}

void FunctionDataflow::noteReinterpretingStore(const Expr &lvalue,
                                               const PlaceRef &written,
                                               core::AnalysisState &state) {
  if (lvalue.getType()->isPointerType())
    return;
  const Expr &e = PlaceBuilder::stripTransparent(lvalue);
  // `u.l = 1`: the union's pointer members now hold bytes a non-pointer
  // member wrote.
  if (const auto *member = dyn_cast<MemberExpr>(&e)) {
    const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
    const auto parent = places.parent(written.place);
    if (field != nullptr && field->getParent()->isUnion() && parent)
      for (const FieldDecl *sibling : field->getParent()->fields())
        if (sibling != field && sibling->getType()->isPointerType())
          state.reinterpreted.insert(builder.fieldPlace(*parent, *sibling));
    return;
  }
  // `dst[i] = b` with `dst = (unsigned char *)&q`: a byte of a pointer
  // object is rewritten.
  const auto access = accessOf(e);
  if (!access || access->base == nullptr)
    return;
  const ValueOrigin origin = builder.classifyValue(*access->base);
  if (!origin.place)
    return;
  std::vector<core::PlaceId> targets;
  if (origin.kind == ValueOrigin::Kind::Borrow)
    targets.push_back(origin.place->place);
  else if (origin.kind == ValueOrigin::Kind::Copy)
    for (const core::Loan &loan : state.loans.heldBy(origin.place->place))
      targets.push_back(loan.place);
  for (const core::PlaceId target : targets)
    for (const core::PlaceId cell : storageOf(target))
      if (const auto *decl =
              dyn_cast_if_present<ValueDecl>(builder.declFor(cell));
          decl != nullptr && decl->getType()->isPointerType())
        state.reinterpreted.insert(cell);
}

bool FunctionDataflow::handleMemoryCopy(const CallExpr &call,
                                        const CallEffects &effects,
                                        core::AnalysisState &state) {
  // RFC 0030 §8: a row that copies bytes from one argument to another by a
  // length that is itself an argument (`memcpy`, `memmove`, `bcopy`), not a
  // bounded string copy (`strncpy`'s `min(...)`).
  const core::LibraryMatch *library = resolvedLibrary(call);
  if (library == nullptr || effects.source != SummarySource::Library ||
      library->entry->copies.size() != 1 ||
      library->entry->copies.front().length.kind !=
          core::LibTerm::Kind::Argument)
    return false;
  const core::LibCopy &copy = library->entry->copies.front();
  const int to = library->callArgument(copy.dst);
  const int from = library->callArgument(copy.src);
  const int count = library->callArgument(copy.length.arg);
  if (to < 0 || from < 0 || count < 0 ||
      static_cast<unsigned>(std::max({to, from, count})) >= call.getNumArgs())
    return false;
  const auto destIndex = static_cast<unsigned>(to);
  const auto sourceIndex = static_cast<unsigned>(from);
  const auto lengthIndex = static_cast<unsigned>(count);

  // RFC 0015's element-wise copy reads `memcpy`'s argument order.
  if (destIndex == 0 && sourceIndex == 1 && lengthIndex == 2 &&
      handleArrayCopy(call, effects, state))
    return true;

  const Expr &destExpr = *call.getArg(destIndex)->IgnoreParenImpCasts();
  const Expr &sourceExpr = *call.getArg(sourceIndex)->IgnoreParenImpCasts();
  const auto objectType = [](const Expr &expr) -> QualType {
    QualType type = expr.getType();
    if (const auto *array = type->getAsArrayTypeUnsafe())
      return array->getElementType();
    return type->isPointerType() ? type->getPointeeType() : QualType{};
  };
  const QualType destType = objectType(destExpr);
  const QualType sourceType = objectType(sourceExpr);
  if (!containsPointer(destType) && !containsPointer(sourceType))
    return false;
  const auto storage = [&](const Expr &expr) -> std::optional<PlaceRef> {
    if (auto addressed = builder.addressedPlace(expr))
      return addressed;
    if (auto pointer = builder.resolvePointerValue(expr)) {
      pointer->addDeref(pointer->place, &expr);
      pointer->place = places.deref(pointer->place);
      return pointer;
    }
    return std::nullopt;
  };
  const auto dest = storage(destExpr);
  const auto source = storage(sourceExpr);
  auto bytes = builder.affineOf(*call.getArg(lengthIndex));
  if (bytes)
    bytes = foldAffine(*bytes, state);
  const auto size = byteSizeOf(sourceType, context);
  if (bytes && bytes->isConstant() && bytes->constant == 0)
    return false;
  const bool compatible =
      !destType.isNull() && !sourceType.isNull() &&
      ASTContext::hasSameUnqualifiedType(destType, sourceType);
  const bool complete =
      dest && source && dest->element.isWhole() && source->element.isWhole() &&
      size && bytes && bytes->isConstant() && bytes->constant == *size &&
      compatible && (sourceType->isPointerType() || sourceType->isRecordType());
  if (!complete) {
    if (dest) {
      for (const auto cell : storageOf(dest->place)) {
        escape(cell, state);
        state.forget(cell);
        // RFC 0030 §2.3: what the copy left in a pointer is a
        // reinterpretation the engine did not follow.
        state.reinterpreted.insert(cell);
      }
      state.incompleteHeap.insert(dest->place);
      if (destType->isFunctionPointerType())
        state.callTargets[dest->place] = core::CallTargets::any();
    }
    decideIncomplete("unsupported memory copy of pointer-containing storage",
                     call);
    return false;
  }

  checkRequiredArguments(call, *effects.summary, state);
  checkRequiredExtents(call, *effects.summary, state);
  checkAnnotationOnWrite(*dest, call, state);
  recordAccess(source->place, false, state);
  recordAccess(dest->place, true, state);
  if (source->place == dest->place)
    return true;

  auto it = memorySnapshots.find(&call);
  if (it == memorySnapshots.end())
    it = memorySnapshots.emplace(&call, places.create("copy-input")).first;
  const core::PlaceId snapshot = it->second;
  pointerSnapshots.insert(snapshot);
  if (sourceType->isPointerType()) {
    copyHeapValue(source->place, snapshot, state);
    ValueOrigin origin;
    origin.kind = ValueOrigin::Kind::Copy;
    origin.place = PlaceRef{.place = snapshot, .derefs = {}, .element = {}};
    noteRewritten(dest->place, state);
    noteOverwritten(dest->place, state);
    applyPointerAssign(dest->place, origin, call,
                       sourceType->getPointeeType().isConstQualified(), state);
    applyHeapValue(dest->place, origin, state);
  } else {
    copyRecordPlaces(snapshot, source->place, state);
    const auto storagePlaces = storageOf(dest->place);
    const std::set<core::PlaceId> going(storagePlaces.begin(),
                                        storagePlaces.end());
    checkLeaks(
        storagePlaces, [&going](core::PlaceId p) { return going.contains(p); },
        LeakForm::Overwritten, locate(call), state);
    noteRewritten(dest->place, state);
    noteOverwritten(dest->place, state);
    copyRecordPlaces(dest->place, snapshot, state);
    for (const auto field : storageOf(dest->place)) {
      if (field == dest->place)
        continue;
      if (const auto input = state.incoming.find(field);
          input != state.incoming.end())
        noteCalleeStore(field, call, state);
    }
  }
  // Output capture uses the normal heap machinery, with immutable source
  // identities retained while this copy's scratch holders are retired.
  for (const auto child : places.descendants(snapshot))
    state.forget(child);
  state.forget(snapshot);
  return true;
}

} // namespace weavec::analysis
