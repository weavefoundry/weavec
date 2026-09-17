//===- DataflowBufferDiscovery.cpp - State roles (RFC 0029) --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "AffineSupport.h"
#include "Dataflow.h"
#include "IntegerSupport.h"

#include "clang/AST/RecordLayout.h"
#include "clang/Basic/Version.h"

#include <algorithm>
#include <ranges>

using namespace clang;
namespace weavec::analysis {
std::optional<core::BufferShape>
FunctionDataflow::discoverBufferShape(const RecordDecl &record) {
  return summaries.bufferShape(
      record, [&]() -> std::optional<core::BufferShape> {
        if (!record.isCompleteDefinition() || record.isUnion())
          return std::nullopt;
        std::vector<const FieldDecl *> pointers;
        std::vector<const FieldDecl *> counts;
        for (const auto *field : record.fields()) {
          const auto type = field->getType();
          if (field->isBitField() || field->getName().empty() ||
              type.isVolatileQualified() || type->isAtomicType())
            return std::nullopt;
          if (type->isPointerType() && !type->isFunctionPointerType()) {
            const auto element = type->getPointeeType();
            if (element->isScalarType() && !element.isVolatileQualified() &&
                !element->isAtomicType() &&
                (!element.isConstQualified() || element->isCharType()))
              pointers.push_back(field);
          } else if (type->isUnsignedIntegerType() && !type->isBooleanType()) {
            counts.push_back(field);
          }
        }
        if (pointers.empty() || pointers.size() > 16 || counts.size() < 2 ||
            counts.size() > 16)
          return std::nullopt;
        std::vector<const Stmt *> usage;
        for (const auto *decl : context.getTranslationUnitDecl()->decls())
          if (const auto *definition = dyn_cast<FunctionDecl>(decl);
              definition && definition->doesThisDeclarationHaveABody())
            usage.push_back(definition->getBody());
        for (std::size_t i = 0; i < usage.size(); ++i) {
          if (usage.size() > 65536)
            return std::nullopt;
          if (usage[i])
            for (const auto *child : usage[i]->children()) {
              if (usage.size() == 65536)
                return std::nullopt;
              usage.push_back(child);
            }
        }
        const FieldDecl *data = pointers.front();
        if (pointers.size() > 1) {
          std::map<const FieldDecl *, unsigned> uses;
          for (const auto *statement : usage) {
            const Expr *base = nullptr;
            if (const auto *index =
                    dyn_cast_or_null<ArraySubscriptExpr>(statement))
              base = index->getBase();
            if (const auto *operation =
                    dyn_cast_or_null<BinaryOperator>(statement);
                operation && operation->getOpcode() == BO_Add &&
                operation->getLHS()->getType()->isPointerType())
              base = operation->getLHS();
            const auto *member =
                base ? dyn_cast<MemberExpr>(base->IgnoreParenImpCasts())
                     : nullptr;
            if (member)
              for (const auto *pointer : pointers)
                if (member->getMemberDecl() == pointer)
                  ++uses[pointer];
          }
          unsigned score = 0;
          data = nullptr;
          for (const auto &[pointer, count] : uses)
            if (count > score) {
              data = pointer;
              score = count;
            } else if (count == score) {
              data = nullptr;
            }
          if (!data)
            return std::nullopt;
        }
        const auto element = data->getType()->getPointeeType();
        const auto unit = byteSizeOf(element, context);
        if (!unit || *unit <= 0 || element.isVolatileQualified() ||
            element->isAtomicType() ||
            (element.isConstQualified() && !element->isCharType()) ||
            !element->isScalarType() || element->isFunctionType())
          return std::nullopt;
#if CLANG_VERSION_MAJOR >= 23
        const auto type = context.getCanonicalTagType(&record);
#else
    const auto type = context.getRecordType(&record);
#endif
        const auto objectType =
            core::ObjectType::parse(checkedObjectType(type));
        if (!objectType)
          return std::nullopt;
        const auto field = [&](const FieldDecl &decl) {
          return core::ContainerField{
              .name = decl.getNameAsString(),
              .offset = context.getASTRecordLayout(&record).getFieldOffset(
                            decl.getFieldIndex()) /
                        context.getCharWidth(),
              .bytes = static_cast<std::uint64_t>(
                  context.getTypeSizeInChars(decl.getType()).getQuantity())};
        };
        // Look for the count used to select cells of this record's data field.
        // Discovery is shared across the TU's definitions so constructors and
        // reserve helpers agree with indexing helpers. This nominates a
        // relation; callers still have to establish its physical and
        // initialized storage.
        std::vector<unsigned> indexed(counts.size());
        std::vector<unsigned> capacityUses(counts.size());
        std::set<std::pair<unsigned, unsigned>> compared;
        std::set<const ValueDecl *> localIndices;
        std::set<std::pair<const ValueDecl *, unsigned>> indexBounds;
        bool indexedData = false;
        const auto counter = [&](const Expr *expr) -> std::optional<unsigned> {
          const auto *member =
              expr ? dyn_cast<MemberExpr>(expr->IgnoreParenImpCasts())
                   : nullptr;
          if (!member)
            return std::nullopt;
          for (unsigned c = 0; c < counts.size(); ++c)
            if (member->getMemberDecl() == counts[c])
              return c;
          return std::nullopt;
        };
        const auto comparedCounter = [&](const Expr *expr) {
          if (const auto direct = counter(expr))
            return direct;
          const auto *sum =
              dyn_cast<BinaryOperator>(expr->IgnoreParenImpCasts());
          if (!sum ||
              (sum->getOpcode() != BO_Add && sum->getOpcode() != BO_Sub))
            return std::optional<unsigned>{};
          const auto left = counter(sum->getLHS());
          const auto right = counter(sum->getRHS());
          // An indexed cursor plus a local index or constant still nominates
          // the extent it is compared with. Two field counters are ambiguous.
          if (left.has_value() == right.has_value())
            return std::optional<unsigned>{};
          return left ? left : right;
        };
        const auto indexCounter = [&](const Expr *expr) {
          if (const auto *unary =
                  dyn_cast<UnaryOperator>(expr->IgnoreParenImpCasts()))
            expr = unary->getSubExpr();
          if (const auto c = counter(expr)) {
            ++indexed[*c];
            return;
          }
          if (const auto *index =
                  dyn_cast<DeclRefExpr>(expr->IgnoreParenImpCasts()))
            localIndices.insert(index->getDecl());
          if (const auto *operation =
                  dyn_cast<BinaryOperator>(expr->IgnoreParenImpCasts());
              operation && (operation->getOpcode() == BO_Add ||
                            operation->getOpcode() == BO_Sub)) {
            if (const auto c = counter(operation->getLHS()))
              ++indexed[*c];
            if (const auto c = counter(operation->getRHS()))
              ++indexed[*c];
          }
        };
        for (std::size_t i = 0; i < usage.size() && usage.size() <= 65536;
             ++i) {
          if (!usage[i])
            continue;
          if (const auto *index = dyn_cast<ArraySubscriptExpr>(usage[i])) {
            const auto *base =
                dyn_cast<MemberExpr>(index->getBase()->IgnoreParenImpCasts());
            if (base && base->getMemberDecl() == data) {
              indexedData = true;
              indexCounter(index->getIdx());
            }
          }
          if (const auto *operation = dyn_cast<BinaryOperator>(usage[i])) {
            const auto *base = dyn_cast<MemberExpr>(
                operation->getLHS()->IgnoreParenImpCasts());
            if (operation->getOpcode() == BO_Add && base &&
                base->getMemberDecl() == data) {
              indexedData = true;
              indexCounter(operation->getRHS());
            }
            if (operation->isComparisonOp()) {
              const auto left = comparedCounter(operation->getLHS());
              const auto right = comparedCounter(operation->getRHS());
              if (left || right) {
                const auto *bound = dyn_cast<DeclRefExpr>(
                    (left ? operation->getRHS() : operation->getLHS())
                        ->IgnoreParenImpCasts());
                if (bound)
                  indexBounds.emplace(bound->getDecl(),
                                      left.value_or(right.value_or(0)));
              }
              if (left && right && *left != *right) {
                compared.emplace(*left, *right);
                compared.emplace(*right, *left);
              } else if (left || right) {
                ++capacityUses[left.value_or(right.value_or(0))];
              }
            }
          }
        }
        if (usage.size() > 65536)
          return std::nullopt;
        for (const auto &[index, count] : indexBounds)
          if (localIndices.contains(index))
            ++indexed[count];
        // Discovery only proposes a sufficient predicate. Its relation still
        // has to hold at every caller and after every mutation. An unrelated
        // depth/generation field is not a capacity merely because it follows
        // the logical length in the declaration.
        const auto uniqueMaximum =
            [](const std::vector<unsigned> &scores) -> std::optional<unsigned> {
          const auto best = std::ranges::max_element(scores);
          if (best == scores.end() || *best == 0 ||
              std::ranges::count(scores, *best) != 1)
            return std::nullopt;
          return static_cast<unsigned>(best - scores.begin());
        };
        auto length = uniqueMaximum(indexed);
        std::optional<unsigned> capacity;
        if (length) {
          std::vector<unsigned> candidates(counts.size());
          for (unsigned c = 0; c < counts.size(); ++c)
            if (c != *length && compared.contains({*length, c}))
              candidates[c] = 1 + capacityUses[c];
          capacity = uniqueMaximum(candidates);
          if (!capacity && counts.size() == 2 && !element.isConstQualified())
            capacity = 1 - *length;
        } else if (counts.size() == 2 && indexedData &&
                   !element.isConstQualified()) {
          // With a backing access but no distinguishing counter operation,
          // retain one deterministic two-counter candidate. A scalar payload
          // beside unrelated counters does not nominate a buffer at all.
          // The caller must still establish the candidate's actual bounds
          // and initialized prefix.
          length = 0;
          capacity = 1;
        }
        if (!length || !capacity)
          return std::nullopt;
        core::BufferShape shape{.object = *objectType,
                                .data = field(*data),
                                .length = field(*counts[*length]),
                                .capacity = field(*counts[*capacity]),
                                .elementBytes =
                                    static_cast<std::uint64_t>(*unit),
                                .pointerElements = element->isPointerType(),
                                .reader = element.isConstQualified()};
        return shape.valid() ? std::optional(shape) : std::nullopt;
      });
}

} // namespace weavec::analysis
