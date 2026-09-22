//===- KindInferenceFields.cpp - Counted relations of fields (RFC 0030) ---===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// §7.6: the counted-field candidates of the unit's structs, with the
// disqualifications that hold before round 1; and §7.4 rule 7: the store
// groups of every declared or candidate relation between a pointer field
// and its count.
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

// -- Store groups (§7.4 rule 7)
// ------------------------------------------------------

/// The designator chain of an lvalue made of variables and member accesses
/// (`b`, `b->s.t`); none for anything else. Variables are told apart by
/// address, so shadowed names never meet.
static std::optional<std::string> chainOf(const clang::Expr *expr) {
  expr = expr->IgnoreParenImpCasts();
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
    const auto *variable = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
    if (variable == nullptr)
      return std::nullopt;
    return variable->getNameAsString() + "@" +
           std::to_string(variable->getCanonicalDecl()->getID());
  }
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
    auto base = chainOf(member->getBase());
    if (!base)
      return std::nullopt;
    return *base + (member->isArrow() ? "->" : ".") +
           member->getMemberDecl()->getNameAsString();
  }
  return std::nullopt;
}

/// The object a member access, subscript or dereference reaches, as the
/// chain of its base followed by `->` or `.`.
static std::optional<std::string> objectOf(const clang::Expr *base,
                                           bool arrow) {
  auto chain = chainOf(base);
  if (!chain)
    return std::nullopt;
  return *chain + (arrow ? "->" : ".");
}

/// A store of a relation field: an assignment or an increment whose lvalue
/// is a member access to one.
static const clang::MemberExpr *
relationStore(const clang::Stmt *stmt,
              const llvm::DenseSet<const clang::FieldDecl *> &fields) {
  const clang::Expr *lvalue = nullptr;
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(stmt);
      binary != nullptr && binary->isAssignmentOp())
    lvalue = binary->getLHS();
  else if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt);
           unary != nullptr && unary->isIncrementDecrementOp())
    lvalue = unary->getSubExpr();
  const auto *member =
      lvalue != nullptr
          ? llvm::dyn_cast<clang::MemberExpr>(lvalue->IgnoreParens())
          : nullptr;
  if (member == nullptr)
    return nullptr;
  const auto *field = llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
  return field != nullptr && fields.contains(field->getCanonicalDecl())
             ? member
             : nullptr;
}

/// The object an access reaches (a member access, a subscript or a
/// dereference), when the syntax names it.
static std::optional<std::string> accessedObject(const clang::Stmt *stmt) {
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(stmt))
    return objectOf(member->getBase(), member->isArrow());
  if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(stmt))
    return objectOf(subscript->getBase(), true);
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt);
      unary != nullptr && unary->getOpcode() == clang::UO_Deref)
    return objectOf(unary->getSubExpr(), true);
  return std::nullopt;
}

