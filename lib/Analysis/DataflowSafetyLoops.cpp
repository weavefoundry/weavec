//===- DataflowSafetyLoops.cpp - Sufficient loop bounds (RFC 0018)
//----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"
#include "IntegerSupport.h"

#include "clang/AST/ParentMapContext.h"

using namespace clang;

namespace weavec::analysis {

// Eligibility is a bounded syntax check, not induction over the CFG. Exhausting
// this budget fails projection through noteExtentRequirement's existing path.
static constexpr std::size_t MaxLoopRequirementStatements = 4096;
static constexpr unsigned MaxLoopRequirementConditionNodes = 64;

static bool
collectLoopRequirementStatements(const Stmt *root,
                                 std::vector<const Stmt *> &statements) {
  if (root)
    statements.push_back(root);
  for (std::size_t i = 0; i < statements.size(); ++i) {
    for (const auto *child : statements[i]->children()) {
      if (!child)
        continue;
      if (statements.size() == MaxLoopRequirementStatements)
        return false;
      statements.push_back(child);
    }
  }
  return true;
}

static const VarDecl *loopRequirementVariable(const Expr *expr) {
  const auto *ref =
      expr ? dyn_cast<DeclRefExpr>(expr->IgnoreParenImpCasts()) : nullptr;
  const auto *var = ref ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
  return var ? var->getCanonicalDecl() : nullptr;
}

static const VarDecl *loopRequirementIndex(const ForStmt &loop,
                                           const Expr *&initial) {
  if (const auto *decl = dyn_cast_or_null<DeclStmt>(loop.getInit());
      decl && decl->isSingleDecl()) {
    const auto *var = dyn_cast<VarDecl>(decl->getSingleDecl());
    initial = var ? var->getInit() : nullptr;
    return var ? var->getCanonicalDecl() : nullptr;
  }
  const auto *expr = dyn_cast_or_null<Expr>(loop.getInit());
  const auto *assignment =
      expr ? dyn_cast<BinaryOperator>(expr->IgnoreParens()) : nullptr;
  if (!assignment || assignment->getOpcode() != BO_Assign)
    return nullptr;
  initial = assignment->getRHS();
  return loopRequirementVariable(assignment->getLHS());
}

void FunctionDataflow::checkedReaderLoop(const Stmt &at,
                                         core::AnalysisState &state) {
  if (bufferObjects.empty())
    return;
  const auto found = checkedLoops.find(&at);
  if (found == checkedLoops.end())
    return;
  const auto &loop = *found->second;
  auto [saved, inserted] = checkedReaderLoops.try_emplace(&loop);
  if (inserted) {
    const Expr *initial = nullptr;
    const auto *index = loopRequirementIndex(loop, initial);
    const auto type = index ? integerTypeOf(*index, context) : std::nullopt;
    const auto *increment =
        loop.getInc()
            ? dyn_cast<UnaryOperator>(loop.getInc()->IgnoreParenImpCasts())
            : nullptr;
    if (!index || !type || type->isSigned || type->isBoolean ||
        !index->hasLocalStorage() || addressTaken.contains(index) ||
        index->getType().isVolatileQualified() ||
        index->getType()->isAtomicType() || !initial ||
        integerConstant(*initial, context) != 0 ||
        initial->HasSideEffects(context) || !increment ||
        !increment->isIncrementOp() ||
        loopRequirementVariable(increment->getSubExpr()) != index ||
        !loop.getCond() || loop.getCond()->HasSideEffects(context))
      return;
    // A surrounding switch could jump directly into a case in the loop.
    const Stmt *ancestor = &loop;
    bool reachedBody = false;
    for (unsigned depth = 0; depth < 128; ++depth) {
      if (ancestor == function.getBody()) {
        reachedBody = true;
        break;
      }
      const auto parents = context.getParents(*ancestor);
      if (parents.size() != 1)
        return;
      ancestor = parents[0].get<Stmt>();
      if (!ancestor || isa<SwitchStmt>(ancestor))
        return;
    }
    if (!reachedBody)
      return;
    std::vector<const Stmt *> body;
    if (!collectLoopRequirementStatements(loop.getBody(), body))
      return;
    for (const auto *statement : body) {
      if (isa<CallExpr, AsmStmt, LabelStmt, IndirectGotoStmt, ForStmt,
              WhileStmt, DoStmt>(statement))
        return;
      const Expr *written = nullptr;
      if (const auto *assignment = dyn_cast<BinaryOperator>(statement);
          assignment && assignment->isAssignmentOp())
        written = assignment->getLHS();
      if (const auto *unary = dyn_cast<UnaryOperator>(statement);
          unary && unary->isIncrementDecrementOp())
        written = unary->getSubExpr();
      if (written) {
        const auto *variable = loopRequirementVariable(written);
        if (!variable || variable == index || !variable->hasLocalStorage() ||
            !variable->getType()->isArithmeticType())
          return;
      }
    }
    std::vector<const Expr *> conditions{loop.getCond()};
    for (std::size_t i = 0; i < conditions.size(); ++i) {
      if (conditions.size() > MaxLoopRequirementConditionNodes)
        return;
      const auto *comparison =
          dyn_cast<BinaryOperator>(conditions[i]->IgnoreParenImpCasts());
      if (!comparison)
        continue;
      if (comparison->getOpcode() == BO_LAnd) {
        conditions.push_back(comparison->getLHS());
        conditions.push_back(comparison->getRHS());
        continue;
      }
      if (comparison->getOpcode() != BO_LT)
        continue;
      const auto *sum =
          dyn_cast<BinaryOperator>(comparison->getLHS()->IgnoreParenImpCasts());
      if (!sum || sum->getOpcode() != BO_Add ||
          integerTypeOf(sum->getType(), context) != type ||
          integerTypeOf(comparison->getRHS()->getType(), context) != type)
        continue;
      const Expr *indexed = sum->getLHS();
      const Expr *position = sum->getRHS();
      if (loopRequirementVariable(indexed) != index)
        std::swap(indexed, position);
      if (loopRequirementVariable(indexed) != index ||
          integerTypeOf(indexed->getType(), context) != type ||
          integerTypeOf(position->getType(), context) != type ||
          !isa<MemberExpr>(position->IgnoreParenImpCasts()) ||
          !isa<MemberExpr>(comparison->getRHS()->IgnoreParenImpCasts()))
        continue;
      const auto cursor = builder.resolve(*position);
      const auto capacity = builder.resolve(*comparison->getRHS());
      if (!cursor || !capacity)
        continue;
      const auto *cursorDecl =
          dyn_cast_or_null<ValueDecl>(builder.declFor(cursor->place));
      const auto *capacityDecl =
          dyn_cast_or_null<ValueDecl>(builder.declFor(capacity->place));
      if (!cursorDecl || !capacityDecl ||
          integerTypeOf(*cursorDecl, context) != type ||
          integerTypeOf(*capacityDecl, context) != type)
        continue;
      for (const auto &[object, shape] : bufferObjects)
        if (shape.reader &&
            places.field(object, shape.length.name) == cursor->place &&
            places.field(object, shape.capacity.name) == capacity->place) {
          saved->second = CheckedReaderLoop{
              .index = indexed,
              .position = position,
              .capacity = comparison->getRHS(),
              .backing = places.field(object, shape.data.name)};
          const auto *compound = dyn_cast<CompoundStmt>(loop.getBody());
          const auto *selection =
              compound && compound->size() == 1
                  ? dyn_cast<SwitchStmt>(*compound->body_begin())
                  : dyn_cast<SwitchStmt>(loop.getBody());
          const auto *byte =
              selection ? dyn_cast<ArraySubscriptExpr>(
                              selection->getCond()->IgnoreParenImpCasts())
                        : nullptr;
          bool numeric = byte != nullptr &&
                         byteSizeOf(byte->getType(), context) == 1 &&
                         !byte->getType().isVolatileQualified() &&
                         !byte->getType()->isAtomicType() &&
                         loopRequirementVariable(byte->getIdx()) == index;
          bool hasDefault = false;
          unsigned labels = 0;
          for (const auto *label = selection ? selection->getSwitchCaseList()
                                             : nullptr;
               label && numeric; label = label->getNextSwitchCase()) {
            if (++labels > 256) {
              numeric = false;
              break;
            }
            if (const auto *branch = dyn_cast<CaseStmt>(label)) {
              numeric &=
                  branch->getRHS() == nullptr &&
                  integerConstant(*branch->getLHS(), context).has_value() &&
                  checkedNumericByte(*branch->getLHS(), state);
            } else {
              const auto *otherwise = cast<DefaultStmt>(label);
              const auto *jump = dyn_cast<GotoStmt>(otherwise->getSubStmt());
              hasDefault = true;
              numeric &= jump != nullptr &&
                         std::ranges::find(body, jump->getLabel()->getStmt()) ==
                             body.end();
            }
          }
          if (numeric && hasDefault && labels > 1)
            saved->second->numericInput = byte->getBase();
          break;
        }
    }
  }
  if (!saved->second)
    return;
  const auto &candidate = *saved->second;
  const auto *buffer = bufferFact(candidate.backing, state);
  if (!buffer || !buffer->shape.reader || !buffer->initialized)
    return;
  const auto index = integerExpressionOf(*candidate.index, state);
  const auto cursor = integerExpressionOf(*candidate.position, state);
  const auto capacity = integerExpressionOf(*candidate.capacity, state);
  if (!index || !cursor || !capacity ||
      !checkedAtMost(core::Affine::ofPlace(buffer->length),
                     core::Affine::ofPlace(buffer->capacity), state))
    return;
  const auto room = NumericExpression::operation(core::IntegerOp::Subtract,
                                                 *capacity, *cursor);
  if (!room)
    return;
  // The initial index is zero; stable counters and the strict successful
  // test leave room for the next unit step. This is an arithmetic induction
  // premise, independently of the storage/initialization obligations.
  state.numericConditions.requireInteger(
      {.lhs = *index, .op = core::IntegerOp::LessEqual, .rhs = *room});
  const bool strict = &at != &loop && !checkedLoopConditions.contains(&at);
  if (strict)
    state.numericConditions.requireInteger(
        {.lhs = *index, .op = core::IntegerOp::Less, .rhs = *room});
  const auto local = builder.resolve(*candidate.index);
  if (local) {
    const auto type = index->type();
    auto range = core::IntegerRange::full(type);
    const auto available = evaluateNumericExpression(*room, state);
    if (!available.mayBeInvalid)
      range = range.satisfying(strict ? core::IntegerOp::Less
                                      : core::IntegerOp::LessEqual,
                               available.values);
    state.scalars.set(
        local->place,
        core::ValueFact::ofInteger(
            integerRangeAt(local->place, type, state).intersect(range)));
    if (candidate.numericInput && &at != &loop) {
      const auto memory =
          checkedMemory(*candidate.numericInput, {},
                        core::Affine::ofPlace(local->place), state);
      if (memory && checkedAtMost(memory->begin, memory->end, state))
        state.safety->initialize(
            memory->storage,
            {.begin = memory->begin, .end = memory->end, .numericText = true});
    }
  }
}

// Only scalar locals and parameters can be stable without tracking heap writes.
// Explicit minimum expressions are supported; arithmetic bounds are left to a
// later extension rather than assuming that their evaluation cannot overflow.
static bool stableLoopRequirementBound(
    const Expr &expr, const VarDecl &index, ASTContext &context,
    const llvm::DenseSet<const VarDecl *> &addressTaken,
    std::set<const VarDecl *> &inputs, unsigned &remaining) {
  if (remaining == 0)
    return false;
  --remaining;
  if (integerConstant(expr, context))
    return !expr.HasSideEffects(context);
  const Expr *value = expr.IgnoreParenImpCasts();
  if (const auto *var = loopRequirementVariable(value)) {
    if (var == &index || !var->hasLocalStorage() ||
        var->getType().isVolatileQualified() ||
        var->getType()->isAtomicType() || !var->getType()->isIntegerType() ||
        addressTaken.contains(var))
      return false;
    inputs.insert(var);
    return true;
  }
  if (const auto *conditional = dyn_cast<ConditionalOperator>(value))
    return stableLoopRequirementBound(*conditional->getCond(), index, context,
                                      addressTaken, inputs, remaining) &&
           stableLoopRequirementBound(*conditional->getTrueExpr(), index,
                                      context, addressTaken, inputs,
                                      remaining) &&
           stableLoopRequirementBound(*conditional->getFalseExpr(), index,
                                      context, addressTaken, inputs, remaining);
  const auto *comparison = dyn_cast<BinaryOperator>(value);
  return comparison != nullptr && comparison->isComparisonOp() &&
         stableLoopRequirementBound(*comparison->getLHS(), index, context,
                                    addressTaken, inputs, remaining) &&
         stableLoopRequirementBound(*comparison->getRHS(), index, context,
                                    addressTaken, inputs, remaining);
}

static bool canonicalLoopRequirementCondition(
    const Expr &expr, const VarDecl &index, core::IntegerType indexType,
    ASTContext &context, const llvm::DenseSet<const VarDecl *> &addressTaken,
    std::set<const VarDecl *> &inputs, bool &incrementFits,
    unsigned &remaining) {
  if (remaining == 0)
    return false;
  --remaining;
  const auto *condition = dyn_cast<BinaryOperator>(expr.IgnoreParens());
  if (!condition)
    return false;
  if (condition->getOpcode() == BO_LAnd)
    return canonicalLoopRequirementCondition(
               *condition->getLHS(), index, indexType, context, addressTaken,
               inputs, incrementFits, remaining) &&
           canonicalLoopRequirementCondition(*condition->getRHS(), index,
                                             indexType, context, addressTaken,
                                             inputs, incrementFits, remaining);
  const auto op = condition->getOpcode();
  if ((op != BO_LT && op != BO_LE) ||
      loopRequirementVariable(condition->getLHS()) != &index ||
      !stableLoopRequirementBound(*condition->getRHS(), index, context,
                                  addressTaken, inputs, remaining))
    return false;

  const auto maximum = *core::IntegerRange::full(indexType).maximum();
  const auto comparisonType =
      integerTypeOf(condition->getLHS()->getType(), context);
  const auto nonnegative = core::IntegerRange::between(
      core::IntegerValue::ofBits(indexType, 0), maximum);
  if (!comparisonType || !conversionPreserves(nonnegative, *comparisonType))
    return false;

  // At least one conjunct must stop before ++ can wrap or overflow. In
  // particular, `unsigned char i; i < unsigned_n` alone is insufficient.
  const auto boundType = integerTypeOf(condition->getRHS()->getType(), context);
  if (!boundType)
    return false;
  auto upper = core::IntegerRange::full(*boundType).maximum()->bits;
  if (const auto constant = integerConstant(*condition->getRHS(), context)) {
    if (*constant < 0)
      return false;
    upper = static_cast<std::uint64_t>(*constant);
  }
  incrementFits |= op == BO_LT ? upper <= maximum.bits : upper < maximum.bits;
  return true;
}

std::optional<core::PathAffine> FunctionDataflow::checkedLoopRequirement(
    const core::Affine &need, const Stmt &at, core::AnalysisState &state) {
  (void)state;
  const auto found = checkedLoops.find(&at);
  if (found == checkedLoops.end() || !need.place || need.scale <= 0)
    return std::nullopt;
  const auto *index = builder.varForPlace(*need.place);
  const auto &loop = *found->second;
  const Expr *initial = nullptr;
  if (!index || loopRequirementIndex(loop, initial) != index || !initial ||
      initial->HasSideEffects(context) ||
      integerConstant(*initial, context) != 0 || addressTaken.contains(index) ||
      !loop.getCond() || !loop.getInc())
    return std::nullopt;
  const auto *inc = dyn_cast<UnaryOperator>(loop.getInc()->IgnoreParens());
  const auto type = integerTypeOf(*index, context);
  if (!inc || !inc->isIncrementOp() ||
      loopRequirementVariable(inc->getSubExpr()) != index || !type)
    return std::nullopt;
  bool fits = false;
  unsigned remaining = MaxLoopRequirementConditionNodes;
  std::set<const VarDecl *> inputs{index};
  if (!canonicalLoopRequirementCondition(*loop.getCond(), *index, *type,
                                         context, addressTaken, inputs, fits,
                                         remaining) ||
      !fits)
    return std::nullopt;
  std::vector<const Stmt *> statements;
  if (!collectLoopRequirementStatements(loop.getBody(), statements))
    return std::nullopt;
  for (const auto *stmt : statements) {
    if (isa<GotoStmt, IndirectGotoStmt, AsmStmt, CallExpr, ForStmt, WhileStmt,
            DoStmt>(stmt))
      return std::nullopt;
    const Expr *written = nullptr;
    if (const auto *b = dyn_cast<BinaryOperator>(stmt);
        b && b->isAssignmentOp())
      written = b->getLHS();
    if (const auto *u = dyn_cast<UnaryOperator>(stmt);
        u && u->isIncrementDecrementOp())
      written = u->getSubExpr();
    if (written) {
      const auto *var = loopRequirementVariable(written);
      if ((var && (inputs.contains(var) || var->getType()->isPointerType())) ||
          (!var && written->getType()->isPointerType()))
        return std::nullopt;
    }
  }
  const auto *condition =
      dyn_cast<BinaryOperator>(loop.getCond()->IgnoreParens());
  if (!condition || condition->getOpcode() != BO_LT)
    return std::nullopt;
  const auto bound = builder.affineOf(*condition->getRHS());
  if (!bound)
    return std::nullopt;
  const auto scaled = bound->times(need.scale);
  std::int64_t shift = 0;
  if (__builtin_sub_overflow(need.constant, need.scale, &shift))
    return std::nullopt;
  const auto end = scaled ? scaled->shifted(shift) : std::nullopt;
  return summaryAffineOf(end);
}

// RFC 0019: only the normal exit of an unconditional, unit-stride store
// establishes a prefix. The sufficient-requirement recognizer alone is not a
// must-write proof: break/continue and conditional stores are rejected here.
void FunctionDataflow::checkedLoopExit(const ForStmt &loop,
                                       core::AnalysisState &state) {
  const Expr *initial = nullptr;
  const auto *index = loopRequirementIndex(loop, initial);
  const auto *condition =
      loop.getCond() ? dyn_cast<BinaryOperator>(loop.getCond()->IgnoreParens())
                     : nullptr;
  if (index && condition && condition->getOpcode() == BO_GT &&
      loopRequirementVariable(condition->getLHS()) == index &&
      integerConstant(*condition->getRHS(), context) == 0) {
    const auto type = integerTypeOf(*index, context);
    const auto *decrement =
        loop.getInc()
            ? dyn_cast<UnaryOperator>(loop.getInc()->IgnoreParenImpCasts())
            : nullptr;
    if (!type || type->isBoolean || !index->hasLocalStorage() ||
        addressTaken.contains(index) ||
        index->getType().isVolatileQualified() ||
        index->getType()->isAtomicType() || !initial ||
        initial->HasSideEffects(context) || !decrement ||
        !decrement->isDecrementOp() ||
        loopRequirementVariable(decrement->getSubExpr()) != index)
      return;
    std::set<const VarDecl *> inputs{index};
    std::vector<const Stmt *> initialNodes;
    if (!collectLoopRequirementStatements(initial, initialNodes))
      return;
    for (const auto *node : initialNodes)
      if (const auto *reference = dyn_cast<DeclRefExpr>(node)) {
        const auto *variable = dyn_cast<VarDecl>(reference->getDecl());
        if (!variable || !variable->hasLocalStorage() ||
            !variable->getType()->isIntegerType() ||
            variable->getType().isVolatileQualified() ||
            variable->getType()->isAtomicType() ||
            addressTaken.contains(variable->getCanonicalDecl()))
          return;
        inputs.insert(variable->getCanonicalDecl());
      }
    std::vector<const Stmt *> statements;
    if (!collectLoopRequirementStatements(loop.getBody(), statements))
      return;
    const ArraySubscriptExpr *written = nullptr;
    for (const auto *statement : statements) {
      if (isa<IfStmt, SwitchStmt, CaseStmt, DefaultStmt, BreakStmt,
              ContinueStmt, ReturnStmt, GotoStmt, LabelStmt, IndirectGotoStmt,
              ConditionalOperator, CallExpr, ForStmt, WhileStmt, DoStmt,
              AsmStmt>(statement))
        return;
      const Expr *target = nullptr;
      if (const auto *unary = dyn_cast<UnaryOperator>(statement);
          unary &&
          (unary->isIncrementDecrementOp() || unary->getOpcode() == UO_AddrOf))
        return;
      if (const auto *binary = dyn_cast<BinaryOperator>(statement)) {
        if (binary->isLogicalOp())
          return;
        if (binary->isAssignmentOp()) {
          target = binary->getLHS();
          if (const auto *subscript =
                  dyn_cast<ArraySubscriptExpr>(target->IgnoreParenImpCasts())) {
            if (written || binary->getOpcode() != BO_Assign ||
                loopRequirementVariable(subscript->getIdx()) != index ||
                !subscript->getType()->isCharType() ||
                subscript->getType().isVolatileQualified())
              return;
            written = subscript;
            target = nullptr;
          }
        }
      }
      if (target) {
        const auto *variable = loopRequirementVariable(target);
        if (!variable || !variable->hasLocalStorage() ||
            !variable->getType()->isIntegerType() ||
            inputs.contains(variable) ||
            variable->getType().isVolatileQualified() ||
            variable->getType()->isAtomicType() ||
            addressTaken.contains(variable))
          return;
      }
    }
    if (!written || written->getBase()->HasSideEffects(context))
      return;
    const auto *base = written->getBase()->IgnoreParenImpCasts();
    const auto *slot = dyn_cast<UnaryOperator>(base);
    const auto *baseVariable = loopRequirementVariable(base);
    if (slot && slot->getOpcode() == UO_Deref)
      baseVariable = loopRequirementVariable(slot->getSubExpr());
    else
      slot = nullptr;
    if (!baseVariable || !baseVariable->hasLocalStorage() ||
        (!baseVariable->getType()->isPointerType() &&
         !baseVariable->getType()->isArrayType()) ||
        baseVariable->getType().isVolatileQualified() ||
        baseVariable->getType()->isAtomicType() ||
        (baseVariable->getType()->isPointerType() &&
         addressTaken.contains(baseVariable)))
      return;
    const auto expression = integerExpressionOf(*initial, state);
    const auto first =
        expression ? linearIntegerExpression(*expression, state) : std::nullopt;
    const auto end = first ? first->shifted(1) : std::nullopt;
    if (!first || !end || !checkedAtMost({}, *first, state))
      return;
    const auto memory = checkedMemory(*written->getBase(),
                                      core::Affine::ofConstant(1), *end, state);
    if (!memory)
      return;
    if (slot) {
      const auto bytes = byteSizeOf(slot->getType(), context);
      const auto header =
          bytes ? checkedMemory(*slot->getSubExpr(), {},
                                core::Affine::ofConstant(*bytes), state)
                : std::nullopt;
      if (!header || !runtimeSeparate(*header, *memory, loop, state))
        return;
    }
    state.safety->initialize(memory->storage,
                             {.begin = memory->begin, .end = memory->end});
    return;
  }
  if (!index || !condition || condition->getOpcode() != BO_LT ||
      loopRequirementVariable(condition->getLHS()) != index)
    return;
  std::vector<const Stmt *> statements;
  if (!collectLoopRequirementStatements(loop.getBody(), statements))
    return;
  const BinaryOperator *store = nullptr;
  for (const auto *stmt : statements) {
    if (isa<IfStmt, SwitchStmt, BreakStmt, ContinueStmt, ReturnStmt, GotoStmt,
            IndirectGotoStmt, ConditionalOperator, CallExpr, ForStmt, WhileStmt,
            DoStmt, AsmStmt>(stmt))
      return;
    if (const auto *unary = dyn_cast<UnaryOperator>(stmt);
        unary && unary->isIncrementDecrementOp())
      return;
    if (const auto *binary = dyn_cast<BinaryOperator>(stmt)) {
      if (binary->isLogicalOp())
        return;
      if (binary->isAssignmentOp()) {
        if (store || binary->getOpcode() != BO_Assign)
          return;
        store = binary;
      }
    }
  }
  const auto *subscript =
      store
          ? dyn_cast<ArraySubscriptExpr>(store->getLHS()->IgnoreParenImpCasts())
          : nullptr;
  if (!subscript || loopRequirementVariable(subscript->getIdx()) != index)
    return;
  const auto unit = byteSizeOf(subscript->getType(), context);
  // Keep the stable bound's identity across every visit, including the
  // zero-iteration exit. Flow simplification can otherwise substitute the
  // loop index, whose lifetime ends immediately after this edge.
  std::optional<core::Affine> bound;
  if (const auto constant = integerConstant(*condition->getRHS(), context))
    bound = core::Affine::ofConstant(*constant);
  else if (const auto *variable = loopRequirementVariable(condition->getRHS()))
    bound = core::Affine::ofPlace(builder.placeForVar(*variable));
  const auto end = unit && bound ? bound->times(*unit) : std::nullopt;
  if (!end)
    return;
  const auto next = core::Affine{
      .place = builder.placeForVar(*index), .scale = *unit, .constant = *unit};
  if (!checkedLoopRequirement(next, *subscript, state)) {
    // RFC 0026: an unconditional suffix fill extends a prefix established
    // before the loop. Keep the zero-iteration path: when n <= old length,
    // that same prefix already covers every byte below n.
    const auto base = builder.resolvePointerValue(*subscript->getBase());
    const auto start = initial ? builder.resolve(*initial) : std::nullopt;
    const auto *buffer = base ? bufferFact(base->place, state) : nullptr;
    const auto *increment =
        loop.getInc() ? dyn_cast<UnaryOperator>(loop.getInc()->IgnoreParens())
                      : nullptr;
    const auto type = integerTypeOf(*index, context);
    if (!buffer || !buffer->initialized || buffer->shape.pointerElements ||
        !start || start->place != buffer->length || !initial ||
        initial->HasSideEffects(context) || addressTaken.contains(index) ||
        !increment || !increment->isIncrementOp() ||
        loopRequirementVariable(increment->getSubExpr()) != index || !type)
      return;
    const auto first = integerRangeOf(*initial, state);
    bool fits = false;
    unsigned remaining = MaxLoopRequirementConditionNodes;
    std::set<const VarDecl *> inputs{index};
    if (!first || first->mayBeInvalid ||
        !conversionPreserves(first->values, *type) ||
        !canonicalLoopRequirementCondition(*condition, *index, *type, context,
                                           addressTaken, inputs, fits,
                                           remaining) ||
        !fits)
      return;
  }
  if (const auto memory =
          checkedMemory(*subscript->getBase(), {}, *end, state)) {
    state.safety->initialize(memory->storage,
                             {.begin = memory->begin, .end = memory->end});
  }
}

} // namespace weavec::analysis
