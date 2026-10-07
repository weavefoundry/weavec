//===- KindInferenceFields.cpp - Counted relations of fields (RFC 0030) ---===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// §7.6: the counted-field candidates of the unit's structs, with the
// disqualifications that hold before round 1.
//
//===----------------------------------------------------------------------===//

#include "KindInferenceImpl.h"

#include "clang/Basic/SourceManager.h"

#include "llvm/ADT/STLExtras.h"

#include <cstdint>
#include <functional>
#include <map>
#include <utility>

namespace weavec::analysis {

/// `record` and every record nested in it by value.
static void
forEachNestedRecord(const clang::RecordDecl *record,
                    const clang::ASTContext &context,
                    const std::function<void(const clang::RecordDecl &)> &visit,
                    unsigned depth = 0) {
  if (record == nullptr || depth > 16)
    return;
  if (const clang::RecordDecl *definition = record->getDefinition())
    record = definition;
  visit(*record);
  for (const clang::FieldDecl *field : record->fields()) {
    clang::QualType type = field->getType();
    while (const clang::ArrayType *array = context.getAsArrayType(type))
      type = array->getElementType();
    if (const clang::RecordDecl *nested = type->getAsRecordDecl())
      forEachNestedRecord(nested, context, visit, depth + 1);
  }
}

void KindInferenceState::inferFieldCandidates() {
  const clang::SourceManager &sm = context.getSourceManager();
  // Before round 1 (§7.6): the writes the store-group rule cannot see drop
  // every candidate of the struct.
  llvm::DenseMap<const clang::RecordDecl *,
                 std::pair<std::string, const clang::Stmt *>>
      dropped;
  const auto drop = [&](const clang::RecordDecl &record, std::string reason,
                        const clang::Stmt *where) {
    const clang::RecordDecl *definition = record.getDefinition();
    dropped.try_emplace(definition != nullptr ? definition : &record,
                        std::move(reason), where);
  };
  std::vector<std::pair<const clang::FieldDecl *, const clang::Stmt *>>
      addresses(fieldAddresses.begin(), fieldAddresses.end());
  std::ranges::stable_sort(addresses, [&sm](const auto &a, const auto &b) {
    return sm.isBeforeInTranslationUnit(a.second->getBeginLoc(),
                                        b.second->getBeginLoc());
  });
  for (const auto &[field, where] : addresses)
    drop(*field->getParent(),
         "the address of '" + fieldName(*field) + "' is taken", where);
  for (const ByteWrite &write : byteWrites)
    forEachNestedRecord(
        write.record, context, [&](const clang::RecordDecl &record) {
          drop(record,
               "an object of '" + recordName(record) +
                   "' is written byte-wise (" + write.demotion.reason + ")",
               write.demotion.store);
        });
  for (const Conversion &conversion : conversions)
    forEachNestedRecord(conversion.record, context,
                        [&](const clang::RecordDecl &record) {
                          drop(record,
                               "an object of '" + recordName(record) +
                                   "' is converted from another type",
                               conversion.where);
                        });
  for (const clang::RecordDecl *record : records) {
    if (!record->isUnion())
      continue;
    const auto members =
        std::distance(record->field_begin(), record->field_end());
    for (const clang::FieldDecl *field : record->fields())
      if (const clang::RecordDecl *nested = field->getType()->getAsRecordDecl();
          nested != nullptr && members > 1)
        drop(*nested,
             "'" + recordName(*nested) + "' shares the union '" +
                 recordName(*record) + "' with other members",
             nullptr);
  }

  for (const clang::RecordDecl *record : records) {
    if (record->isUnion() || sm.isInSystemHeader(record->getLocation()))
      continue;
    const std::string key = recordKey(*record, context);
    if (key.empty())
      continue;
    std::vector<const clang::FieldDecl *> pointers;
    std::vector<const clang::FieldDecl *> counts;
    for (const clang::FieldDecl *field : record->fields()) {
      const clang::QualType type = field->getType();
      if (isObjectPointer(type)) {
        // A declared kind needs no candidate; an opaque pointee has no
        // element size to count.
        const KindEntry *entry = kinds.field(*field);
        const bool declared = entry != nullptr && entry->hasDeclaredShape();
        const clang::QualType pointee = type->getPointeeType();
        if (!declared &&
            (pointee->isVoidType() || !pointee->isIncompleteType()))
          pointers.push_back(field);
      } else if (type->isIntegerType() && !type->isBooleanType()) {
        counts.push_back(field);
      }
    }
    if (pointers.empty() || counts.empty())
      continue;
    if (const auto found = dropped.find(record); found != dropped.end()) {
      disqualified.push_back(DisqualifiedRecord{.record = record,
                                                .key = key,
                                                .reason = found->second.first,
                                                .where = found->second.second});
      continue;
    }
    for (const clang::FieldDecl *pointer : pointers) {
      const clang::QualType pointee = pointer->getType()->getPointeeType();
      // One unit is enough where elements are bytes: `bytes` for `void`,
      // `count` for character pointees.
      const bool wantCount = !pointee->isVoidType();
      const bool wantBytes =
          objectWidth(pointee, context) != 1 || pointee->isVoidType();
      for (const clang::FieldDecl *count : counts) {
        if (options.pruneUnrelatedCandidates &&
            !related.contains(
                {pointer->getCanonicalDecl(), count->getCanonicalDecl()}))
          continue;
        for (const std::int64_t offset : {0, 1}) {
          for (const bool bytes : {false, true}) {
            if ((bytes && !wantBytes) || (!bytes && !wantCount))
              continue;
            candidates.push_back(ResolvedCandidate{
                .candidate =
                    FieldCandidate{.record = key,
                                   .pointer = pointer->getNameAsString(),
                                   .count = count->getNameAsString(),
                                   .bytes = bytes,
                                   .offset = offset},
                .record = record,
                .pointer = pointer,
                .count = count});
          }
        }
      }
    }
  }
}

} // namespace weavec::analysis