void KindInferenceState::collectStoreGroups() {
  // The relation fields: declared counted, sized and ended-by fields with
  // their sibling, and the surviving candidates' pairs.
  llvm::DenseSet<const clang::FieldDecl *> fields;
  for (const auto &[field, entry] : kinds.fieldEntries()) {
    if (!entry.hasDeclaredShape() || !core::hasExtent(entry.kind.shape) ||
        !entry.kind.extent.path ||
        entry.kind.extent.path->root != core::ExtentPath::Root::Field)
      continue;
    for (const clang::FieldDecl *sibling : field->getParent()->fields())
      if (sibling->getName() == entry.kind.extent.path->field) {
        fields.insert(field->getCanonicalDecl());
        fields.insert(sibling->getCanonicalDecl());
      }
  }
  for (const ResolvedCandidate &candidate : candidates) {
    fields.insert(candidate.pointer->getCanonicalDecl());
    fields.insert(candidate.count->getCanonicalDecl());
  }
  if (fields.empty())
    return;

  const clang::SourceManager &sm = context.getSourceManager();
  for (const clang::FunctionDecl *definition : definitions) {
    if (sm.isInSystemHeader(definition->getLocation()))
      continue;
    // The designators of relation stores (the lvalue and its bases), which
    // compute where the store goes and access nothing.
    llvm::DenseSet<const clang::Stmt *> designators;
    std::vector<const clang::Stmt *> work{definition->getBody()};
    while (!work.empty()) {
      const clang::Stmt *stmt = work.back();
      work.pop_back();
      if (stmt == nullptr)
        continue;
      if (const clang::MemberExpr *member = relationStore(stmt, fields)) {
        for (const clang::Expr *each = member; each != nullptr;) {
          designators.insert(each);
          const auto *inner = llvm::dyn_cast<clang::MemberExpr>(each);
          each = inner != nullptr ? inner->getBase()->IgnoreParenImpCasts()
                                  : nullptr;
        }
      }
      for (const clang::Stmt *child : stmt->children())
        work.push_back(child);
    }
    if (designators.empty())
      continue;
    const FunctionCfg *cfg = cfgOf(*definition);
    if (cfg == nullptr)
      continue;
    std::vector<StoreGroup> found;
    for (const clang::CFGBlock *block : *cfg->cfg) {
      // Open groups by object and record. Each closes at a call, an access
      // through its object, a store to a designator it depends on, or the
      // end of the block.
      std::map<std::pair<std::string, const clang::RecordDecl *>, std::size_t>
          open;
      const auto closeWhere = [&](const auto &predicate) {
        for (auto it = open.begin(); it != open.end();)
          it = predicate(it->first.first) ? open.erase(it) : std::next(it);
      };
      for (const clang::CFGElement &element : *block) {
        const auto each = element.getAs<clang::CFGStmt>();
        if (!each)
          continue;
        const clang::Stmt *stmt = each->getStmt();
        if (const clang::MemberExpr *member = relationStore(stmt, fields)) {
          const auto *field =
              llvm::cast<clang::FieldDecl>(member->getMemberDecl());
          const auto object = objectOf(member->getBase(), member->isArrow());
          const auto *store = llvm::cast<clang::Expr>(stmt);
          const auto key =
              std::pair{object.value_or(std::string()), field->getParent()};
          if (object && open.contains(key)) {
            StoreGroup &group = found[open[key]];
            group.stores.push_back(store);
            if (!llvm::is_contained(group.fields, field))
              group.fields.push_back(field);
          } else {
            found.push_back(StoreGroup{.function = definition,
                                       .record = field->getParent(),
                                       .object = member->getBase(),
                                       .stores = {store},
                                       .fields = {field}});
            if (object)
              open[key] = found.size() - 1;
          }
          continue;
        }
        if (llvm::isa<clang::CallExpr>(stmt)) {
          open.clear();
          continue;
        }
        // A store to a designator changes the objects behind it.
        const clang::Expr *assigned = nullptr;
        if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(stmt);
            binary != nullptr && binary->isAssignmentOp())
          assigned = binary->getLHS();
        else if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt);
                 unary != nullptr && unary->isIncrementDecrementOp())
          assigned = unary->getSubExpr();
        if (assigned != nullptr) {
          if (const auto chain = chainOf(assigned))
            closeWhere([&](const std::string &object) {
              return llvm::StringRef(object).starts_with(*chain + "->") ||
                     llvm::StringRef(object).starts_with(*chain + ".");
            });
        }
        if (designators.contains(stmt))
          continue;
        if (const auto object = accessedObject(stmt))
          closeWhere([&](const std::string &candidate) {
            return candidate == *object;
          });
      }
    }
    std::ranges::stable_sort(
        found, [&sm](const StoreGroup &a, const StoreGroup &b) {
          const clang::SourceLocation left = a.stores.front()->getBeginLoc();
          const clang::SourceLocation right = b.stores.front()->getBeginLoc();
          return left != right && sm.isBeforeInTranslationUnit(left, right);
        });
    for (StoreGroup &group : found)
      groups.push_back(std::move(group));
  }
}

} // namespace weavec::analysis
