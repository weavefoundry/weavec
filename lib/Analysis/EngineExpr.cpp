//===- EngineExpr.cpp - Expressions in the object engine ------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0031 §5.1: rvalues evaluate to symbols, lvalues to addresses; loads and
// stores go through the address. Every subexpression is a CFG element, so an
// expression's operands are found in the block's memo (or, for the operands
// of `?:`, `&&` and `||`, in the state's expression values).
//
//===----------------------------------------------------------------------===//

#include "Engine.h"
#include "weavec/Analysis/ClangLocation.h"

#include "clang/AST/RecordLayout.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

using namespace clang;

namespace weavec::analysis::engine {

Transfer::Transfer(FunctionRun &functionRun, core::HeapState &heapState)
    : run(functionRun), heap(functionRun.domain()), state(heapState),
      context(functionRun.ast()) {}

//===----------------------------------------------------------------------===//
// Types and constants
//===----------------------------------------------------------------------===//

std::optional<std::int64_t> Transfer::sizeOf(QualType type) const {
  if (type.isNull() || type->isIncompleteType() || type->isFunctionType() ||
      type->isVoidType())
    return std::nullopt;
  if (type->isVariablyModifiedType())
    return std::nullopt;
  return static_cast<std::int64_t>(
      context.getTypeSizeInChars(type).getQuantity());
}

void Transfer::captureVlas(QualType type) {
  // Only the dimensions this declaration spells: a typedef's were taken
  // where the typedef ran.
  const Type *node = type.getTypePtrOrNull();
  while (node != nullptr) {
    if (const auto *paren = dyn_cast<ParenType>(node)) {
      node = paren->getInnerType().getTypePtrOrNull();
    } else if (const auto *pointer = dyn_cast<PointerType>(node)) {
      node = pointer->getPointeeType().getTypePtrOrNull();
    } else if (const auto *vla = dyn_cast<VariableArrayType>(node)) {
      if (const Expr *size = vla->getSizeExpr()) {
        core::Sym count = valueOf(*size);
        state.exprs.set(handleOf(vla), count);
        // RFC 0017: a dimension that is zero or negative for every value.
        if (auto hi = state.zone.upper(count);
            hi && *hi <= 0 && run.isPublishing()) {
          core::Diagnostic diagnostic;
          diagnostic.id = core::diag::InvalidIntegerOperation;
          diagnostic.severity = core::Severity::Error;
          diagnostic.message = "invalid integer operation: nonpositive "
                               "variable array dimension";
          diagnostic.location =
              toCoreLocation(context.getSourceManager(), size->getBeginLoc());
          run.report(std::move(diagnostic), core::Certainty::Definite, size,
                     std::nullopt);
        }
      }
      node = vla->getElementType().getTypePtrOrNull();
    } else if (const auto *array = dyn_cast<ArrayType>(node)) {
      node = array->getElementType().getTypePtrOrNull();
    } else if (const auto *attributed = dyn_cast<AttributedType>(node)) {
      node = attributed->getModifiedType().getTypePtrOrNull();
    } else {
      node = nullptr;
    }
  }
}

core::Sym Transfer::vlaCount(const VariableArrayType &vla) {
  if (const core::Sym *held = state.exprs.find(handleOf(&vla)))
    return *held;
  // Declared where the analysis did not see it run: any count.
  return unknownValue(context.getSizeType());
}

std::optional<core::Sym> Transfer::bytesOf(QualType type, const Expr &at) {
  if (type.isNull())
    return std::nullopt;
  if (!type->isVariablyModifiedType()) {
    auto size = sizeOf(type);
    if (!size)
      return std::nullopt;
    return constant(*size, context.getSizeType());
  }
  const ArrayType *array = context.getAsArrayType(type);
  if (array == nullptr)
    return std::nullopt;
  auto element = bytesOf(array->getElementType(), at);
  if (!element)
    return std::nullopt;
  core::Sym count = core::ZeroSym;
  if (const auto *vla = dyn_cast<VariableArrayType>(array)) {
    if (vla->getSizeExpr() == nullptr)
      return std::nullopt;
    count = vlaCount(*vla);
    // A dimension that may be zero or negative makes no storage C defines
    // (RFC 0017): nothing is known of the object's size.
    auto lo = state.zone.lower(count);
    if (!lo || *lo < 1)
      return std::nullopt;
    count = convertInteger(count, vla->getSizeExpr()->getType(),
                           context.getSizeType());
  } else if (const auto *fixed = dyn_cast<ConstantArrayType>(array)) {
    count = constant(static_cast<std::int64_t>(fixed->getZExtSize()),
                     context.getSizeType());
  } else {
    return std::nullopt;
  }
  core::Sym bytes =
      arithmetic(BO_Mul, count, *element, context.getSizeType(), at);
  // A product that may wrap is no size (RFC 0017): the object is as large
  // as the mathematical product, which `size_t` cannot hold.
  const core::SymInfo &info = heap.info(state, bytes);
  if (!info.defined || !info.defined->exact)
    return std::nullopt;
  return bytes;
}

/// The leaves below `base` of a value of `type` (RFC 0015 §4).
static bool leavesOf(const ASTContext &context, QualType type,
                     std::int64_t base,
                     std::vector<std::pair<std::int64_t, QualType>> &out,
                     int depth) {
  if (depth > 4 || out.size() > 32 || type.isNull() || type->isIncompleteType())
    return false;
  type = type.getCanonicalType();
  if (type->isPointerType() || type->isIntegralOrEnumerationType()) {
    out.emplace_back(base, type);
    return true;
  }
  if (type->isRealFloatingType())
    return true; // no pointer or integer facts to carry
  if (const auto *array = context.getAsConstantArrayType(type)) {
    QualType element = array->getElementType();
    if (element->isIncompleteType())
      return false;
    auto size = static_cast<std::int64_t>(
        context.getTypeSizeInChars(element).getQuantity());
    std::uint64_t count = array->getSize().getZExtValue();
    if (count > 16 || size <= 0)
      return false;
    for (std::uint64_t i = 0; i < count; ++i)
      if (!leavesOf(context, element,
                    base + (static_cast<std::int64_t>(i) * size), out,
                    depth + 1))
        return false;
    return true;
  }
  const RecordDecl *record = type->getAsRecordDecl();
  if (record == nullptr || !record->isCompleteDefinition() || record->isUnion())
    return false;
  const ASTRecordLayout &layout = context.getASTRecordLayout(record);
  for (const FieldDecl *field : record->fields()) {
    if (field->isBitField())
      return false;
    auto offset = static_cast<std::int64_t>(
        layout.getFieldOffset(field->getFieldIndex()) / context.getCharWidth());
    if (!leavesOf(context, field->getType(), base + offset, out, depth + 1))
      return false;
  }
  return true;
}

bool Transfer::recordLeaves(
    QualType type, std::vector<std::pair<std::int64_t, QualType>> &out) const {
  out.clear();
  return leavesOf(context, type, 0, out, 0);
}

bool Transfer::copyRecord(const Address &to, const Address &from,
                          QualType type) {
  std::vector<std::pair<std::int64_t, QualType>> leaves;
  if (to.top || from.top || to.targets.empty() || from.targets.empty() ||
      !recordLeaves(type, leaves))
    return false;
  std::vector<core::Sym> values;
  for (const auto &[offset, leafType] : leaves) {
    Address cell = from;
    for (core::Target &target : cell.targets)
      target.offset = target.offset.plusConstant(offset);
    values.push_back(load(cell, leafType, nullptr));
  }
  for (std::size_t i = 0; i < leaves.size(); ++i) {
    Address cell = to;
    for (core::Target &target : cell.targets)
      target.offset = target.offset.plusConstant(leaves[i].first);
    store(cell, values[i], leaves[i].second, nullptr);
  }
  return true;
}

std::optional<core::IntegerType> Transfer::integerType(QualType type) const {
  if (type.isNull())
    return std::nullopt;
  type = type.getCanonicalType();
  if (type->isPointerType())
    return core::IntegerType{.width = static_cast<unsigned>(
                                 context.getTypeSize(context.getUIntPtrType())),
                             .isSigned = false};
  if (!type->isIntegralOrEnumerationType())
    return std::nullopt;
  return core::IntegerType{.width =
                               static_cast<unsigned>(context.getTypeSize(type)),
                           .isSigned = type->isSignedIntegerOrEnumerationType(),
                           .isBoolean = type->isBooleanType()};
}

/// The range of a C integer type, when it fits in 64-bit bounds.
static std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>>
typeRange(const core::IntegerType &type) {
  if (type.isBoolean)
    return {0, 1};
  if (type.isSigned) {
    if (type.width >= 64)
      return {INT64_MIN, INT64_MAX};
    std::int64_t hi =
        static_cast<std::int64_t>(std::uint64_t{1} << (type.width - 1)) - 1;
    return {-hi - 1, hi};
  }
  if (type.width >= 63)
    return {0, std::nullopt};
  return {0, static_cast<std::int64_t>(std::uint64_t{1} << type.width) - 1};
}

core::Sym Transfer::constant(std::int64_t value, QualType type) {
  core::Sym sym = heap.constant(state, value, integerType(type));
  heap.infoMut(state, sym).ctype = typeHandle(type);
  if (!type.isNull() && type->isPointerType()) {
    core::SymInfo &info = heap.infoMut(state, sym);
    info.type = core::SymInfo::Type::Pointer;
    info.null = value == 0 ? core::PointerNull::Null : core::PointerNull::Maybe;
    if (value != 0) {
      core::ObjectId any = run.unknownObject();
      heap.ensure(state, any);
      info.targets = {core::Target{.object = any}};
      info.raw = true;
    }
  }
  return sym;
}

core::Sym Transfer::constant(const llvm::APSInt &value, QualType type) {
  if (value.isSigned() ? value.getSignificantBits() <= 64
                       : value.getActiveBits() <= 63)
    return constant(value.isSigned()
                        ? value.getSExtValue()
                        : static_cast<std::int64_t>(value.getZExtValue()),
                    type);
  core::Sym sym = unknownValue(type);
  auto integer = integerType(type);
  if (!integer || integer->isBoolean || integer->width > 64 ||
      value.getActiveBits() > 64)
    return sym;
  // (An unsigned 64-bit value above `INT64_MAX`: its interval, §4.4.)
  core::IntegerRange range = core::IntegerRange::singleton(
      core::IntegerValue::ofBits(*integer, value.getZExtValue()));
  if (!range.empty())
    heap.infoMut(state, sym).values = range;
  return sym;
}

core::Sym Transfer::nullPointer(QualType type) {
  core::SymInfo info;
  info.type = core::SymInfo::Type::Pointer;
  info.null = core::PointerNull::Null;
  info.ctype = typeHandle(type);
  return heap.fresh(state, info);
}

core::Sym Transfer::unknownValue(QualType type) {
  core::SymInfo info;
  info.ctype = typeHandle(type);
  if (!type.isNull() && type->isPointerType()) {
    if (type->getPointeeType()->isFunctionType()) {
      info.type = core::SymInfo::Type::Function;
      return heap.fresh(state, info);
    }
    info.type = core::SymInfo::Type::Pointer;
    core::ObjectId any = run.unknownObject();
    heap.ensure(state, any);
    info.targets = {core::Target{.object = any}};
    return heap.fresh(state, info);
  }
  if (auto integer = integerType(type)) {
    info.type = core::SymInfo::Type::Int;
    info.intType = integer;
    core::Sym sym = heap.fresh(state, info);
    auto [lo, hi] = typeRange(*integer);
    state.zone.addRange(sym, lo, hi);
    return sym;
  }
  return heap.fresh(state, info);
}

core::Sym Transfer::pointerTo(const Address &address, QualType pointee,
                              std::string name) {
  core::SymInfo info;
  info.type = core::SymInfo::Type::Pointer;
  info.targets = address.targets;
  info.top = address.top;
  info.null = core::PointerNull::NonNull;
  info.name = std::move(name);
  // RFC 0011: `&p->f` borrows the object `p` points to.
  info.derived = address.base != core::ZeroSym;
  info.ctype =
      pointee.isNull() ? 0 : typeHandle(context.getPointerType(pointee));
  if (address.top) {
    core::ObjectId any = run.unknownObject();
    heap.ensure(state, any);
    info.targets = {core::Target{.object = any}};
    info.top = false;
  }
  for (const core::Target &target : info.targets)
    heap.ensure(state, target.object);
  return heap.fresh(state, info);
}

core::Term Transfer::termOf(core::Sym sym) const {
  const core::SymInfo &info = heap.info(state, sym);
  if (auto c = state.zone.constant(sym))
    return core::Term::of(*c);
  if (info.linear)
    return *info.linear;
  if (info.type == core::SymInfo::Type::Int)
    return core::Term::ofSym(sym);
  return core::Term::unknown();
}

//===----------------------------------------------------------------------===//
// Spelling
//===----------------------------------------------------------------------===//

std::string Transfer::spell(const Expr &expr) const {
  const Expr *e = expr.IgnoreParenImpCasts();
  if (const auto *ref = dyn_cast<DeclRefExpr>(e))
    return ref->getDecl()->getNameAsString();
  if (const auto *member = dyn_cast<MemberExpr>(e)) {
    std::string base = spell(*member->getBase());
    return base + (member->isArrow() ? "->" : ".") +
           member->getMemberDecl()->getNameAsString();
  }
  if (const auto *unary = dyn_cast<UnaryOperator>(e)) {
    if (unary->getOpcode() == UO_Deref) {
      const Expr *sub = unary->getSubExpr()->IgnoreImpCasts();
      if (isa<ParenExpr>(sub))
        return "*(" + spell(*sub) + ")";
      return "*" + spell(*unary->getSubExpr());
    }
    if (unary->getOpcode() == UO_AddrOf)
      return "&" + spell(*unary->getSubExpr());
  }
  if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(e))
    return spell(*subscript->getBase()) + "[" + spell(*subscript->getIdx()) +
           "]";
  if (const auto *cast = dyn_cast<CastExpr>(e))
    return spell(*cast->getSubExpr());
  const SourceManager &sm = context.getSourceManager();
  CharSourceRange range = CharSourceRange::getTokenRange(e->getSourceRange());
  std::string text =
      Lexer::getSourceText(range, sm, context.getLangOpts()).str();
  std::erase_if(text, [](char c) { return c == '\n' || c == '\t'; });
  return text;
}

//===----------------------------------------------------------------------===//
// Elements
//===----------------------------------------------------------------------===//

void Transfer::element(const CFGElement &element) {
  if (auto stmt = element.getAs<CFGStmt>()) {
    const Stmt *s = stmt->getStmt();
    // A value dead where a statement starts is leaked (RFC 0007).
    if (statementStart && run.isPublishing())
      run.checkLeaks(state, *s, FunctionRun::LeakPoint::Statement);
    statementStart = run.isStatementLevel(*s);
    run.noteElement(*s);
    if (auto early = run.vlaCaptureBefore.find(s);
        early != run.vlaCaptureBefore.end())
      for (const VarDecl *var : early->second)
        captureVlas(var->getType());
    if (const auto *expr = dyn_cast<Expr>(s)) {
      evaluate(*expr);
    } else if (const auto *decl = dyn_cast<DeclStmt>(s)) {
      for (const Decl *d : decl->decls()) {
        if (const auto *var = dyn_cast<VarDecl>(d)) {
          if (!var->hasGlobalStorage() && !run.vlaCapturedEarly.contains(var))
            captureVlas(var->getType());
          declare(*var);
        } else if (const auto *alias = dyn_cast<TypedefNameDecl>(d)) {
          captureVlas(alias->getUnderlyingType());
        }
      }
    } else if (const auto *ret = dyn_cast<ReturnStmt>(s)) {
      returned(*ret);
      return;
    } else if (const auto *assembly = dyn_cast<GCCAsmStmt>(s)) {
      inlineAssembly(*assembly);
    }
    // (The site's facets are decided first: they are about the pointer
    // before the access.)
    if (!run.isPublishing())
      accessed(*s);
    if (run.isPublishing()) {
      decideSites(*s);
      accessed(*s);
      if (run.isStatementLevel(*s)) {
        // The statement uses the objects its values point to.
        for (const auto &[expr, result] : memo) {
          if (result.value == core::ZeroSym || isa<CallExpr>(expr))
            continue;
          const core::SymInfo *info = state.syms.find(result.value);
          if (info == nullptr)
            continue;
          for (const core::Target &target : info->targets)
            if (state.objects.contains(target.object) && !isa<DeclStmt>(s))
              state.objects.at(target.object).lastUse = handleOf(s);
        }
        std::string released;
        if (const auto *callExpr = dyn_cast<CallExpr>(s))
          for (core::SiteId id : run.sites().sitesOf(*callExpr))
            if (const SiteInfo *info = run.sites().info(id);
                info != nullptr && info->kind == core::SiteKind::Release &&
                info->operand != nullptr) {
              released = spell(*info->operand);
              declaredOwners(*info->operand, released);
            }
        if (!released.empty())
          run.checkLeaks(state, *s, FunctionRun::LeakPoint::Release, released);
      }
    }
    return;
  }
  if (auto cleanup = element.getAs<CFGCleanupFunction>()) {
    if (const VarDecl *var = cleanup->getVarDecl())
      cleanupFunction(*var);
    return;
  }
  if (auto lifetime = element.getAs<CFGLifetimeEnds>()) {
    if (const VarDecl *var = lifetime->getVarDecl())
      lifetimeEnds(*var);
    return;
  }
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): block hook
void Transfer::finishBlock(const CFGBlock &block) {
  (void)block;
}

void Transfer::declaredOwners(const Expr &operand,
                              const std::string &released) {
  const core::SymInfo value = heap.info(state, valueOf(operand));
  if (value.type != core::SymInfo::Type::Pointer || value.top)
    return;
  QualType pointee = operand.IgnoreParenImpCasts()->getType();
  if (!pointee->isPointerType())
    return;
  const RecordDecl *record = pointee->getPointeeType()->getAsRecordDecl();
  if (record == nullptr || !record->isCompleteDefinition())
    return;
  const ASTRecordLayout &layout = context.getASTRecordLayout(record);
  for (const core::Target &container : value.targets) {
    // RFC 0007 *Owned fields*: what an entry container's `WEAVEC_OWNED`
    // field held at entry is the container's to release with it.
    if (run.table().info(container.object).key.kind !=
            core::ObjectKind::Entry ||
        !container.offset.isConstant())
      continue;
    for (const FieldDecl *field : record->fields()) {
      if (!field->getType()->isPointerType() || !getAnnotations(*field).owned)
        continue;
      Address cell;
      cell.targets = {core::Target{
          .object = container.object,
          .offset = container.offset.plusConstant(static_cast<std::int64_t>(
              layout.getFieldOffset(field->getFieldIndex()) /
              context.getCharWidth()))}};
      const core::SymInfo held =
          heap.info(state, load(cell, field->getType(), nullptr));
      if (held.type != core::SymInfo::Type::Pointer ||
          held.null == core::PointerNull::Null || held.top)
        continue;
      for (const core::Target &target : held.targets) {
        if (run.table().info(target.object).key.kind !=
                core::ObjectKind::Entry ||
            !state.objects.contains(target.object))
          continue;
        core::ObjectState &object = state.objects.at(target.object);
        if (object.owned || object.escaped || object.life != core::Life::Live)
          continue;
        object.owned = true;
        object.holder = released + "->" + field->getNameAsString();
        run.declaredOwner[target.object] = std::make_pair(
            field->getNameAsString(),
            toCoreLocation(context.getSourceManager(), field->getLocation()));
      }
    }
  }
}

core::Sym Transfer::conditionValue(const Expr &condition) {
  return valueOf(condition);
}

ExprResult Transfer::evaluate(const Expr &expr) {
  if (auto it = memo.find(&expr); it != memo.end())
    return it->second;
  if (const core::Sym *carried = state.exprs.find(handleOf(&expr));
      carried != nullptr && !run.evaluatedHere(expr)) {
    ExprResult result{.value = *carried, .address = std::nullopt};
    memo[&expr] = result;
    return result;
  }
  ExprResult result = evaluateUncached(expr);
  // RFC 0017: an integer constant expression is its value, even where its
  // operands do not fit the zone's 64-bit bounds (`(size_t)-1 / 8 + 1`).
  if (result.value != core::ZeroSym && !result.address &&
      expr.getType()->isIntegerType() && !expr.getType()->isBooleanType() &&
      !state.zone.constant(result.value) &&
      (isa<BinaryOperator>(expr) || isa<UnaryOperator>(expr) ||
       isa<CastExpr>(expr)) &&
      !expr.HasSideEffects(context)) {
    Expr::EvalResult folded;
    if (expr.EvaluateAsInt(folded, context))
      result.value = constant(folded.Val.getInt(), expr.getType());
  }
  memo[&expr] = result;
  if (result.value != core::ZeroSym) {
    if (run.isCrossBlock(expr))
      state.exprs.set(handleOf(&expr), result.value);
    if (const Expr *parent = run.armOf(expr))
      state.exprs.set(handleOf(parent), result.value);
  }
  return result;
}

core::Sym Transfer::valueOf(const Expr &expr) {
  ExprResult result = evaluate(expr);
  if (result.value != core::ZeroSym)
    return result.value;
  if (result.address) {
    // An lvalue used as a value: a load (a record or array rvalue).
    QualType type = expr.getType();
    if (type->isArrayType())
      return pointerTo(*result.address,
                       context.getAsArrayType(type)->getElementType(),
                       spell(expr));
    core::Sym loaded = load(*result.address, type, &expr);
    memo[&expr].value = loaded;
    return loaded;
  }
  return unknownValue(expr.getType());
}

Address Transfer::addressOf(const Expr &expr) {
  ExprResult result = evaluate(expr);
  if (result.address)
    return *result.address;
  // An rvalue of pointer type used as an address (`*f()` folded, a record
  // returned by value): a temporary object.
  Address address;
  address.targets = {core::Target{.object = run.literalObject(expr)}};
  heap.ensure(state, address.targets[0].object);
  if (result.value != core::ZeroSym)
    heap.write(state, address.targets[0].object, core::CellKey{}, result.value,
               false);
  return address;
}

//===----------------------------------------------------------------------===//
// Memory
//===----------------------------------------------------------------------===//

/// The cell key of an offset: concrete for a constant, a selected element
/// cell for an affine offset (§4.2 *Amendment (arrays)*), none for an
/// unknown offset.
static std::optional<core::CellKey> cellKeyOf(const core::Term &offset) {
  return core::CellKey::at(offset);
}

bool Transfer::complementary(const Address &address) const {
  if (address.top || address.targets.size() != 2)
    return false;
  auto existence = [&](core::ObjectId id) -> std::optional<core::EntryTest> {
    const core::ObjectInfo &info = run.table().info(id);
    if (!info.singular)
      return std::nullopt;
    if (info.key.kind == core::ObjectKind::Entry && !info.key.dead &&
        info.heldIn)
      return core::EntryTest{.object = info.heldIn->first,
                             .key =
                                 core::CellKey{.offset = info.heldIn->second},
                             .zero = false};
    if (const core::ObjectState *object = heap.findObject(state, id))
      return object->existsIfEntry;
    return std::nullopt;
  };
  auto first = existence(address.targets[0].object);
  auto second = existence(address.targets[1].object);
  return first && second && first->object == second->object &&
         first->key == second->key && first->zero != second->zero;
}

core::Sym Transfer::load(const Address &address, QualType type,
                         const Expr *at) {
  (void)at;
  if (address.top || address.targets.empty() || type.isNull())
    return unknownValue(type);
  core::Sym result = core::ZeroSym;
  core::SymInfo hint;
  if (type->isPointerType())
    hint.type = type->getPointeeType()->isFunctionType()
                    ? core::SymInfo::Type::Function
                    : core::SymInfo::Type::Pointer;
  else
    hint.type = integerType(type) ? core::SymInfo::Type::Int
                                  : core::SymInfo::Type::Unknown;
  hint.ctype = typeHandle(type);
  for (const core::Target &target : address.targets) {
    heap.ensure(state, target.object);
    std::optional<core::CellKey> key = cellKeyOf(target.offset);
    core::Sym value = core::ZeroSym;
    if (!key) {
      // An unknown offset: any cell of the object.
      value = unknownValue(type);
      const core::ObjectState *object = heap.findObject(state, target.object);
      std::vector<core::Sym> cells;
      if (object != nullptr) {
        for (const auto &[cellKey, sym] : object->cells)
          if (heap.info(state, sym).type == hint.type)
            cells.push_back(sym);
        for (const core::Segment &segment : object->segments)
          if (heap.info(state, segment.value).type == hint.type)
            cells.push_back(segment.value);
      }
      for (core::Sym cell : cells)
        value = heap.mergeWeak(state, value, cell);
    } else {
      // A cell, or an element by the element facts (§4.2 *Amendment
      // (arrays)*).
      value = heap.load(state, target.object, *key, hint);
    }
    result =
        result == core::ZeroSym ? value : heap.mergeWeak(state, result, value);
  }
  // Complementary objects: the value an earlier load read from the same
  // two cells, so that what the path learnt of it still holds.
  if (complementary(address)) {
    auto first = cellKeyOf(address.targets[0].offset);
    auto second = cellKeyOf(address.targets[1].offset);
    auto firstValue = first
                          ? heap.read(state, address.targets[0].object, *first)
                          : std::nullopt;
    auto secondValue =
        second ? heap.read(state, address.targets[1].object, *second)
               : std::nullopt;
    if (firstValue && secondValue && first->isConcrete() &&
        second->isConcrete()) {
      core::MergedLoad entry{.first = address.targets[0].object,
                             .firstKey = *first,
                             .firstValue = *firstValue,
                             .second = address.targets[1].object,
                             .secondKey = *second,
                             .secondValue = *secondValue,
                             .merged = core::ZeroSym};
      bool found = false;
      for (const core::MergedLoad &known : state.mergedLoads)
        if (known.first == entry.first && known.firstKey == entry.firstKey &&
            known.firstValue == entry.firstValue &&
            known.second == entry.second &&
            known.secondKey == entry.secondKey &&
            known.secondValue == entry.secondValue &&
            state.syms.contains(known.merged)) {
          result = known.merged;
          found = true;
          break;
        }
      if (!found) {
        entry.merged = result;
        state.mergedLoads.push_back(entry);
      }
    }
  }
  // RFC 0004: a pointer loaded through a raw pointer is raw.
  if (address.base != core::ZeroSym && heap.info(state, address.base).raw &&
      heap.info(state, result).type == core::SymInfo::Type::Pointer &&
      !heap.info(state, result).raw) {
    core::SymInfo copy = heap.info(state, result);
    copy.raw = true;
    copy.rawAt = at != nullptr ? toCoreLocation(context.getSourceManager(),
                                                at->getBeginLoc())
                               : heap.info(state, address.base).rawAt;
    copy.rawOrigin = core::SymInfo::RawOrigin::Loaded;
    copy.rawFrom = heap.info(state, address.base).name;
    copy.rawSome = heap.info(state, address.base).rawSome;
    result = heap.fresh(state, copy);
  }
  // Reinterpretation (§4.2): a pointer read from a cell that holds an
  // integer, and the reverse.
  const core::SymInfo &loaded = heap.info(state, result);
  if (hint.type == core::SymInfo::Type::Pointer &&
      loaded.type == core::SymInfo::Type::Int) {
    // (Even the bits of a pointer, stored as an integer: RFC 0030 §2.3.)
    if (auto c = state.zone.constant(result); c && *c == 0)
      return nullPointer(type);
    core::Sym raw = unknownValue(type);
    heap.infoMut(state, raw).rawCast = true;
    return raw;
  }
  if (hint.type == core::SymInfo::Type::Int &&
      loaded.type == core::SymInfo::Type::Pointer) {
    core::Sym number = unknownValue(type);
    heap.infoMut(state, number).pointerBehind = result;
    return number;
  }
  if (loaded.type == core::SymInfo::Type::Unknown &&
      hint.type != core::SymInfo::Type::Unknown)
    return unknownValue(type);
  return result;
}

void Transfer::store(const Address &address, core::Sym value, QualType type,
                     const Expr *at, const std::string &holderName) {
  if (address.top) {
    // A store through a pointer the engine cannot follow: every object that
    // could be the target forgets its cells.
    std::vector<core::ObjectId> ids;
    for (const auto &[id, object] : state.objects) {
      core::ObjectKind kind = run.table().info(id).key.kind;
      if (kind != core::ObjectKind::Local || object.escaped)
        ids.push_back(id);
    }
    for (core::ObjectId id : ids) {
      heap.forgetCells(state, id, 0, std::nullopt);
      state.objects.at(id).havocked = true;
    }
    return;
  }
  bool strong = address.targets.size() == 1 &&
                run.table().info(address.targets[0].object).singular;
  // Two objects of which each path has exactly one: the caller's object a
  // cell's entry value points to, and the one made where that value was
  // null (`if (!g) g = malloc(…); g->data = …`). The store is to the one
  // that exists, strongly.
  if (complementary(address))
    strong = true;
  core::Sym overwritten = core::ZeroSym;
  for (const core::Target &target : address.targets) {
    heap.ensure(state, target.object);
    std::optional<core::CellKey> key = cellKeyOf(target.offset);
    if (!key) {
      // An unknown offset: every cell may be the one written.
      std::vector<core::CellKey> keys;
      for (const auto &[cellKey, sym] : state.objects.at(target.object).cells)
        keys.push_back(cellKey);
      for (const core::CellKey &cellKey : keys)
        heap.write(state, target.object, cellKey, value, true);
      // Every range may hold it too.
      std::vector<core::Segment> segments =
          state.objects.at(target.object).segments;
      for (core::Segment &segment : segments)
        segment.value = heap.mergeWeak(state, segment.value, value);
      state.objects.at(target.object).segments = std::move(segments);
      state.objects.at(target.object).havocked =
          state.objects.at(target.object).havocked || keys.empty();
      state.objects.at(target.object).nulWithin.reset();
      state.objects.at(target.object).nulFrom.reset();
      continue;
    }
    // RFC 0012: the store may overwrite the object's terminator.
    if (auto width = sizeOf(type))
      stringStored(target.object, target.offset, *width, value);
    else
      stringStored(target.object, target.offset, 1U << 20U, value);
    bool weak = !strong || key->isSummary();
    run.writtenCells.insert({target.object, *key});
    if (weak && !heap.read(state, target.object, *key)) {
      core::SymInfo hint = heap.info(state, value);
      run.unwritten(state, target.object, *key, hint);
    }
    if (!weak)
      if (auto old = heap.read(state, target.object, *key))
        overwritten = *old;
    heap.write(state, target.object, *key, value, weak);
    if (run.isPublishing())
      recordFrameStore(target.object, *key, value, at, holderName);
    // Messages name an allocation after where it was last stored.
    if (at != nullptr || !holderName.empty()) {
      const core::SymInfo &stored = heap.info(state, value);
      std::string holder = !holderName.empty() ? holderName : std::string();
      if (holder.empty())
        if (const auto *assign = dyn_cast_or_null<BinaryOperator>(at))
          holder = spell(*assign->getLHS());
      if (!holder.empty())
        for (const core::Target &t : stored.targets)
          if (state.objects.contains(t.object))
            state.objects.at(t.object).holder = holder;
    }
    // A pointer stored into an object that already escaped escapes too;
    // one stored in the caller's memory or a global is reachable from the
    // leak roots, and escapes at the next call (EngineCalls.cpp).
    bool visible = state.objects.at(target.object).escaped;
    if (visible) {
      const core::SymInfo &stored = heap.info(state, value);
      std::vector<core::ObjectId> targets;
      targets.reserve(stored.targets.size());
      for (const core::Target &t : stored.targets)
        targets.push_back(t.object);
      for (core::ObjectId id : targets)
        if (state.objects.contains(id))
          state.objects.at(id).escaped = true;
    }
  }
  if (overwritten != core::ZeroSym && at != nullptr)
    checkLeaks(overwritten, *at);
}

std::vector<core::ParamPairTest> Transfer::pairGuard() const {
  std::vector<core::ParamPairTest> facts;
  const FunctionDecl &function = run.decl();
  std::vector<std::pair<std::uint32_t, core::Sym>> held;
  for (unsigned i = 0; i < function.getNumParams(); ++i) {
    const ParmVarDecl *param = function.getParamDecl(i);
    if (!run.isUnmodifiedParam(i) || !param->getType()->isPointerType())
      continue;
    if (auto sym =
            heap.read(state, run.variableObject(*param), core::CellKey{}))
      held.emplace_back(i, *sym);
  }
  for (std::size_t a = 0; a < held.size(); ++a)
    for (std::size_t b = a + 1; b < held.size(); ++b)
      if (auto equal =
              core::Heap::pointersEqual(state, held[a].second, held[b].second))
        facts.push_back(core::ParamPairTest{
            .first = held[a].first, .second = held[b].first, .equal = *equal});
  return facts;
}

std::optional<bool>
Transfer::argumentsEqual(const std::vector<core::Sym> &args,
                         const core::ParamPairTest &test) const {
  if (test.first >= args.size() || test.second >= args.size())
    return std::nullopt;
  core::Sym x = args[test.first];
  core::Sym y = args[test.second];
  if (auto known = core::Heap::pointersEqual(state, x, y))
    return known;
  const core::SymInfo &a = heap.info(state, x);
  const core::SymInfo &b = heap.info(state, y);
  if (a.type != core::SymInfo::Type::Pointer ||
      b.type != core::SymInfo::Type::Pointer || a.top || b.top)
    return std::nullopt;
  if (a.null == core::PointerNull::Null && b.null == core::PointerNull::Null)
    return true;
  if ((a.null == core::PointerNull::Null &&
       b.null == core::PointerNull::NonNull) ||
      (b.null == core::PointerNull::Null &&
       a.null == core::PointerNull::NonNull))
    return false;
  if (a.null != core::PointerNull::NonNull ||
      b.null != core::PointerNull::NonNull || a.targets.empty() ||
      b.targets.empty())
    return std::nullopt;
  // Into objects that cannot overlap.
  for (const core::Target &s : a.targets)
    for (const core::Target &t : b.targets)
      if (heap.mayOverlap(state, s.object, t.object))
        return std::nullopt;
  return false;
}

std::optional<bool> isZeroValue(const core::Heap &heap,
                                const core::HeapState &state, core::Sym value) {
  const core::SymInfo &info = heap.info(state, value);
  if (info.type == core::SymInfo::Type::Pointer) {
    // A null test of a pointer parameter (`if (b == NULL)`) is its `=0`.
    if (info.null == core::PointerNull::Null)
      return true;
    if (info.null == core::PointerNull::NonNull)
      return false;
    return std::nullopt;
  }
  auto lo = state.zone.lower(value);
  auto hi = state.zone.upper(value);
  if (lo && hi && *lo == 0 && *hi == 0)
    return true;
  if ((lo && *lo > 0) || (hi && *hi < 0) || info.nonZero)
    return false;
  return std::nullopt;
}

std::vector<core::Handle> Transfer::nonNullLocals() const {
  std::vector<core::Handle> held;
  for (const VarDecl *var : run.fixedPointerLocals()) {
    const core::ObjectState *holder =
        heap.findObject(state, run.variableObject(*var));
    const core::Sym *value =
        holder != nullptr ? holder->cells.find(core::CellKey{}) : nullptr;
    if (value != nullptr &&
        heap.info(state, *value).null == core::PointerNull::NonNull)
      held.push_back(handleOf(var));
  }
  std::ranges::sort(held);
  return held;
}

std::vector<std::pair<std::uint32_t, bool>> Transfer::paramGuard() const {
  std::vector<std::pair<std::uint32_t, bool>> facts;
  const FunctionDecl &function = run.decl();
  for (unsigned i = 0; i < function.getNumParams(); ++i) {
    const ParmVarDecl *param = function.getParamDecl(i);
    if (!run.isUnmodifiedParam(i) || !param->getType()->isIntegerType())
      continue;
    auto held = heap.read(state, run.variableObject(*param), core::CellKey{});
    if (!held)
      continue;
    if (auto zero = isZeroValue(heap, state, *held))
      facts.emplace_back(i, *zero);
  }
  return facts;
}

void Transfer::assignVariable(const VarDecl &var, core::Sym sym) {
  core::ObjectId object = run.variableObject(var);
  heap.ensure(state, object);
  Address address;
  address.targets = {core::Target{.object = object}};
  store(address, sym, var.getType(), nullptr);
}

void Transfer::checkLeaks(core::Sym overwritten, const Stmt &at) {
  const core::SymInfo &old = heap.info(state, overwritten);
  if (old.type != core::SymInfo::Type::Pointer || old.targets.empty())
    return;
  std::vector<core::ObjectId> owned;
  for (const core::Target &target : old.targets) {
    const core::ObjectState *object = heap.findObject(state, target.object);
    core::ObjectKind kind = run.table().info(target.object).key.kind;
    if (object != nullptr && object->owned && !object->escaped &&
        object->life == core::Life::Live &&
        object->family != core::StackFamily &&
        (kind == core::ObjectKind::HeapRecent ||
         kind == core::ObjectKind::HeapOld))
      owned.push_back(target.object);
  }
  if (owned.empty() || !run.isPublishing())
    return;
  std::vector<core::ObjectId> reachable =
      heap.reachableObjects(state, run.roots(state));
  for (core::ObjectId id : owned) {
    if (std::ranges::find(reachable, id) != reachable.end())
      continue;
    std::string name = state.objects.at(id).holder;
    if (name.empty())
      name = !old.name.empty() ? old.name : run.table().info(id).name;
    core::Diagnostic diagnostic;
    diagnostic.id = core::diag::Leak;
    diagnostic.severity = core::Severity::Warning;
    diagnostic.message =
        "'" + name + "' is leaked: it is overwritten without being released";
    diagnostic.location =
        toCoreLocation(context.getSourceManager(), at.getBeginLoc());
    run.report(std::move(diagnostic), core::Certainty::Possible, &at,
               std::nullopt);
    // Reported once: the object is no longer owned here.
    state.objects.at(id).owned = false;
  }
}

//===----------------------------------------------------------------------===//
// Declarations and scopes
//===----------------------------------------------------------------------===//

void Transfer::zeroFill(const Address &address, QualType type) {
  (void)type;
  for (const core::Target &target : address.targets) {
    heap.ensure(state, target.object);
    core::ObjectState &object = state.objects.at(target.object);
    if (target.offset.isConstant() && target.offset.constant == 0) {
      object.cells = {};
      object.zeroed = true;
      object.uninitialised = false;
    }
  }
}

void Transfer::initialize(const Address &address, QualType type,
                          const Expr *init) {
  if (init == nullptr)
    return;
  init = init->IgnoreParens();
  if (const auto *list = dyn_cast<InitListExpr>(init)) {
    if (list->isSyntacticForm() && list->getSemanticForm() != nullptr)
      list = list->getSemanticForm();
    QualType canonical = type.getCanonicalType();
    // Everything not named is zero.
    zeroFill(address, type);
    if (list->isStringLiteralInit() && list->getNumInits() == 1) {
      initialize(address, type, list->getInit(0));
      return;
    }
    if (const RecordDecl *record = canonical->getAsRecordDecl()) {
      if (!record->isCompleteDefinition())
        return;
      const ASTRecordLayout &layout = context.getASTRecordLayout(record);
      unsigned index = 0;
      for (const FieldDecl *field : record->fields()) {
        if (record->isUnion() && list->getInitializedFieldInUnion() != field)
          continue;
        if (index >= list->getNumInits())
          break;
        const Expr *fieldInit = list->getInit(record->isUnion() ? 0 : index);
        ++index;
        auto offset = static_cast<std::int64_t>(
            layout.getFieldOffset(field->getFieldIndex()) /
            context.getCharWidth());
        Address fieldAddress = address;
        for (core::Target &target : fieldAddress.targets)
          target.offset = target.offset.plusConstant(offset);
        if (field->isBitField()) {
          if (fieldInit != nullptr && !isa<ImplicitValueInitExpr>(fieldInit))
            (void)storeBitField(fieldAddress, valueOf(*fieldInit),
                                field->getType(), *field, fieldInit);
          continue;
        }
        initialize(fieldAddress, field->getType(), fieldInit);
      }
      return;
    }
    if (const auto *array = context.getAsArrayType(canonical)) {
      auto size = sizeOf(array->getElementType());
      if (!size)
        return;
      for (unsigned i = 0; i < list->getNumInits() && i < 64; ++i) {
        Address element = address;
        for (core::Target &target : element.targets)
          target.offset = target.offset.plusConstant(*size * i);
        initialize(element, array->getElementType(), list->getInit(i));
      }
      return;
    }
    if (list->getNumInits() == 1)
      initialize(address, type, list->getInit(0));
    return;
  }
  if (isa<ImplicitValueInitExpr>(init)) {
    if (type->isScalarType())
      store(address, constant(0, type), type, nullptr);
    return;
  }
  if (const auto *literal = dyn_cast<StringLiteral>(init)) {
    // A character array initialised by a literal holds its characters, then
    // zeros to its end; an array no longer than the literal's characters
    // has no terminator (RFC 0012).
    auto size = sizeOf(type);
    QualType element = context.getAsArrayType(type) != nullptr
                           ? context.getAsArrayType(type)->getElementType()
                           : context.CharTy;
    for (const core::Target &target : address.targets) {
      heap.ensure(state, target.object);
      core::ObjectState &object = state.objects.at(target.object);
      object.cells = {};
      object.uninitialised = false;
      if (literal->getCharByteWidth() != 1 || !size ||
          !target.offset.isConstant()) {
        object.havocked = true;
        continue;
      }
      llvm::StringRef bytes = literal->getBytes();
      auto count = std::min<std::int64_t>(
          static_cast<std::int64_t>(bytes.size()), *size);
      if (count > 64) {
        // Too long to keep byte by byte: the string fact only.
        object.havocked = true;
        std::size_t nul = bytes.find('\0');
        if (nul == llvm::StringRef::npos && count < *size)
          nul = static_cast<std::size_t>(count);
        if (nul != llvm::StringRef::npos && std::cmp_less(nul, *size)) {
          object.nulWithin =
              target.offset.plusConstant(static_cast<std::int64_t>(nul));
          object.nulFrom = target.offset;
        }
        continue;
      }
      object.zeroed = true;
      object.havocked = false;
      object.forgotten.clear();
      object.mayForgotten.clear();
      std::vector<core::Sym> chars;
      chars.reserve(static_cast<std::size_t>(std::max<std::int64_t>(count, 0)));
      for (std::int64_t i = 0; i < count; ++i)
        chars.push_back(
            constant(element->isUnsignedIntegerType()
                         ? static_cast<std::int64_t>(static_cast<unsigned char>(
                               bytes[static_cast<std::size_t>(i)]))
                         : static_cast<std::int64_t>(static_cast<signed char>(
                               bytes[static_cast<std::size_t>(i)])),
                     element));
      for (std::int64_t i = 0; i < count; ++i)
        heap.write(state, target.object,
                   core::CellKey{.offset = target.offset.constant + i},
                   chars[static_cast<std::size_t>(i)], false);
    }
    return;
  }
  QualType canonical = type.getCanonicalType();
  if (canonical->isRecordType()) {
    // A record copied from another: the cells follow (a whole copy).
    const Expr *copied = init;
    if (const auto *cast = dyn_cast<ImplicitCastExpr>(copied);
        cast != nullptr && cast->getCastKind() == CK_LValueToRValue)
      copied = cast->getSubExpr();
    if (copied->isGLValue() && copyRecord(address, addressOf(*copied), type))
      return;
    ExprResult source = evaluate(*init);
    if (source.address && source.address->targets.size() == 1 &&
        address.targets.size() == 1) {
      core::ObjectId from = source.address->targets[0].object;
      core::ObjectId to = address.targets[0].object;
      if (source.address->targets[0].offset.isConstant() &&
          address.targets[0].offset.isConstant() &&
          address.targets[0].offset.constant == 0 &&
          source.address->targets[0].offset.constant == 0 &&
          state.objects.contains(from)) {
        const core::ObjectState original = state.objects.at(from);
        core::ObjectState &copy = heap.ensure(state, to);
        copy.cells = original.cells;
        // What the source's unwritten cells read as (§4.2).
        copy.havocked = original.havocked;
        copy.forgotten = original.forgotten;
        copy.mayForgotten = original.mayForgotten;
        copy.zeroed = original.zeroed;
        copy.uninitialised = original.uninitialised;
        // Messages name an allocation after the member now holding it
        // (`'p.a' is leaked`).
        nameHeldByMembers(to, type, run.table().info(to).name);
        return;
      }
    }
    for (const core::Target &target : address.targets) {
      heap.ensure(state, target.object);
      state.objects.at(target.object).cells = {};
      state.objects.at(target.object).havocked = true;
    }
    return;
  }
  std::string holder;
  if (address.targets.size() == 1)
    holder = run.table().info(address.targets[0].object).name;
  store(address, valueOf(*init), type, init, holder);
}

void Transfer::nameHeldByMembers(core::ObjectId object, QualType type,
                                 const std::string &prefix) {
  const RecordDecl *record = type->getAsRecordDecl();
  const core::ObjectState *holder = heap.findObject(state, object);
  if (record == nullptr || !record->isCompleteDefinition() ||
      holder == nullptr || prefix.empty())
    return;
  const ASTRecordLayout &layout = context.getASTRecordLayout(record);
  std::vector<std::pair<core::ObjectId, std::string>> named;
  for (const FieldDecl *field : record->fields()) {
    if (!field->getType()->isPointerType() || field->getName().empty())
      continue;
    auto offset = static_cast<std::int64_t>(
        layout.getFieldOffset(field->getFieldIndex()) / context.getCharWidth());
    const core::Sym *held = holder->cells.find(core::CellKey{.offset = offset});
    if (held == nullptr)
      continue;
    for (const core::Target &target : heap.info(state, *held).targets)
      named.emplace_back(target.object,
                         prefix + "." + field->getNameAsString());
  }
  for (const auto &[id, name] : named)
    if (state.objects.contains(id) && state.objects.at(id).owned)
      state.objects.at(id).holder = name;
}

void Transfer::declare(const VarDecl &var) {
  core::ObjectId object = run.variableObject(var);
  if (var.hasGlobalStorage()) {
    heap.ensure(state, object);
    return;
  }
  // A fresh activation of the variable.
  core::ObjectState fresh;
  fresh.uninitialised = true;
  QualType type = var.getType();
  if (const auto *vla = context.getAsVariableArrayType(type)) {
    if (auto bytes = bytesOf(type, *vla->getSizeExpr()))
      fresh.extent = core::Extent{.bytes = termOf(*bytes),
                                  .cls = core::ExtentClass::Exact};
  } else if (auto size = sizeOf(type)) {
    fresh.extent = core::Extent{.bytes = core::Term::of(*size),
                                .cls = core::ExtentClass::Exact};
  }
  state.objects.set(object, fresh);
  if (const Expr *init = var.getInit()) {
    Address address;
    address.targets = {core::Target{.object = object}};
    initialize(address, type, init);
    if (type->isArrayType())
      run.byteWrites[object] =
          toCoreLocation(context.getSourceManager(), var.getLocation());
    // RFC 0004: a declaration with a safe kind asserts it of its value.
    if (type->isPointerType() && !isa<InitListExpr>(init->IgnoreParens()))
      if (auto held = heap.read(state, object, core::CellKey{})) {
        core::Sym laundered =
            launder(*held, getAnnotations(var), var.getNameAsString(), *init);
        if (laundered != *held)
          heap.write(state, object, core::CellKey{}, laundered, false);
      }
  }
}

void Transfer::lifetimeEnds(const VarDecl &var) {
  if (var.hasGlobalStorage())
    return;
  core::ObjectId object = run.variableObject(var);
  if (!state.objects.contains(object))
    return;
  // (The record a `return` hands back keeps what it holds: the caller
  // receives it.)
  if (const core::Sym *returned = state.exprs.find(handleOf(&run.decl())))
    for (const core::Target &target : heap.info(state, *returned).targets)
      if (target.object == object) {
        state.objects.at(object).life = core::Life::Ended;
        return;
      }
  core::ObjectState &ended = state.objects.at(object);
  ended.life = core::Life::Ended;
  ended.cells = {};
}

void Transfer::returned(const ReturnStmt &ret) {
  if (const Expr *value = ret.getRetValue()) {
    QualType type = value->getType();
    if (type->isRecordType()) {
      state.result = core::ZeroSym;
      // RFC 0013: where the returned record is, for the summary's
      // `result.f` stores (EngineSummary.cpp).
      const Expr *returnedRecord = value->IgnoreParens();
      if (const auto *cast = dyn_cast<ImplicitCastExpr>(returnedRecord);
          cast != nullptr && cast->getCastKind() == CK_LValueToRValue)
        returnedRecord = cast->getSubExpr();
      std::optional<Address> storage;
      if (returnedRecord->isGLValue())
        storage = addressOf(*returnedRecord);
      else
        storage = evaluate(*returnedRecord).address;
      if (storage && !storage->top && storage->targets.size() == 1)
        state.exprs.set(handleOf(&run.decl()),
                        pointerTo(*storage, type, std::string()));
    } else {
      state.result = valueOf(*value);
    }
    // RFC 0004: returning a raw value from a function whose result is
    // declared with a safe kind asserts that kind.
    if (type->isPointerType()) {
      AnnotationSet declared;
      for (const FunctionDecl *redecl : run.decl().redecls()) {
        AnnotationSet set = getAnnotations(*redecl);
        declared.owned = declared.owned || set.owned;
        declared.borrowed = declared.borrowed || set.borrowed;
        declared.mutBorrowed = declared.mutBorrowed || set.mutBorrowed;
      }
      state.result = launder(state.result, declared, std::string(), *value);
    }
    if (state.result != core::ZeroSym)
      checkReturnAnnotation(state.result, *value);
  }
  if (run.isPublishing()) {
    decideSites(ret);
    exitLifetimes(ret, &ret);
  }
  run.checkLeaks(state, ret, FunctionRun::LeakPoint::Exit);
  run.noteExit(state);
}

//===----------------------------------------------------------------------===//
// Evaluation
//===----------------------------------------------------------------------===//

core::Sym Transfer::pointerAdd(core::Sym pointer, core::Sym index,
                               std::int64_t size, bool subtract,
                               const Expr &at) {
  const core::SymInfo base = heap.info(state, pointer);
  core::SymInfo out = base;
  out.entryOf.reset();
  out.name = spell(at);
  out.pending.clear();
  out.condition.reset();
  out.linear.reset();
  out.derived = true;
  // (Non-zero arithmetic on null is undefined: the result is null exactly
  // when the pointer is, and follows it when a check or a test decides it.)
  auto follow = [&](core::Sym result) {
    core::Sym root = pointer;
    for (const auto &[derived, from] : state.nullFollows)
      if (derived == pointer) {
        root = from;
        break;
      }
    state.nullFollows.emplace_back(result, root);
    return result;
  };
  // A non-zero amount makes a non-null pointer: were the pointer null, the
  // arithmetic would be undefined, and its result is not the null pointer
  // (a check of it could not fail).
  const auto lowest = state.zone.lower(index);
  const auto highest = state.zone.upper(index);
  const bool nonZero =
      size != 0 && ((lowest && *lowest > 0) || (highest && *highest < 0));
  if (base.type != core::SymInfo::Type::Pointer) {
    out.type = core::SymInfo::Type::Pointer;
    out.targets.clear();
    core::ObjectId any = run.unknownObject();
    heap.ensure(state, any);
    out.targets = {core::Target{.object = any}};
    out.null = nonZero ? core::PointerNull::NonNull : core::PointerNull::Maybe;
    core::Sym result = heap.fresh(state, out);
    return nonZero ? result : follow(result);
  }
  if (nonZero && out.null == core::PointerNull::Maybe)
    out.null = core::PointerNull::NonNull;
  core::Term step = termOf(index);
  if (step.known) {
    step.scale *= subtract ? -size : size;
    step.constant *= subtract ? -size : size;
    if (step.isConstant())
      step.scale = 0;
  }
  for (core::Target &target : out.targets) {
    auto sum = step.known ? target.offset.plus(step) : std::nullopt;
    target.offset = sum && sum->known ? *sum : core::Term::unknown();
  }
  // RFC 0015 array storage (§4.2 *Amendment (arrays)*): an index that is
  // not a constant expression makes the object's cells of this size
  // elements, which fold into ranges at loop heads.
  const Expr *indexExpr = nullptr;
  if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(&at))
    indexExpr = subscript->getIdx();
  else if (const auto *binary = dyn_cast<BinaryOperator>(&at))
    indexExpr = binary->getLHS()->getType()->isIntegerType() ? binary->getLHS()
                                                             : binary->getRHS();
  bool variable = isa<UnaryOperator>(at) ||
                  (indexExpr != nullptr && !indexExpr->isValueDependent() &&
                   !indexExpr->isIntegerConstantExpr(context));
  if (variable && size > 0 && std::cmp_less_equal(size, 1U << 20U))
    for (const core::Target &target : base.targets)
      if (state.objects.contains(target.object) &&
          state.objects.at(target.object).stride == 0)
        state.objects.at(target.object).stride =
            static_cast<std::uint32_t>(size);
  // The result keeps the pointer's nullness.
  core::Sym result = heap.fresh(state, out);
  return out.null == core::PointerNull::Maybe ? follow(result) : result;
}

ExprResult Transfer::evaluateUncached(const Expr &expr) {
  ExprResult result;
  switch (expr.getStmtClass()) {
  case Stmt::ParenExprClass:
    return evaluate(*cast<ParenExpr>(expr).getSubExpr());
  case Stmt::ConstantExprClass:
  case Stmt::ExprWithCleanupsClass:
    return evaluate(*cast<FullExpr>(expr).getSubExpr());
  case Stmt::IntegerLiteralClass: {
    const auto &literal = cast<IntegerLiteral>(expr);
    result.value = constant(
        llvm::APSInt(literal.getValue(), /*isUnsigned=*/true), expr.getType());
    return result;
  }
  case Stmt::CharacterLiteralClass:
    result.value =
        constant(cast<CharacterLiteral>(expr).getValue(), expr.getType());
    return result;
  case Stmt::StringLiteralClass:
  case Stmt::PredefinedExprClass: {
    core::ObjectId object = run.literalObject(expr);
    core::ObjectState &literal = heap.ensure(state, object);
    literal.readonly = true;
    // Its bytes are the literal's (EngineStrings.cpp, FunctionRun::unwritten).
    if (auto size = sizeOf(expr.getType()))
      literal.extent = core::Extent{.bytes = core::Term::of(*size),
                                    .cls = core::ExtentClass::Exact};
    Address address;
    address.targets = {core::Target{.object = object}};
    result.address = address;
    return result;
  }
  case Stmt::DeclRefExprClass: {
    const auto &ref = cast<DeclRefExpr>(expr);
    const ValueDecl *decl = ref.getDecl();
    if (const auto *var = dyn_cast<VarDecl>(decl)) {
      core::ObjectId object = run.variableObject(*var);
      core::ObjectState &variable = heap.ensure(state, object);
      if (!variable.extent)
        if (auto size = sizeOf(var->getType()))
          variable.extent = core::Extent{.bytes = core::Term::of(*size),
                                         .cls = core::ExtentClass::Exact};
      Address address;
      address.targets = {core::Target{.object = object}};
      result.address = address;
      return result;
    }
    if (const auto *enumerator = dyn_cast<EnumConstantDecl>(decl)) {
      llvm::APSInt value = enumerator->getInitVal();
      result.value = value.getSignificantBits() <= 64
                         ? constant(value.getExtValue(), expr.getType())
                         : unknownValue(expr.getType());
      return result;
    }
    if (const auto *fn = dyn_cast<FunctionDecl>(decl)) {
      core::SymInfo info;
      info.type = core::SymInfo::Type::Function;
      info.functionsKnown = true;
      info.functions = {handleOf(fn->getCanonicalDecl())};
      info.name = fn->getNameAsString();
      result.value = heap.fresh(state, info);
      return result;
    }
    result.value = unknownValue(expr.getType());
    return result;
  }
  case Stmt::MemberExprClass: {
    const auto &member = cast<MemberExpr>(expr);
    const auto *field = dyn_cast<FieldDecl>(member.getMemberDecl());
    std::int64_t offset = 0;
    if (field != nullptr && field->getParent()->isCompleteDefinition()) {
      const ASTRecordLayout &layout =
          context.getASTRecordLayout(field->getParent());
      offset = static_cast<std::int64_t>(
          layout.getFieldOffset(field->getFieldIndex()) /
          context.getCharWidth());
    }
    Address address;
    if (member.isArrow()) {
      core::Sym base = valueOf(*member.getBase());
      const core::SymInfo &info = heap.info(state, base);
      address.base = base;
      address.top = info.top;
      address.targets = info.targets;
      if (info.type != core::SymInfo::Type::Pointer)
        address.top = true;
    } else {
      address = addressOf(*member.getBase());
    }
    for (core::Target &target : address.targets)
      target.offset = target.offset.plusConstant(offset);
    result.address = address;
    return result;
  }
  case Stmt::ArraySubscriptExprClass: {
    const auto &subscript = cast<ArraySubscriptExpr>(expr);
    core::Sym base = valueOf(*subscript.getBase());
    core::Sym index = valueOf(*subscript.getIdx());
    std::int64_t size = sizeOf(expr.getType()).value_or(1);
    core::Sym element = pointerAdd(base, index, size, false, expr);
    const core::SymInfo &info = heap.info(state, element);
    Address address;
    address.base = base;
    address.targets = info.targets;
    address.top = info.top || info.type != core::SymInfo::Type::Pointer;
    result.address = address;
    return result;
  }
  case Stmt::UnaryOperatorClass: {
    const auto &unary = cast<UnaryOperator>(expr);
    if (unary.getOpcode() == UO_Deref) {
      core::Sym base = valueOf(*unary.getSubExpr());
      const core::SymInfo &info = heap.info(state, base);
      Address address;
      address.base = base;
      address.targets = info.targets;
      address.top = info.top || info.type != core::SymInfo::Type::Pointer;
      if (info.type == core::SymInfo::Type::Function)
        address.top = false;
      result.address = address;
      if (info.type == core::SymInfo::Type::Function)
        result.value = base;
      return result;
    }
    if (unary.getOpcode() == UO_AddrOf) {
      const Expr *sub = unary.getSubExpr();
      if (sub->getType()->isFunctionType()) {
        result.value = valueOf(*sub);
        return result;
      }
      Address address = addressOf(*sub);
      result.value = pointerTo(address, sub->getType(), spell(*sub));
      return result;
    }
    result.value = evaluateUnary(unary);
    return result;
  }
  case Stmt::BinaryOperatorClass:
  case Stmt::CompoundAssignOperatorClass:
    result.value = evaluateBinary(cast<BinaryOperator>(expr));
    if (result.value == core::ZeroSym)
      result.value = unknownValue(expr.getType());
    return result;
  case Stmt::ImplicitCastExprClass:
  case Stmt::CStyleCastExprClass: {
    const auto &castExpr = cast<CastExpr>(expr);
    if (castExpr.getCastKind() == CK_LValueBitCast ||
        castExpr.getCastKind() == CK_LValueToRValueBitCast) {
      result.address = addressOf(*castExpr.getSubExpr());
      return result;
    }
    if (castExpr.getCastKind() == CK_NoOp && expr.isGLValue()) {
      return evaluate(*castExpr.getSubExpr());
    }
    result.value = evaluateCast(castExpr);
    return result;
  }
  case Stmt::ConditionalOperatorClass:
  case Stmt::BinaryConditionalOperatorClass: {
    // The arms recorded their values under the operator (§2).
    if (const core::Sym *value = state.exprs.find(handleOf(&expr))) {
      result.value = *value;
      return result;
    }
    const auto &conditional = cast<AbstractConditionalOperator>(expr);
    auto armValue = [&](const Expr *arm) -> core::Sym {
      if (auto it = memo.find(arm); it != memo.end())
        return it->second.value;
      return core::ZeroSym;
    };
    core::Sym a = armValue(conditional.getTrueExpr());
    core::Sym b = armValue(conditional.getFalseExpr());
    if (a != core::ZeroSym && b != core::ZeroSym)
      result.value = heap.mergeWeak(state, a, b);
    else if (a != core::ZeroSym || b != core::ZeroSym)
      result.value = a != core::ZeroSym ? a : b;
    else
      result.value = unknownValue(expr.getType());
    return result;
  }
  case Stmt::CallExprClass: {
    // RFC 0013: a record returned by value lands in a temporary of the
    // caller, which the callee's summary describes (`result.f`) or, for a
    // callee the analysis does not follow, holds unknown values.
    std::optional<Address> temporary;
    if (expr.getType()->isRecordType()) {
      core::ObjectId object = run.recordResultObject(cast<CallExpr>(expr));
      core::ObjectState fresh;
      fresh.havocked = true;
      if (auto size = sizeOf(expr.getType()))
        fresh.extent = core::Extent{.bytes = core::Term::of(*size),
                                    .cls = core::ExtentClass::Exact};
      state.objects.set(object, fresh);
      temporary = Address{};
      temporary->targets = {core::Target{.object = object}};
    }
    result.value = call(cast<CallExpr>(expr));
    if (temporary)
      result.address = std::move(temporary);
    return result;
  }
  case Stmt::UnaryExprOrTypeTraitExprClass: {
    const auto &trait = cast<UnaryExprOrTypeTraitExpr>(expr);
    if (trait.getKind() == UETT_SizeOf) {
      QualType argument = trait.getTypeOfArgument();
      if (argument->isVariablyModifiedType()) {
        // `sizeof vla`: the dimensions captured at its declaration.
        if (auto bytes = bytesOf(argument, expr)) {
          result.value = *bytes;
          return result;
        }
        result.value = unknownValue(expr.getType());
        return result;
      }
    }
    Expr::EvalResult value;
    if (expr.EvaluateAsInt(value, context))
      result.value = constant(value.Val.getInt().getExtValue(), expr.getType());
    else
      result.value = unknownValue(expr.getType());
    return result;
  }
  case Stmt::OffsetOfExprClass: {
    Expr::EvalResult value;
    if (expr.EvaluateAsInt(value, context))
      result.value = constant(value.Val.getInt().getExtValue(), expr.getType());
    else
      result.value = unknownValue(expr.getType());
    return result;
  }
  case Stmt::CompoundLiteralExprClass: {
    const auto &literal = cast<CompoundLiteralExpr>(expr);
    core::ObjectId object = run.literalObject(expr);
    core::ObjectState fresh;
    if (auto size = sizeOf(expr.getType()))
      fresh.extent = core::Extent{.bytes = core::Term::of(*size),
                                  .cls = core::ExtentClass::Exact};
    state.objects.set(object, fresh);
    Address address;
    address.targets = {core::Target{.object = object}};
    initialize(address, expr.getType(), literal.getInitializer());
    result.address = address;
    return result;
  }
  case Stmt::ImplicitValueInitExprClass:
    result.value = expr.getType()->isPointerType()
                       ? nullPointer(expr.getType())
                       : constant(0, expr.getType());
    return result;
  case Stmt::StmtExprClass: {
    const auto &statement = cast<StmtExpr>(expr);
    const CompoundStmt *body = statement.getSubStmt();
    if (body != nullptr && !body->body_empty())
      if (const auto *last = dyn_cast<Expr>(body->body_back())) {
        result.value = valueOf(*last);
        return result;
      }
    result.value = unknownValue(expr.getType());
    return result;
  }
  case Stmt::ChooseExprClass:
    return evaluate(*cast<ChooseExpr>(expr).getChosenSubExpr());
  case Stmt::GenericSelectionExprClass:
    return evaluate(*cast<GenericSelectionExpr>(expr).getResultExpr());
  case Stmt::OpaqueValueExprClass:
    if (const Expr *source = cast<OpaqueValueExpr>(expr).getSourceExpr())
      return evaluate(*source);
    result.value = unknownValue(expr.getType());
    return result;
  case Stmt::VAArgExprClass:
    result.value = unknownValue(expr.getType());
    if (expr.getType()->isPointerType())
      heap.infoMut(state, result.value).rawCast = true;
    return result;
  case Stmt::InitListExprClass:
    result.value = unknownValue(expr.getType());
    return result;
  case Stmt::AtomicExprClass: {
    // RFC 0031 §5.1: an atomic operation's value is unknown, and what it
    // may write through its pointer operands (every one but the location a
    // plain load reads) holds unknown bytes after it.
    const auto &atomic = cast<AtomicExpr>(expr);
    const bool load = atomic.getOp() == AtomicExpr::AO__atomic_load_n ||
                      atomic.getOp() == AtomicExpr::AO__c11_atomic_load;
    std::vector<std::pair<const Expr *, core::Sym>> operands;
    for (unsigned i = 0; i < atomic.getNumSubExprs(); ++i)
      if (const Expr *operand = atomic.getSubExprs()[i])
        operands.emplace_back(operand, valueOf(*operand));
    for (const auto &[operand, value] : operands) {
      QualType type = operand->getType();
      if (!type->isPointerType() || type->getPointeeType().isConstQualified() ||
          (load && operand == atomic.getPtr()))
        continue;
      const core::SymInfo info = heap.info(state, value);
      if (info.type != core::SymInfo::Type::Pointer)
        continue;
      auto bytes = sizeOf(type->getPointeeType());
      for (const core::Target &target : info.targets) {
        heap.ensure(state, target.object);
        if (bytes && target.offset.isConstant())
          heap.forgetCells(state, target.object, target.offset.constant, bytes);
        else
          heap.forgetCells(state, target.object, 0, std::nullopt);
        state.objects.at(target.object).stored = true;
      }
    }
    result.value = unknownValue(expr.getType());
    return result;
  }
  default:
    result.value = unknownValue(expr.getType());
    return result;
  }
}

void Transfer::uninitialisedRead(core::Sym value, const Expr &lvalue,
                                 const Address &address) {
  // RFC 0008, RFC 0031 §5.9: reading a local pointer that no path assigned
  // (to copy, pass, release or dereference it) is the error, reported where
  // it is read and once per value: a copy reports at the copy.
  if (!run.isPublishing() || address.top || address.targets.empty())
    return;
  const core::SymInfo &info = heap.info(state, value);
  if (info.type != core::SymInfo::Type::Pointer || !info.uninit ||
      info.null != core::PointerNull::Null)
    return;
  for (const core::Target &target : address.targets)
    if (run.table().info(target.object).key.kind != core::ObjectKind::Local)
      return;
  if (!run.reportedUninit.insert(value).second)
    return;
  core::Diagnostic diagnostic;
  diagnostic.id = core::diag::UseOfUninitialized;
  diagnostic.severity = core::Severity::Error;
  diagnostic.message =
      "use of '" + spell(lvalue) + "' before it was initialized";
  diagnostic.location =
      toCoreLocation(context.getSourceManager(), lvalue.getBeginLoc());
  const core::ObjectId object = address.targets.front().object;
  const core::ObjectInfo &declared = run.table().info(object);
  if (declared.created.isValid())
    diagnostic.addNote("'" + run.frameName(object) + "' is declared here",
                       declared.created);
  run.report(std::move(diagnostic), core::Certainty::Definite, nullptr,
             core::Facet::Null);
}

core::Sym Transfer::evaluateCast(const CastExpr &castExpr) {
  const Expr &sub = *castExpr.getSubExpr();
  QualType type = castExpr.getType();
  switch (castExpr.getCastKind()) {
  case CK_LValueToRValue: {
    ExprResult operand = evaluate(sub);
    if (!operand.address)
      return operand.value != core::ZeroSym ? operand.value
                                            : unknownValue(type);
    const FieldDecl *bitField = sub.getSourceBitField();
    core::Sym loaded =
        bitField != nullptr
            ? loadBitField(*operand.address, type, *bitField, &castExpr)
            : load(*operand.address, type, &castExpr);
    // RFC 0004: a place declared `WEAVEC_RAW` (a field, a local) holds raw
    // pointers.
    if (heap.info(state, loaded).type == core::SymInfo::Type::Pointer) {
      const Expr *place = sub.IgnoreParenImpCasts();
      const Decl *declared = nullptr;
      if (const auto *member = dyn_cast<MemberExpr>(place))
        declared = member->getMemberDecl();
      else if (const auto *ref = dyn_cast<DeclRefExpr>(place);
               ref != nullptr && !isa<ParmVarDecl>(ref->getDecl()))
        declared = ref->getDecl();
      if (declared != nullptr && getAnnotations(*declared).raw &&
          !heap.info(state, loaded).raw) {
        core::SymInfo copy = heap.info(state, loaded);
        copy.pending.clear();
        copy.entryOf.reset();
        copy.raw = true;
        copy.rawOrigin = core::SymInfo::RawOrigin::Declared;
        copy.rawAt =
            toCoreLocation(context.getSourceManager(), declared->getLocation());
        loaded = heap.fresh(state, copy);
      }
    }
    // §4.5 D6: a pointer loaded from an owning slot of the object `base`
    // points to is distinct from that object (A3), whatever becomes of it.
    if (const auto *member = dyn_cast<MemberExpr>(sub.IgnoreParenImpCasts());
        member != nullptr && member->isArrow() &&
        heap.info(state, loaded).type == core::SymInfo::Type::Pointer &&
        operand.address->base != core::ZeroSym &&
        run.unitRun().owningSlots.contains(
            member->getMemberDecl()->getCanonicalDecl())) {
      const core::Sym base = operand.address->base;
      const std::vector<core::Sym> above = heap.info(state, base).ancestors;
      core::SymInfo &derived = heap.infoMut(state, loaded);
      if (std::ranges::find(derived.ancestors, base) ==
          derived.ancestors.end()) {
        derived.ancestors.insert(derived.ancestors.begin(), base);
        for (core::Sym ancestor : above)
          if (derived.ancestors.size() < 8 &&
              std::ranges::find(derived.ancestors, ancestor) ==
                  derived.ancestors.end())
            derived.ancestors.push_back(ancestor);
        if (derived.ancestors.size() > 8)
          derived.ancestors.resize(8);
      }
    }
    // Name the loaded value after the lvalue it was read from.
    core::SymInfo &info = heap.infoMut(state, loaded);
    // (A raw value keeps the first place it was read from.)
    if (info.raw && info.rawVia.empty() && !info.name.empty() &&
        info.name.find('(') == std::string::npos)
      info.rawVia = info.name;
    if (info.name.empty() || info.type == core::SymInfo::Type::Pointer)
      info.name = spell(sub);
    uninitialisedRead(loaded, sub, *operand.address);
    return loaded;
  }
  case CK_ArrayToPointerDecay: {
    Address address = addressOf(sub);
    QualType element =
        context.getAsArrayType(sub.getType()) != nullptr
            ? context.getAsArrayType(sub.getType())->getElementType()
            : QualType();
    return pointerTo(address, element, spell(sub));
  }
  case CK_FunctionToPointerDecay:
  case CK_BuiltinFnToFnPtr:
    return valueOf(sub);
  case CK_NullToPointer: {
    core::Sym null = nullPointer(type);
    heap.infoMut(state, null).nullOrigin =
        core::NullOrigin{.reason = core::NullOrigin::Reason::Assigned,
                         .where = toCoreLocation(context.getSourceManager(),
                                                 castExpr.getBeginLoc()),
                         .detail = {}};
    return null;
  }
  case CK_NoOp:
  case CK_BitCast:
  case CK_AddressSpaceConversion:
  case CK_AtomicToNonAtomic:
  case CK_NonAtomicToAtomic: {
    core::Sym value = valueOf(sub);
    // An allocation gets its type from its first typed use.
    if (type->isPointerType()) {
      QualType pointee = type->getPointeeType();
      if (!pointee->isVoidType() && !pointee->isCharType() &&
          !pointee->isIncompleteType() && !pointee->isFunctionType()) {
        const core::SymInfo &info = heap.info(state, value);
        for (const core::Target &target : info.targets) {
          core::ObjectKind kind = run.table().info(target.object).key.kind;
          if ((kind == core::ObjectKind::HeapRecent ||
               kind == core::ObjectKind::HeapOld) &&
              target.offset.isConstant() && target.offset.constant == 0)
            run.table().setTypeIfUnknown(target.object, typeHandle(pointee));
        }
      }
    }
    return value;
  }
  case CK_IntegralToPointer: {
    core::Sym value = valueOf(sub);
    const core::SymInfo &info = heap.info(state, value);
    if (info.pointerBehind != core::ZeroSym)
      return info.pointerBehind;
    if (auto c = state.zone.constant(value); c && *c == 0)
      return nullPointer(type);
    core::Sym raw = unknownValue(type);
    core::SymInfo &rawInfo = heap.infoMut(state, raw);
    rawInfo.raw = true;
    rawInfo.rawAt =
        toCoreLocation(context.getSourceManager(), castExpr.getBeginLoc());
    return raw;
  }
  case CK_PointerToIntegral: {
    core::Sym pointer = valueOf(sub);
    // RFC 0007 *Escape*: a pointer cast to an integer may be kept where
    // the analysis cannot follow it, so what it points to is not leaked.
    {
      std::vector<core::ObjectId> targets;
      for (const core::Target &target : heap.info(state, pointer).targets)
        targets.push_back(target.object);
      for (core::ObjectId id : targets)
        if (state.objects.contains(id))
          state.objects.at(id).escaped = true;
    }
    core::Sym number = unknownValue(type);
    heap.infoMut(state, number).pointerBehind = pointer;
    if (heap.info(state, pointer).null == core::PointerNull::Null)
      state.zone.addRange(number, 0, 0);
    return number;
  }
  case CK_PointerToBoolean:
  case CK_IntegralToBoolean:
  case CK_MemberPointerToBoolean: {
    core::Sym operand = valueOf(sub);
    core::Sym result = unknownValue(type);
    state.zone.addRange(result, 0, 1);
    core::SymInfo &info = heap.infoMut(state, result);
    info.condition =
        core::Condition{.op = core::Condition::Op::NonZero, .left = operand};
    // Decided statically when the operand is.
    const core::SymInfo &op = heap.info(state, operand);
    if (op.type == core::SymInfo::Type::Pointer) {
      if (op.null == core::PointerNull::Null)
        state.zone.addRange(result, 0, 0);
      else if (op.null == core::PointerNull::NonNull)
        state.zone.addRange(result, 1, 1);
    } else if (auto lo = state.zone.lower(operand); lo && *lo > 0) {
      state.zone.addRange(result, 1, 1);
    } else if (auto c = state.zone.constant(operand)) {
      state.zone.addRange(result, *c != 0 ? 1 : 0, *c != 0 ? 1 : 0);
    }
    return result;
  }
  case CK_IntegralCast:
    // RFC 0017: modular conversion.
    return convertInteger(valueOf(sub), sub.getType(), type);
  case CK_BooleanToSignedIntegral: {
    (void)valueOf(sub);
    core::Sym out = unknownValue(type);
    state.zone.addRange(out, -1, 0);
    return out;
  }
  case CK_ToVoid:
  default:
    (void)valueOf(sub);
    return unknownValue(type);
  }
}

core::Sym Transfer::evaluateUnary(const UnaryOperator &op) {
  const Expr &sub = *op.getSubExpr();
  QualType type = op.getType();
  switch (op.getOpcode()) {
  case UO_PreInc:
  case UO_PreDec:
  case UO_PostInc:
  case UO_PostDec: {
    Address address = addressOf(sub);
    QualType valueType = sub.getType();
    const FieldDecl *bitField = sub.getSourceBitField();
    core::Sym old = bitField != nullptr
                        ? loadBitField(address, valueType, *bitField, &op)
                        : load(address, valueType, &op);
    bool increment = op.isIncrementOp();
    core::Sym updated = core::ZeroSym;
    if (valueType->isPointerType()) {
      core::Sym one = constant(1, context.IntTy);
      std::int64_t size = sizeOf(valueType->getPointeeType()).value_or(1);
      updated = pointerAdd(old, one, size, !increment, sub);
    } else {
      // RFC 0017: a narrow operand is promoted, then converted back.
      QualType computation = valueType;
      if (valueType->isIntegerType() &&
          context.isPromotableIntegerType(valueType))
        computation = context.getPromotedIntegerType(valueType);
      core::Sym widened = computation == valueType
                              ? old
                              : convertInteger(old, valueType, computation);
      core::Sym one = constant(1, computation);
      updated = arithmetic(increment ? BO_Add : BO_Sub, widened, one,
                           computation, op);
      if (computation != valueType)
        updated = convertInteger(updated, computation, valueType);
    }
    checkWriteAnnotation(address, op);
    if (bitField != nullptr)
      updated = storeBitField(address, updated, valueType, *bitField, &op);
    else
      store(address, updated, valueType, &op);
    return op.isPrefix() ? updated : old;
  }
  case UO_Plus:
    return valueOf(sub);
  case UO_Minus: {
    core::Sym zero = constant(0, type);
    return arithmetic(BO_Sub, zero, valueOf(sub), type, op);
  }
  case UO_LNot: {
    core::Sym operand = valueOf(sub);
    core::Sym result = unknownValue(type);
    state.zone.addRange(result, 0, 1);
    core::SymInfo &info = heap.infoMut(state, result);
    info.condition = core::Condition{.op = core::Condition::Op::Eq,
                                     .left = operand,
                                     .rightIsConstant = true,
                                     .constant = 0};
    return result;
  }
  case UO_Not: {
    (void)valueOf(sub);
    return unknownValue(type);
  }
  case UO_Extension:
    return valueOf(sub);
  default:
    (void)valueOf(sub);
    return unknownValue(type);
  }
}

//===----------------------------------------------------------------------===//
// Integers (RFC 0017, RFC 0031 §4.4, §5.10)
//===----------------------------------------------------------------------===//

/// The C integer operation of a binary operator.
static std::optional<core::IntegerOp> integerOpOf(BinaryOperatorKind kind) {
  switch (kind) {
  case BO_Add:
    return core::IntegerOp::Add;
  case BO_Sub:
    return core::IntegerOp::Subtract;
  case BO_Mul:
    return core::IntegerOp::Multiply;
  case BO_Div:
    return core::IntegerOp::Divide;
  case BO_Rem:
    return core::IntegerOp::Remainder;
  case BO_Shl:
    return core::IntegerOp::ShiftLeft;
  case BO_Shr:
    return core::IntegerOp::ShiftRight;
  case BO_And:
    return core::IntegerOp::BitAnd;
  case BO_Or:
    return core::IntegerOp::BitOr;
  case BO_Xor:
    return core::IntegerOp::BitXor;
  default:
    return std::nullopt;
  }
}

static bool commutative(core::IntegerOp op) {
  return op == core::IntegerOp::Add || op == core::IntegerOp::Multiply ||
         op == core::IntegerOp::BitAnd || op == core::IntegerOp::BitOr ||
         op == core::IntegerOp::BitXor;
}

/// The least and greatest values of `type`, as mathematical integers.
static __int128 typeMin(const core::IntegerType &type) {
  if (type.isBoolean || !type.isSigned)
    return 0;
  return -static_cast<__int128>(static_cast<unsigned __int128>(1)
                                << (type.width - 1));
}
static __int128 typeMax(const core::IntegerType &type) {
  if (type.isBoolean)
    return 1;
  if (type.isSigned)
    return static_cast<__int128>(static_cast<unsigned __int128>(1)
                                 << (type.width - 1)) -
           1;
  return static_cast<__int128>(static_cast<unsigned __int128>(1)
                               << type.width) -
         1;
}

/// An integer value as a mathematical integer.
static __int128 mathematical(const core::IntegerValue &value) {
  return value.negative() ? -static_cast<__int128>(value.magnitude())
                          : static_cast<__int128>(value.magnitude());
}

/// `sym`'s bounds as a value of `type`: its zone bounds within the type's
/// range (every integer symbol lies in its type's range, §4.4), and its
/// interval where the zone cannot hold it (an unsigned 64-bit value above
/// `INT64_MAX`).
static std::pair<__int128, __int128> boundsOf(const core::HeapState &state,
                                              core::Sym sym,
                                              const core::IntegerType &type) {
  __int128 lo = typeMin(type);
  __int128 hi = typeMax(type);
  if (auto lower = state.zone.lower(sym); lower && *lower > lo)
    lo = *lower;
  if (auto upper = state.zone.upper(sym); upper && *upper < hi)
    hi = *upper;
  if (const core::SymInfo *info = state.syms.find(sym);
      info != nullptr && info->values && info->values->type == type &&
      !info->values->empty()) {
    lo = std::max(lo, mathematical(*info->values->minimum()));
    hi = std::min(hi, mathematical(*info->values->maximum()));
  }
  if (lo > hi)
    return {typeMin(type), typeMax(type)};
  return {lo, hi};
}

/// `sym`'s values as a `type`, for RFC 0017's range evaluation.
static core::IntegerRange rangeOf(const core::HeapState &state, core::Sym sym,
                                  const core::IntegerType &type) {
  auto [lo, hi] = boundsOf(state, sym, type);
  core::IntegerRange range = core::IntegerRange::between(
      core::IntegerValue::ofBits(type, static_cast<std::uint64_t>(lo)),
      core::IntegerValue::ofBits(type, static_cast<std::uint64_t>(hi)));
  if (const core::SymInfo *info = state.syms.find(sym);
      info != nullptr && info->values && info->values->type == type)
    if (core::IntegerRange both = range.intersect(*info->values); !both.empty())
      return both;
  return range;
}

/// Bounds `sym` by `range`: the zone takes the hull's 64-bit bounds, and a
/// range beyond them (an unsigned 64-bit value above `INT64_MAX`) is kept as
/// the symbol's interval (§4.4).
static void boundBy(const core::Heap &heap, core::HeapState &state,
                    core::Sym sym, const core::IntegerRange &range) {
  if (range.empty() || range.isFull())
    return;
  std::optional<std::int64_t> lo = range.minimum()->signedValue();
  std::optional<std::int64_t> hi = range.maximum()->signedValue();
  if (lo || hi)
    state.zone.addRange(sym, lo, hi);
  if (hi)
    return;
  core::SymInfo &info = heap.infoMut(state, sym);
  if (info.type != core::SymInfo::Type::Int)
    return;
  if (info.values && info.values->type == range.type) {
    core::IntegerRange both = info.values->intersect(range);
    if (!both.empty())
      info.values = both;
  } else {
    info.values = range;
  }
}

static __int128 floorDiv(__int128 value, __int128 divisor) {
  return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

/// RFC 0017 wrap-around: the multiple of 2^width that every value of the
/// mathematical range `[lo, hi]` loses when it is stored in `type`, when
/// they all lose the same one (`UINT_MAX + 2` is `1` with a shift of 2^32).
static std::optional<__int128> wrapShift(__int128 lo, __int128 hi,
                                         const core::IntegerType &type) {
  if (type.isBoolean || type.width == 0 || type.width > 64)
    return std::nullopt;
  auto modulus =
      static_cast<__int128>(static_cast<unsigned __int128>(1) << type.width);
  __int128 k = floorDiv(lo - typeMin(type), modulus);
  if (floorDiv(hi - typeMin(type), modulus) != k)
    return std::nullopt;
  return k * modulus;
}

static bool fitsInt64(__int128 value) {
  return value >= INT64_MIN && value <= INT64_MAX;
}

/// Whether `base + delta` stays within `type` because the zone keeps
/// `base` below another integer by at least `delta` (`i < n` for a `size_t
/// i`, whose upper bound does not fit the zone's 64-bit bounds).
static bool belowAnother(const core::Heap &heap, const core::HeapState &state,
                         core::Sym base, __int128 delta,
                         const core::IntegerType &type) {
  for (core::Sym other : state.zone.symbols()) {
    if (other == core::ZeroSym || other == base)
      continue;
    auto bound = state.zone.bound(base, other);
    if (!bound)
      continue;
    const core::SymInfo &info = heap.info(state, other);
    if (info.type != core::SymInfo::Type::Int || !info.intType)
      continue;
    __int128 otherMax = typeMax(*info.intType);
    if (auto upper = state.zone.upper(other); upper && *upper < otherMax)
      otherMax = *upper;
    if (otherMax + *bound + delta <= typeMax(type))
      return true;
  }
  return false;
}

void Transfer::reportInteger(core::IntegerError error, const Expr &at) {
  if (!run.isPublishing() || error == core::IntegerError::None ||
      error == core::IntegerError::IncompatibleTypes)
    return;
  core::Diagnostic diagnostic;
  diagnostic.id = core::diag::InvalidIntegerOperation;
  diagnostic.severity = core::Severity::Error;
  diagnostic.message =
      "invalid integer operation: " + std::string(core::toString(error));
  diagnostic.location =
      toCoreLocation(context.getSourceManager(), at.getBeginLoc());
  run.report(std::move(diagnostic), core::Certainty::Definite, &at,
             std::nullopt);
}

/// RFC 0017 §3: the values of `left op right` computed without wrap-around
/// that `type` can hold (a checked operation's result when it did not
/// overflow); none when there are none.
static std::optional<core::IntegerRange>
exactRange(core::IntegerOp op, std::pair<__int128, __int128> left,
           std::pair<__int128, __int128> right, const core::IntegerType &type) {
  __int128 lo = 0;
  __int128 hi = 0;
  switch (op) {
  case core::IntegerOp::Add:
    lo = left.first + right.first;
    hi = left.second + right.second;
    break;
  case core::IntegerOp::Subtract:
    lo = left.first - right.second;
    hi = left.second - right.first;
    break;
  case core::IntegerOp::Multiply: {
    bool first = true;
    for (__int128 x : {left.first, left.second})
      for (__int128 y : {right.first, right.second}) {
        __int128 product = 0;
        // (Beyond 128 bits, saturated: the type's limits clip it below.)
        if (__builtin_mul_overflow(x, y, &product)) {
          const auto most =
              static_cast<__int128>(~static_cast<unsigned __int128>(0) >> 1U);
          product = (x < 0) != (y < 0) ? -most - 1 : most;
        }
        lo = first ? product : std::min(lo, product);
        hi = first ? product : std::max(hi, product);
        first = false;
      }
    break;
  }
  default:
    return std::nullopt;
  }
  lo = std::max(lo, typeMin(type));
  hi = std::min(hi, typeMax(type));
  if (lo > hi)
    return std::nullopt;
  return core::IntegerRange::between(
      core::IntegerValue::ofBits(type, static_cast<std::uint64_t>(lo)),
      core::IntegerValue::ofBits(type, static_cast<std::uint64_t>(hi)));
}

std::optional<core::Sym> Transfer::checkedProduct(core::Sym left,
                                                  core::Sym right,
                                                  QualType type,
                                                  const Expr &at) {
  auto integer = integerType(type);
  if (integer && !integer->isBoolean && integer->width <= 64 &&
      !exactRange(core::IntegerOp::Multiply, boundsOf(state, left, *integer),
                  boundsOf(state, right, *integer), *integer))
    return std::nullopt;
  return arithmetic(BO_Mul, left, right, type, at);
}

core::Sym Transfer::checkedArithmetic(const CallExpr &call, core::IntegerOp op,
                                      const std::vector<core::Sym> &args,
                                      core::Sym result) {
  QualType outType = call.getArg(2)->getType();
  if (!outType->isPointerType())
    return result;
  outType = outType->getPointeeType();
  auto a = integerType(call.getArg(0)->getType());
  auto b = integerType(call.getArg(1)->getType());
  auto destination = integerType(outType);
  if (!a || !b || !destination || a->width > 64 || b->width > 64 ||
      destination->width > 64 || destination->isBoolean)
    return result;
  // Computed in infinite precision, then stored converted, with the flag
  // saying whether the destination holds it.
  core::CheckedIntegerRange checked =
      core::evaluateCheckedInteger(op, rangeOf(state, args[0], *a),
                                   rangeOf(state, args[1], *b), *destination);
  core::Sym stored = unknownValue(outType);
  boundBy(heap, state, stored, checked.values);
  const core::SymInfo &out = heap.info(state, args[2]);
  if (out.type == core::SymInfo::Type::Pointer && !out.top) {
    Address address;
    address.targets = out.targets;
    store(address, stored, outType, nullptr);
  }
  core::Sym flag = unknownValue(call.getType());
  state.zone.addRange(flag, 0, 1);
  if (auto overflow = checked.overflow.constant())
    state.zone.addRange(flag, static_cast<std::int64_t>(overflow->bits),
                        static_cast<std::int64_t>(overflow->bits));
  // RFC 0017 §3: when the flag is false the stored value is the exact
  // result, within the operands' mathematical range.
  if (auto exact = exactRange(op, boundsOf(state, args[0], *a),
                              boundsOf(state, args[1], *b), *destination);
      exact && !exact->isFull()) {
    core::PendingCase success;
    success.kind = core::PendingCase::Kind::Bound;
    success.classes = {"zero"};
    success.subject = stored;
    success.bound = *exact;
    heap.infoMut(state, flag).pending.push_back(std::move(success));
  }
  (void)result;
  return flag;
}

core::Sym Transfer::convertInteger(core::Sym value, QualType from,
                                   QualType to) {
  auto target = integerType(to);
  if (!target)
    return unknownValue(to);
  return convertInteger(value, from, *target, to);
}

core::Sym Transfer::convertInteger(core::Sym value, QualType from,
                                   const core::IntegerType &targetType,
                                   QualType to) {
  auto source = integerType(from);
  const core::IntegerType *target = &targetType;
  if (!source || source->width > 64 || target->width > 64)
    return unknownValue(to);
  auto [lo, hi] = boundsOf(state, value, *source);
  const core::SymInfo &info = heap.info(state, value);
  if (!target->isBoolean && lo >= typeMin(*target) && hi <= typeMax(*target) &&
      info.type != core::SymInfo::Type::Pointer) {
    // The value is unchanged: keep the symbol and its relations.
    core::SymInfo &kept = heap.infoMut(state, value);
    if (kept.type == core::SymInfo::Type::Unknown) {
      kept.type = core::SymInfo::Type::Int;
      kept.intType = *target;
    }
    return value;
  }
  core::Sym pointerBehind = info.pointerBehind;
  core::Sym out = unknownValue(to);
  boundBy(heap, state, out, rangeOf(state, value, *source).converted(*target));
  // Every value wraps by the same multiple of 2^width: the result is the
  // value moved by it (RFC 0017, `(unsigned char)256` is `0`).
  if (!target->isBoolean)
    if (auto shift = wrapShift(lo, hi, *target); shift && fitsInt64(*shift)) {
      state.zone.addEq(out, value, static_cast<std::int64_t>(-*shift));
      core::Term base = termOf(value);
      if (base.known && !base.isConstant())
        heap.infoMut(state, out).linear =
            base.plusConstant(static_cast<std::int64_t>(-*shift));
    }
  if (pointerBehind != core::ZeroSym)
    heap.infoMut(state, out).pointerBehind = pointerBehind;
  return out;
}

/// RFC 0017: whether no other member of the bit-field's record shares a
/// byte with it, so that the cell at its first byte is its own.
static bool bitFieldAlone(const ASTContext &context, const FieldDecl &field) {
  const RecordDecl *record = field.getParent();
  if (record == nullptr || record->isUnion() ||
      !record->isCompleteDefinition() || field.isZeroLengthBitField())
    return false;
  const ASTRecordLayout &layout = context.getASTRecordLayout(record);
  const std::uint64_t charWidth = context.getCharWidth();
  auto bytes = [&](const FieldDecl &member)
      -> std::optional<std::pair<std::uint64_t, std::uint64_t>> {
    std::uint64_t start = layout.getFieldOffset(member.getFieldIndex());
    std::uint64_t width = 0;
    if (member.isBitField())
      width = member.getBitWidthValue();
    else if (!member.getType()->isIncompleteType())
      width = context.getTypeSize(member.getType());
    if (width == 0)
      return std::nullopt;
    return std::make_pair(start / charWidth, (start + width - 1) / charWidth);
  };
  auto own = bytes(field);
  if (!own)
    return false;
  for (const FieldDecl *other : record->fields()) {
    if (other == &field)
      continue;
    auto theirs = bytes(*other);
    if (theirs && theirs->first <= own->second && own->first <= theirs->second)
      return false;
  }
  return true;
}

std::optional<core::IntegerType>
Transfer::bitFieldType(const FieldDecl &field) const {
  auto type = integerType(field.getType());
  if (!type || type->isBoolean || !field.isBitField() ||
      field.getBitWidth()->isValueDependent())
    return std::nullopt;
  const unsigned width = field.getBitWidthValue();
  if (width == 0 || width > type->width)
    return std::nullopt;
  type->width = width;
  return type;
}

core::Sym Transfer::loadBitField(const Address &address, QualType type,
                                 const FieldDecl &field, const Expr *at) {
  auto width = bitFieldType(field);
  auto integer = integerType(type);
  if (!width || !integer)
    return load(address, type, at);
  if (!bitFieldAlone(context, field)) {
    core::Sym value = unknownValue(type);
    boundBy(heap, state, value,
            core::IntegerRange::full(*width).converted(*integer));
    return value;
  }
  core::Sym loaded = load(address, type, at);
  if (heap.info(state, loaded).type != core::SymInfo::Type::Int)
    return loaded;
  // Every value of the cell is one of the width's (a store converts).
  auto [lo, hi] = boundsOf(state, loaded, *integer);
  if (lo >= typeMin(*width) && hi <= typeMax(*width))
    return loaded;
  return convertInteger(loaded, type, *width, type);
}

core::Sym Transfer::storeBitField(const Address &address, core::Sym value,
                                  QualType type, const FieldDecl &field,
                                  const Expr *at) {
  auto width = bitFieldType(field);
  if (!width || !integerType(type)) {
    store(address, value, type, at);
    return value;
  }
  core::Sym stored = convertInteger(value, type, *width, type);
  if (bitFieldAlone(context, field)) {
    store(address, stored, type, at);
    return stored;
  }
  // The bytes it shares hold another member's bits too: forget them.
  const std::uint64_t charWidth = context.getCharWidth();
  const std::uint64_t start = context.getASTRecordLayout(field.getParent())
                                  .getFieldOffset(field.getFieldIndex());
  const auto bytes = static_cast<std::int64_t>(
      ((start % charWidth) + field.getBitWidthValue() + charWidth - 1) /
      charWidth);
  for (const core::Target &target : address.targets)
    if (target.offset.isConstant())
      heap.forgetCells(state, target.object, target.offset.constant, bytes);
    else
      heap.forgetCells(state, target.object, 0, std::nullopt);
  return stored;
}

core::Sym Transfer::arithmetic(BinaryOperatorKind kind, core::Sym left,
                               core::Sym right, QualType type, const Expr &at) {
  auto integer = integerType(type);
  auto op = integerOpOf(kind);
  if (!integer || !op || integer->width > 64 || integer->isBoolean)
    return unknownValue(type);
  // §4.1: symbols are immutable, so the same operation on the same values
  // is the same value (`malloc(rows * cols)` and `p[rows * cols]`).
  auto c1 = state.zone.constant(left);
  auto c2 = state.zone.constant(right);
  core::SymDefinition definition{.op = *op, .left = left, .right = right};
  if (c2) {
    definition.right = core::ZeroSym;
    definition.constant = c2;
  } else if (c1 && commutative(*op)) {
    definition.left = right;
    definition.right = core::ZeroSym;
    definition.constant = c1;
  } else if (commutative(*op) && definition.right < definition.left) {
    std::swap(definition.left, definition.right);
  }
  core::Handle ctype = typeHandle(type);
  FunctionRun::OperationKey key{*op, definition.left, definition.right,
                                definition.constant, ctype};
  if (auto it = run.operations.find(key); it != run.operations.end())
    if (const core::SymInfo *same = state.syms.find(it->second);
        same != nullptr && same->defined &&
        same->defined->sameOperation(definition) && same->ctype == ctype)
      return it->second;

  const bool wrapSigned = context.getLangOpts().isSignedOverflowDefined();
  const bool shift =
      *op == core::IntegerOp::ShiftLeft || *op == core::IntegerOp::ShiftRight;
  core::IntegerType rhsType = *integer;
  if (shift)
    rhsType =
        heap.info(state, right)
            .intType.value_or(core::IntegerType{.width = 64, .isSigned = true});
  core::IntegerRangeEvaluation evaluation =
      core::evaluateInteger(*op, rangeOf(state, left, *integer),
                            rangeOf(state, right, rhsType), wrapSigned);
  if (evaluation.alwaysInvalid)
    reportInteger(evaluation.error, at);
  core::Sym result = unknownValue(type);
  if (!evaluation.mayBeInvalid)
    boundBy(heap, state, result, evaluation.values);

  auto [lo1, hi1] = boundsOf(state, left, *integer);
  auto [lo2, hi2] = boundsOf(state, right, rhsType);
  // Whether the C result is the mathematical one for every value.
  bool exact = false;
  auto fits = [&](__int128 lo, __int128 hi) {
    return lo >= typeMin(*integer) && hi <= typeMax(*integer);
  };
  switch (*op) {
  case core::IntegerOp::Add:
    exact = fits(lo1 + lo2, hi1 + hi2);
    break;
  case core::IntegerOp::Subtract:
    exact = fits(lo1 - hi2, hi1 - lo2);
    break;
  case core::IntegerOp::Multiply: {
    // (Bounds of 64-bit types: a product may not fit 128 bits either.)
    bool wraps = false;
    auto product = [&](__int128 x, __int128 y) {
      __int128 result = 0;
      wraps = __builtin_mul_overflow(x, y, &result) || wraps;
      return result;
    };
    const std::array<__int128, 4> products = {
        product(lo1, lo2), product(lo1, hi2), product(hi1, lo2),
        product(hi1, hi2)};
    exact = !wraps && fits(*std::ranges::min_element(products),
                           *std::ranges::max_element(products));
    break;
  }
  case core::IntegerOp::Divide:
    exact = c2 && *c2 > 0 && lo1 >= 0;
    break;
  default:
    break;
  }

  // RFC 0017 §3: a range guard (`n <= INT_MAX / m`) keeps a product of
  // non-negative operands from wrapping.
  if (*op == core::IntegerOp::Multiply && !exact && lo1 >= 0 && lo2 >= 0) {
    auto guard = [&](core::Sym x, core::Sym y) -> std::optional<__int128> {
      const core::SymInfo *info = state.syms.find(x);
      if (info == nullptr || !info->productAtMost ||
          info->productAtMost->first != y)
        return std::nullopt;
      return static_cast<__int128>(info->productAtMost->second);
    };
    auto bound = guard(left, right);
    if (!bound)
      bound = guard(right, left);
    if (bound) {
      __int128 top = std::min(*bound, typeMax(*integer));
      exact = true;
      if (fitsInt64(top))
        state.zone.addRange(result, 0, static_cast<std::int64_t>(top));
    }
  }

  // `a = b + c` with a constant `c` records `a - b = c` (§4.4), moved by the
  // wrap when every value wraps alike (RFC 0017).
  const bool additive =
      *op == core::IntegerOp::Add || *op == core::IntegerOp::Subtract;
  if (additive && !evaluation.alwaysInvalid &&
      (c2 || (c1 && *op == core::IntegerOp::Add))) {
    core::Sym base = c2 ? left : right;
    auto delta = static_cast<__int128>(c2 ? *c2 : *c1);
    if (c2 && *op != core::IntegerOp::Add)
      delta = -delta;
    auto [lo, hi] = boundsOf(state, base, *integer);
    std::optional<__int128> moved = wrapShift(lo + delta, hi + delta, *integer);
    if (moved && *moved != 0 && integer->isSigned && !wrapSigned)
      moved.reset(); // signed overflow: undefined, no relation
    // Below another value of the type by at least `delta`: no wrap, signed
    // or not (a loop's induction after widening dropped its bounds).
    if (!moved && delta > 0 && belowAnother(heap, state, base, delta, *integer))
      moved = 0;
    if (moved && fitsInt64(delta - *moved)) {
      auto total = static_cast<std::int64_t>(delta - *moved);
      state.zone.addEq(result, base, total);
      core::Term term = termOf(base);
      if (term.known && !term.isConstant())
        heap.infoMut(state, result).linear = term.plusConstant(total);
      exact = exact || *moved == 0;
    }
  }
  // `a = b * k` in an unsigned type that may wrap: `b * k` reduced, never
  // more than it (an allocation of it ends there at the latest).
  if (*op == core::IntegerOp::Multiply && !exact && (c1 || c2) &&
      !integer->isSigned && !integer->isBoolean && integer->width == 64) {
    std::int64_t k = c2 ? *c2 : *c1;
    core::Term base = termOf(c2 ? left : right);
    if (k > 0 && base.known && !base.isConstant() && base.scale > 0 &&
        base.constant >= 0) {
      __int128 scale = static_cast<__int128>(base.scale) * k;
      __int128 constant = static_cast<__int128>(base.constant) * k;
      if (fitsInt64(scale) && fitsInt64(constant))
        heap.infoMut(state, result).unwrapped =
            core::Term::ofSym(base.var, static_cast<std::int64_t>(scale),
                              static_cast<std::int64_t>(constant));
    }
  }
  // `a = b * k` without wrap-around is linear in `b`.
  if (*op == core::IntegerOp::Multiply && exact && (c1 || c2)) {
    std::int64_t k = c2 ? *c2 : *c1;
    core::Term base = termOf(c2 ? left : right);
    if (base.known) {
      __int128 scale = static_cast<__int128>(base.scale) * k;
      __int128 constant = static_cast<__int128>(base.constant) * k;
      if (fitsInt64(scale) && fitsInt64(constant))
        heap.infoMut(state, result).linear =
            base.isConstant()
                ? core::Term::of(static_cast<std::int64_t>(constant))
                : core::Term::ofSym(base.var, static_cast<std::int64_t>(scale),
                                    static_cast<std::int64_t>(constant));
    }
  }
  definition.exact = exact;
  if (definition.left == left)
    if (auto k = rangeOf(state, left, *integer).constant())
      definition.leftValue = *k;
  heap.infoMut(state, result).defined = definition;
  run.operations[key] = result;
  return result;
}

core::Sym Transfer::compare(BinaryOperatorKind kind, core::Sym left,
                            core::Sym right, QualType operandType) {
  core::Sym result = unknownValue(context.IntTy);
  state.zone.addRange(result, 0, 1);
  core::Condition condition;
  switch (kind) {
  case BO_EQ:
    condition.op = core::Condition::Op::Eq;
    break;
  case BO_NE:
    condition.op = core::Condition::Op::Ne;
    break;
  case BO_LT:
    condition.op = core::Condition::Op::Lt;
    break;
  case BO_LE:
    condition.op = core::Condition::Op::Le;
    break;
  case BO_GT:
    condition.op = core::Condition::Op::Gt;
    break;
  case BO_GE:
    condition.op = core::Condition::Op::Ge;
    break;
  default:
    return result;
  }
  condition.left = left;
  // The same value on both sides.
  if (left == right && (kind == BO_EQ || kind == BO_NE || kind == BO_LE ||
                        kind == BO_GE || kind == BO_LT || kind == BO_GT)) {
    bool truth = kind == BO_EQ || kind == BO_LE || kind == BO_GE;
    state.zone.addRange(result, truth ? 1 : 0, truth ? 1 : 0);
    return result;
  }
  if (auto c = state.zone.constant(right); c && !operandType->isPointerType()) {
    condition.rightIsConstant = true;
    condition.constant = *c;
  } else if (operandType->isPointerType() &&
             heap.info(state, right).null == core::PointerNull::Null) {
    condition.rightIsConstant = true;
    condition.constant = 0;
  } else if (operandType->isPointerType() &&
             heap.info(state, left).null == core::PointerNull::Null) {
    // `NULL == p`: swap.
    condition.left = right;
    condition.rightIsConstant = true;
    condition.constant = 0;
  } else {
    condition.right = right;
  }
  condition.isUnsigned = operandType->isUnsignedIntegerType();
  heap.infoMut(state, result).condition = condition;
  // Two pointers into known objects at known offsets.
  if (operandType->isPointerType() && !condition.rightIsConstant &&
      (kind == BO_EQ || kind == BO_NE)) {
    const core::SymInfo &a = heap.info(state, left);
    const core::SymInfo &b = heap.info(state, right);
    if (a.type == core::SymInfo::Type::Pointer &&
        b.type == core::SymInfo::Type::Pointer && !a.top && !b.top &&
        a.null == core::PointerNull::NonNull &&
        b.null == core::PointerNull::NonNull && a.targets.size() == 1 &&
        b.targets.size() == 1 && a.targets[0].offset.isConstant() &&
        b.targets[0].offset.isConstant()) {
      std::optional<bool> equal;
      if (a.targets[0].object == b.targets[0].object &&
          run.table().info(a.targets[0].object).singular)
        equal = a.targets[0].offset.constant == b.targets[0].offset.constant;
      else if (!heap.mayOverlap(state, a.targets[0].object,
                                b.targets[0].object))
        equal = false;
      if (equal) {
        bool truth = kind == BO_EQ ? *equal : !*equal;
        state.zone.addRange(result, truth ? 1 : 0, truth ? 1 : 0);
        return result;
      }
    }
  }
  // Decide it now when the facts do.
  core::HeapState whenTrue = state;
  core::HeapState whenFalse = state;
  bool canTrue = refine(run, whenTrue, result, true);
  bool canFalse = refine(run, whenFalse, result, false);
  if (canTrue && !canFalse)
    state.zone.addRange(result, 1, 1);
  else if (!canTrue && canFalse)
    state.zone.addRange(result, 0, 0);
  return result;
}

core::Sym Transfer::evaluateAssign(const BinaryOperator &op) {
  const Expr &lhs = *op.getLHS();
  QualType type = lhs.getType();
  Address address = addressOf(lhs);
  core::Sym value = core::ZeroSym;
  if (op.getOpcode() == BO_Assign) {
    if (type->isRecordType()) {
      // A record assignment copies the cells.
      const Expr *rhs = op.getRHS()->IgnoreParens();
      if (const auto *cast = dyn_cast<ImplicitCastExpr>(rhs);
          cast != nullptr && cast->getCastKind() == CK_LValueToRValue)
        rhs = cast->getSubExpr();
      if (rhs->isGLValue() && copyRecord(address, addressOf(*rhs), type))
        return unknownValue(type);
      ExprResult source = evaluate(*op.getRHS());
      if (source.address && source.address->targets.size() == 1 &&
          address.targets.size() == 1 &&
          source.address->targets[0].offset.isConstant() &&
          address.targets[0].offset.isConstant() &&
          address.targets[0].offset.constant == 0 &&
          source.address->targets[0].offset.constant == 0 &&
          state.objects.contains(source.address->targets[0].object)) {
        const core::ObjectState from =
            state.objects.at(source.address->targets[0].object);
        core::ObjectState &copy = heap.ensure(state, address.targets[0].object);
        copy.cells = from.cells;
        copy.havocked = from.havocked;
        copy.forgotten = from.forgotten;
        copy.mayForgotten = from.mayForgotten;
        copy.zeroed = from.zeroed;
        copy.uninitialised = from.uninitialised;
      } else {
        for (const core::Target &target : address.targets) {
          heap.ensure(state, target.object).cells = {};
          state.objects.at(target.object).havocked = true;
        }
      }
      return unknownValue(type);
    }
    value = valueOf(*op.getRHS());
    // RFC 0004: an assignment to a place declared with a safe kind.
    const Expr *target = lhs.IgnoreParenImpCasts();
    const ValueDecl *declared = nullptr;
    if (const auto *ref = dyn_cast<DeclRefExpr>(target))
      declared = ref->getDecl();
    else if (const auto *member = dyn_cast<MemberExpr>(target))
      declared = member->getMemberDecl();
    if (declared != nullptr && type->isPointerType())
      value =
          launder(value, getAnnotations(*declared), spell(lhs), *op.getRHS());
    checkWriteAnnotation(address, op);
  } else {
    checkWriteAnnotation(address, op);
    const FieldDecl *bitField = lhs.getSourceBitField();
    core::Sym old = bitField != nullptr
                        ? loadBitField(address, type, *bitField, &op)
                        : load(address, type, &op);
    core::Sym rhs = valueOf(*op.getRHS());
    BinaryOperatorKind kind =
        BinaryOperator::getOpForCompoundAssignment(op.getOpcode());
    if (type->isPointerType() && (kind == BO_Add || kind == BO_Sub)) {
      std::int64_t size = sizeOf(type->getPointeeType()).value_or(1);
      value = pointerAdd(old, rhs, size, kind == BO_Sub, op);
    } else if (const auto *compound = dyn_cast<CompoundAssignOperator>(&op);
               compound != nullptr && type->isIntegerType() &&
               compound->getComputationLHSType()->isIntegerType() &&
               compound->getComputationResultType()->isIntegerType()) {
      // RFC 0017: computed in the promoted type, then converted back.
      QualType computation = compound->getComputationLHSType();
      QualType result = compound->getComputationResultType();
      core::Sym promoted =
          computation == type ? old : convertInteger(old, type, computation);
      value = arithmetic(kind, promoted, rhs, result, op);
      if (result != type)
        value = convertInteger(value, result, type);
    } else {
      value = arithmetic(kind, old, rhs, type, op);
    }
  }
  if (const FieldDecl *bitField = lhs.getSourceBitField())
    value = storeBitField(address, value, type, *bitField, &op);
  else
    store(address, value, type, &op);
  if (const auto *member = dyn_cast<MemberExpr>(lhs.IgnoreParenImpCasts()))
    checkSizedFieldStore(*member, address);
  return value;
}

std::optional<core::Sym> Transfer::truthValue(const Expr &expr) {
  const Expr *bare = expr.IgnoreParenImpCasts();
  auto truth = [&](core::Condition condition) {
    core::Sym result = unknownValue(context.IntTy);
    state.zone.addRange(result, 0, 1);
    heap.infoMut(state, result).condition = condition;
    return result;
  };
  if (const auto *op = dyn_cast<BinaryOperator>(bare)) {
    if (op->isComparisonOp())
      return comparison(*op);
    if (op->isLogicalOp()) {
      auto left = truthValue(*op->getLHS());
      auto right = left ? truthValue(*op->getRHS()) : std::nullopt;
      if (!left || !right)
        return std::nullopt;
      return truth(core::Condition{.op = op->getOpcode() == BO_LAnd
                                             ? core::Condition::Op::And
                                             : core::Condition::Op::Or,
                                   .left = *left,
                                   .right = *right});
    }
    return std::nullopt;
  }
  if (const auto *op = dyn_cast<UnaryOperator>(bare);
      op != nullptr && op->getOpcode() == UO_LNot) {
    auto inner = truthValue(*op->getSubExpr());
    if (!inner)
      return std::nullopt;
    return truth(core::Condition{.op = core::Condition::Op::Eq,
                                 .left = *inner,
                                 .rightIsConstant = true,
                                 .constant = 0});
  }
  QualType type = bare->getType();
  if (type->isPointerType() || type->isIntegerType())
    return truth(core::Condition{.op = core::Condition::Op::NonZero,
                                 .left = valueOf(*bare)});
  return std::nullopt;
}

core::Sym Transfer::evaluateBinary(const BinaryOperator &op) {
  BinaryOperatorKind kind = op.getOpcode();
  if (op.isAssignmentOp())
    return evaluateAssign(op);
  if (kind == BO_Comma) {
    (void)valueOf(*op.getLHS());
    return valueOf(*op.getRHS());
  }
  if (kind == BO_LAnd || kind == BO_LOr) {
    // Used as a value (`__builtin_expect(p && n, 1)`, `!(p || n)`): what
    // its truth says of both operands, which a side-effect-free operator
    // lets us evaluate again here (§5.1).
    if (!op.HasSideEffects(context))
      if (auto truth = truthValue(op))
        return *truth;
    core::Sym result = unknownValue(op.getType());
    state.zone.addRange(result, 0, 1);
    return result;
  }
  const Expr &lhs = *op.getLHS();
  const Expr &rhs = *op.getRHS();
  core::Sym left = valueOf(lhs);
  core::Sym right = valueOf(rhs);
  if (op.isComparisonOp())
    return compare(kind, left, right, lhs.getType());
  QualType lt = lhs.getType();
  QualType rt = rhs.getType();
  if ((kind == BO_Add || kind == BO_Sub) && lt->isPointerType() &&
      rt->isIntegerType()) {
    std::int64_t size = sizeOf(lt->getPointeeType()).value_or(1);
    return pointerAdd(left, right, size, kind == BO_Sub, op);
  }
  if (kind == BO_Add && rt->isPointerType() && lt->isIntegerType()) {
    std::int64_t size = sizeOf(rt->getPointeeType()).value_or(1);
    return pointerAdd(right, left, size, false, op);
  }
  if (kind == BO_Sub && lt->isPointerType() && rt->isPointerType()) {
    // The difference of two pointers into one object.
    const core::SymInfo &a = heap.info(state, left);
    const core::SymInfo &b = heap.info(state, right);
    core::Sym result = unknownValue(op.getType());
    std::int64_t size = sizeOf(lt->getPointeeType()).value_or(1);
    if (a.targets.size() == 1 && b.targets.size() == 1 &&
        a.targets[0].object == b.targets[0].object &&
        a.targets[0].offset.isConstant() && b.targets[0].offset.isConstant() &&
        size > 0) {
      std::int64_t diff =
          (a.targets[0].offset.constant - b.targets[0].offset.constant) / size;
      state.zone.addRange(result, diff, diff);
    }
    return result;
  }
  return arithmetic(kind, left, right, op.getType(), op);
}

//===----------------------------------------------------------------------===//
// Refinement on branches
//===----------------------------------------------------------------------===//

/// The result classes a value may still have.
static std::set<std::string> possibleClasses(const core::Heap &heap,
                                             const core::HeapState &state,
                                             core::Sym sym) {
  const core::SymInfo &info = heap.info(state, sym);
  if (info.type == core::SymInfo::Type::Pointer) {
    switch (info.null) {
    case core::PointerNull::Null:
      return {"null"};
    case core::PointerNull::NonNull:
      return {"nonnull"};
    case core::PointerNull::Maybe:
      return {"null", "nonnull"};
    }
  }
  std::set<std::string> out;
  auto lo = state.zone.lower(sym);
  auto hi = state.zone.upper(sym);
  if (!lo || *lo < 0)
    out.insert("negative");
  if ((!lo || *lo <= 0) && (!hi || *hi >= 0) && !info.nonZero)
    out.insert("zero");
  if (!hi || *hi > 0)
    out.insert("positive");
  return out;
}

/// RFC 0031 §6.3: every cell and carried expression holding `from` holds
/// `to` instead (a join symbol a test resolved to one of its members).
static void replaceHeld(core::HeapState &state, core::Sym from, core::Sym to) {
  if (from == to || !state.syms.contains(to))
    return;
  std::vector<std::pair<core::ObjectId, core::CellKey>> cells;
  for (const auto &[id, object] : state.objects)
    for (const auto &[key, sym] : object.cells)
      if (sym == from)
        cells.emplace_back(id, key);
  for (const auto &[id, key] : cells)
    state.objects.at(id).cells.set(key, to);
  std::vector<core::Handle> exprs;
  for (const auto &[handle, sym] : state.exprs)
    if (sym == from)
      exprs.push_back(handle);
  for (core::Handle handle : exprs)
    state.exprs.set(handle, to);
}

/// Applies the pending cases of `sym` a test has decided (RFC 0030 §9.1):
/// a selected release is made again with its own record; one the test rules
/// out is undone, unless another selected case releases the same value.
static void resolvePending(FunctionRun &run, core::HeapState &state,
                           core::Sym sym) {
  const core::Heap &heap = run.domain();
  const core::SymInfo *info = state.syms.find(sym);
  if (info == nullptr || info->pending.empty())
    return;
  std::set<std::string> possible = possibleClasses(heap, state, sym);
  std::vector<core::PendingCase> kept;
  std::vector<core::PendingCase> selected;
  std::vector<core::PendingCase> excluded;
  for (const core::PendingCase &pending : info->pending) {
    bool all = true;
    bool none = true;
    for (const std::string &c : possible) {
      bool in = std::ranges::find(pending.classes, c) != pending.classes.end();
      all = all && in;
      none = none && !in;
    }
    // A release whose parameter test the path has decided the other way
    // never happened here.
    if (all && pending.kind == core::PendingCase::Kind::Release &&
        pending.argumentZero &&
        state.syms.contains(pending.argumentZero->first))
      if (auto zero = isZeroValue(heap, state, pending.argumentZero->first);
          zero && *zero != pending.argumentZero->second) {
        excluded.push_back(pending);
        continue;
      }
    if (all)
      selected.push_back(pending);
    else if (none)
      excluded.push_back(pending);
    else
      kept.push_back(pending);
  }
  auto releasedBySelected = [&](core::Sym subject) {
    auto releases = [&](const core::PendingCase &pending) {
      return pending.subject == subject &&
             pending.kind == core::PendingCase::Kind::Release;
    };
    return std::ranges::any_of(selected, releases) ||
           std::ranges::any_of(kept, releases);
  };
  for (const core::PendingCase &pending : excluded)
    if (pending.kind == core::PendingCase::Kind::Stored)
      replaceHeld(state, pending.subject, pending.previous);
  for (const core::PendingCase &pending : excluded) {
    if (pending.kind != core::PendingCase::Kind::Release ||
        !state.syms.contains(pending.subject) ||
        releasedBySelected(pending.subject))
      continue;
    core::SymInfo &subject = heap.infoMut(state, pending.subject);
    if (subject.release && subject.release->where == pending.record.where)
      subject.release.reset();
    std::vector<core::ObjectId> targets;
    targets.reserve(subject.targets.size());
    for (const core::Target &target : subject.targets)
      targets.push_back(target.object);
    for (core::ObjectId id : targets) {
      if (!state.objects.contains(id))
        continue;
      core::ObjectState &object = state.objects.at(id);
      if (!object.record || object.record->where != pending.record.where)
        continue;
      object.life = core::Life::Live;
      object.record.reset();
      object.effectReleased = false;
      object.effectMayReleased = false;
    }
  }
  for (const core::PendingCase &pending : selected) {
    if (!state.syms.contains(pending.subject))
      continue;
    if (pending.kind == core::PendingCase::Kind::Bound) {
      if (pending.bound)
        boundBy(heap, state, pending.subject, *pending.bound);
      continue;
    }
    if (pending.kind == core::PendingCase::Kind::NonNull) {
      heap.infoMut(state, pending.subject).null = core::PointerNull::NonNull;
      continue;
    }
    if (pending.kind == core::PendingCase::Kind::Stored) {
      replaceHeld(state, pending.subject, pending.stored);
      continue;
    }
    if (pending.kind == core::PendingCase::Kind::Absent) {
      // The store did not happen on this path: its new objects own nothing.
      std::vector<core::ObjectId> targets;
      for (const core::Target &target :
           heap.info(state, pending.subject).targets)
        targets.push_back(target.object);
      for (core::ObjectId id : targets)
        if (state.objects.contains(id)) {
          state.objects.at(id).owned = false;
          state.objects.at(id).absent = true;
        }
      continue;
    }
    // Replace the conditional release by the selected one.
    core::SymInfo &subject = heap.infoMut(state, pending.subject);
    subject.release.reset();
    std::vector<core::ObjectId> targets;
    targets.reserve(subject.targets.size());
    for (const core::Target &target : subject.targets)
      targets.push_back(target.object);
    for (core::ObjectId id : targets)
      if (state.objects.contains(id) && state.objects.at(id).record &&
          state.objects.at(id).record->where == pending.record.where) {
        state.objects.at(id).record.reset();
        state.objects.at(id).life = core::Life::Live;
      }
    core::ReleaseRecord record = pending.record;
    // (Its parameter test still open: a possible release, which a later
    // test of the argument settles.)
    bool open = pending.argumentZero &&
                (!state.syms.contains(pending.argumentZero->first) ||
                 !isZeroValue(heap, state, pending.argumentZero->first));
    if (open) {
      record.conditional = true;
      record.allPaths = false;
    }
    heap.release(state, pending.subject, record);
    if (open && state.syms.contains(pending.argumentZero->first)) {
      core::PendingCase onArgument = pending;
      onArgument.argumentZero.reset();
      onArgument.classes =
          pending.argumentZero->second
              ? std::vector<std::string>{"zero"}
              : std::vector<std::string>{"positive", "negative"};
      onArgument.record = record;
      heap.infoMut(state, pending.argumentZero->first)
          .pending.push_back(std::move(onArgument));
    }
  }
  heap.infoMut(state, sym).pending = std::move(kept);
}

bool Transfer::refine(FunctionRun &run, core::HeapState &state,
                      core::Sym condition, bool truth) {
  bool feasible = refineCondition(run, state, condition, truth);
  if (!feasible)
    return false;
  // Pending cases of the values the condition tested.
  const core::SymInfo *info = state.syms.find(condition);
  std::vector<core::Sym> tested{condition};
  if (info != nullptr && info->condition) {
    tested.push_back(info->condition->left);
    if (!info->condition->rightIsConstant)
      tested.push_back(info->condition->right);
    const core::SymInfo *left = state.syms.find(info->condition->left);
    if (left != nullptr && left->pointerBehind != core::ZeroSym)
      tested.push_back(left->pointerBehind);
    if (left != nullptr && left->condition)
      tested.push_back(left->condition->left);
  }
  if (info != nullptr && info->pointerBehind != core::ZeroSym)
    tested.push_back(info->pointerBehind);
  for (core::Sym sym : tested)
    resolvePending(run, state, sym);
  // What the path now knows of the values cells held at entry.
  for (core::Sym sym : tested) {
    const core::SymInfo *value = state.syms.find(sym);
    if (value == nullptr || !value->entryOf)
      continue;
    auto zero = isZeroValue(run.domain(), state, sym);
    if (!zero)
      continue;
    core::EntryTest test{.object = value->entryOf->first,
                         .key = value->entryOf->second,
                         .zero = *zero};
    auto &tests = state.entryTests;
    std::erase_if(tests, [&](const core::EntryTest &other) {
      return other.object == test.object && other.key == test.key;
    });
    tests.insert(std::ranges::lower_bound(tests, test), test);
  }
  return true;
}

bool Transfer::refineSwitchEdge(FunctionRun &run, core::HeapState &state,
                                core::Sym value, QualType type,
                                const SwitchStmt &switchStmt,
                                const CFGBlock &successor, bool isDefault) {
  ASTContext &context = run.ast();
  if (!type->isIntegralOrEnumerationType() || state.unreachable)
    return !state.unreachable;
  // A label's value converted to the promoted type of the condition (C17
  // 6.8.4.2p5): `case -1` of an `unsigned long long` is its maximum.
  auto labelValue = [&](const Expr *label) -> std::optional<llvm::APSInt> {
    Expr::EvalResult result;
    if (label == nullptr || label->isValueDependent() ||
        !label->EvaluateAsInt(result, context))
      return std::nullopt;
    llvm::APSInt converted =
        result.Val.getInt().extOrTrunc(context.getIntWidth(type));
    converted.setIsUnsigned(type->isUnsignedIntegerOrEnumerationType());
    return converted;
  };
  Transfer transfer(run, state);
  auto holds = [&](BinaryOperatorKind kind, const llvm::APSInt &label) {
    core::Sym bound = transfer.constant(label, type);
    core::Sym test = transfer.compare(kind, value, bound, type);
    return refine(run, state, test, true);
  };
  if (isDefault) {
    // No label matched.
    for (const SwitchCase *label = switchStmt.getSwitchCaseList();
         label != nullptr; label = label->getNextSwitchCase()) {
      const auto *single = dyn_cast<CaseStmt>(label);
      if (single == nullptr || single->caseStmtIsGNURange())
        continue;
      if (auto v = labelValue(single->getLHS()); v && !holds(BO_NE, *v))
        return false;
    }
    return true;
  }
  const auto *label = dyn_cast_or_null<CaseStmt>(successor.getLabel());
  if (label == nullptr)
    return true;
  auto low = labelValue(label->getLHS());
  if (!low)
    return true;
  if (!label->caseStmtIsGNURange())
    return holds(BO_EQ, *low);
  auto high = labelValue(label->getRHS());
  return holds(BO_GE, *low) && (!high || holds(BO_LE, *high));
}

void Transfer::settlePending(FunctionRun &run, core::HeapState &state,
                             core::Sym sym) {
  resolvePending(run, state, sym);
}

bool Transfer::selectClass(FunctionRun &run, core::HeapState &state,
                           core::Sym sym, core::ResultClass resultClass) {
  const core::Heap &heap = run.domain();
  const core::SymInfo &info = heap.info(state, sym);
  bool feasible = true;
  switch (resultClass) {
  case core::ResultClass::Null:
  case core::ResultClass::NonNull:
    if (info.type != core::SymInfo::Type::Pointer)
      return false;
    feasible = refineCondition(run, state, sym,
                               resultClass == core::ResultClass::NonNull);
    break;
  case core::ResultClass::Zero:
    feasible = !info.nonZero && state.zone.addRange(sym, 0, 0);
    break;
  case core::ResultClass::Positive:
    feasible = state.zone.addRange(sym, 1, std::nullopt);
    break;
  case core::ResultClass::Negative:
    feasible = state.zone.addRange(sym, std::nullopt, -1);
    break;
  }
  // A comparison's truth refines its operands too (`return *out != NULL`).
  if (feasible && heap.info(state, sym).condition &&
      resultClass != core::ResultClass::Null &&
      resultClass != core::ResultClass::NonNull)
    feasible = refineCondition(run, state, sym,
                               resultClass != core::ResultClass::Zero);
  if (!feasible)
    return false;
  resolvePending(run, state, sym);
  return true;
}

bool Transfer::refineCondition(FunctionRun &run, core::HeapState &state,
                               core::Sym condition, bool truth) {
  // A condition its own operands reach again (as an operand evaluated
  // before a joining call once made, `Heap::join`'s `keepBelow`), or one
  // nested past this depth: nothing more is refined, which is always
  // sound.
  constexpr std::size_t MaxConditionDepth = 64;
  std::vector<core::Sym> &open = run.openConditions;
  if (open.size() >= MaxConditionDepth ||
      std::ranges::find(open, condition) != open.end())
    return true;
  struct Nested {
    std::vector<core::Sym> &open;
    Nested(std::vector<core::Sym> &at, core::Sym sym) : open(at) {
      open.push_back(sym);
    }
    Nested(const Nested &) = delete;
    Nested &operator=(const Nested &) = delete;
    ~Nested() { open.pop_back(); }
  } nested(open, condition);
  core::Heap &heap = run.domain();
  const core::SymInfo info = heap.info(state, condition);
  auto setNull = [&](core::Sym pointer, bool isNull) {
    core::SymInfo &p = heap.infoMut(state, pointer);
    if (isNull) {
      if (p.null == core::PointerNull::NonNull)
        return false;
      p.null = core::PointerNull::Null;
      // A failed allocation made no object on this path.
      // Nor the objects the same call created inside it (a summary's
      // `result->f` stores, RFC 0013).
      if (p.allocatorSource) {
        std::vector<core::ObjectId> work;
        work.reserve(p.targets.size());
        for (const core::Target &target : p.targets)
          work.push_back(target.object);
        std::set<core::ObjectId> seen;
        std::optional<core::Handle> site;
        while (!work.empty()) {
          core::ObjectId id = work.back();
          work.pop_back();
          if (!seen.insert(id).second || !state.objects.contains(id))
            continue;
          const core::ObjectKey &key = run.table().info(id).key;
          if (key.kind != core::ObjectKind::HeapRecent &&
              key.kind != core::ObjectKind::HeapOld)
            continue;
          if (!site)
            site = key.handle;
          if (key.handle != *site)
            continue;
          state.objects.at(id).owned = false;
          state.objects.at(id).absent = true;
          for (const auto &[cell, held] : state.objects.at(id).cells) {
            const core::SymInfo *value = state.syms.find(held);
            if (value != nullptr)
              for (const core::Target &target : value->targets)
                work.push_back(target.object);
          }
        }
      }
    } else {
      if (p.null == core::PointerNull::Null)
        return false;
      p.null = core::PointerNull::NonNull;
      heap.markNonNull(state, pointer);
    }
    // A pointer the value was converted from shares the test.
    return true;
  };
  auto nonZero = [&](core::Sym value, bool isNonZero) -> bool {
    const core::SymInfo &v = heap.info(state, value);
    if (v.type == core::SymInfo::Type::Pointer)
      return setNull(value, !isNonZero);
    if (v.pointerBehind != core::ZeroSym &&
        !setNull(v.pointerBehind, !isNonZero))
      return false;
    if (v.condition) {
      // A nested condition: `if (!(p == NULL))`.
      if (!refineCondition(run, state, value, isNonZero))
        return false;
    }
    if (isNonZero) {
      auto lo = state.zone.lower(value);
      auto hi = state.zone.upper(value);
      if (lo && hi && *lo == 0 && *hi == 0)
        return false;
      heap.infoMut(state, value).nonZero = true;
      if (lo && *lo == 0)
        return state.zone.addRange(value, 1, std::nullopt);
      if (hi && *hi == 0)
        return state.zone.addRange(value, std::nullopt, -1);
      return true;
    }
    if (heap.info(state, value).nonZero)
      return false;
    return state.zone.addRange(value, 0, 0);
  };
  if (state.unreachable)
    return false;
  if (!info.condition) {
    if (auto c = state.zone.constant(condition))
      return (*c != 0) == truth;
    return nonZero(condition, truth);
  }
  // A decided boolean.
  if (auto c = state.zone.constant(condition))
    if ((*c != 0) != truth)
      return false;
  core::Condition cond = *info.condition;
  using Op = core::Condition::Op;
  if (cond.op == Op::NonZero)
    return nonZero(cond.left, truth);
  // A true `&&` or a false `||` decides both operands; the other outcome
  // says nothing of either.
  if (cond.op == Op::And || cond.op == Op::Or) {
    if ((cond.op == Op::And) != truth)
      return true;
    return refineCondition(run, state, cond.left, truth) &&
           refineCondition(run, state, cond.right, truth);
  }
  Op op = cond.op;
  if (!truth) {
    switch (op) {
    case Op::Eq:
      op = Op::Ne;
      break;
    case Op::Ne:
      op = Op::Eq;
      break;
    case Op::Lt:
      op = Op::Ge;
      break;
    case Op::Le:
      op = Op::Gt;
      break;
    case Op::Gt:
      op = Op::Le;
      break;
    case Op::Ge:
      op = Op::Lt;
      break;
    case Op::NonZero:
    case Op::And:
    case Op::Or:
      break;
    }
  }
  const core::SymInfo &left = heap.info(state, cond.left);
  if (left.type == core::SymInfo::Type::Pointer) {
    if (cond.rightIsConstant && cond.constant == 0) {
      if (op == Op::Eq)
        return setNull(cond.left, true);
      if (op == Op::Ne)
        return setNull(cond.left, false);
    }
    // Two pointers into one object compare as their offsets (`p < buf +
    // n`, RFC 0031 §5.1).
    if (cond.rightIsConstant)
      return true;
    // RFC 0014: the path remembers how two pointers compared.
    if ((op == Op::Eq || op == Op::Ne) &&
        heap.info(state, cond.right).type == core::SymInfo::Type::Pointer &&
        !core::Heap::assumePointersEqual(state, cond.left, cond.right,
                                         op == Op::Eq))
      return false;
    // A pointer unequal to one naming exactly one place does not point
    // there (`if (line != sentinel) free(line);` never frees the sentinel).
    // A null other pointer names no place, unless it is the value at entry
    // that an entry object stands behind: null there, that object does not
    // exist, so nothing points to it either.
    if (op == Op::Ne) {
      const core::SymInfo &other = heap.info(state, cond.right);
      auto names = [&](const core::SymInfo &value) {
        if (value.null == core::PointerNull::NonNull)
          return true;
        return value.entryOf.has_value() && value.targets.size() == 1 &&
               run.table().info(value.targets[0].object).key.kind ==
                   core::ObjectKind::Entry &&
               value.targets[0].offset.isConstant() &&
               value.targets[0].offset.constant == 0;
      };
      if (other.type == core::SymInfo::Type::Pointer && !other.top &&
          names(other) && other.targets.size() == 1 &&
          other.targets[0].offset.isConstant() &&
          run.table().info(other.targets[0].object).singular && !left.top &&
          left.targets.size() > 1) {
        const core::Target place = other.targets[0];
        core::SymInfo &narrowed = heap.infoMut(state, cond.left);
        std::erase_if(narrowed.targets, [&](const core::Target &target) {
          return target.object == place.object && target.offset.isConstant() &&
                 target.offset.constant == place.offset.constant;
        });
        // (Nothing left: null, or no such path.)
        if (narrowed.targets.empty()) {
          if (narrowed.null == core::PointerNull::NonNull)
            return false;
          narrowed.null = core::PointerNull::Null;
        }
        return true;
      }
    }
    const core::SymInfo &right = heap.info(state, cond.right);
    if (right.type != core::SymInfo::Type::Pointer || left.top || right.top ||
        left.targets.size() != 1 || right.targets.size() != 1 ||
        left.targets[0].object != right.targets[0].object ||
        !run.table().info(left.targets[0].object).singular)
      return true;
    const core::Term l = left.targets[0].offset;
    const core::Term r = right.targets[0].offset;
    auto linear = [](const core::Term &term) {
      return term.known && (term.isConstant() || term.scale == 1);
    };
    if (!linear(l) || !linear(r))
      return true;
    core::Sym x = l.isConstant() ? core::ZeroSym : l.var;
    core::Sym y = r.isConstant() ? core::ZeroSym : r.var;
    // x + cl (op) y + cr
    std::int64_t cl = l.constant;
    std::int64_t cr = r.constant;
    bool ok = true;
    auto le = [&](core::Sym a, core::Sym b, __int128 c) {
      if (c < INT64_MIN || c > INT64_MAX)
        return;
      if (a == b) {
        ok = ok && c >= 0;
        return;
      }
      ok = ok && state.zone.addLE(a, b, static_cast<std::int64_t>(c));
    };
    __int128 gap = static_cast<__int128>(cr) - cl;
    switch (op) {
    case Op::Lt:
      le(x, y, gap - 1);
      break;
    case Op::Le:
      le(x, y, gap);
      break;
    case Op::Gt:
      le(y, x, -gap - 1);
      break;
    case Op::Ge:
      le(y, x, -gap);
      break;
    case Op::Eq:
      le(x, y, gap);
      le(y, x, -gap);
      break;
    case Op::Ne:
      if (x == y && gap == 0)
        return false;
      break;
    case Op::NonZero:
    case Op::And:
    case Op::Or:
      break;
    }
    return ok && !state.zone.isBottom();
  }
  bool ok = true;
  // §4.4: an operand whose interval the zone cannot hold (an unsigned
  // 64-bit value above `INT64_MAX`) is refined by the intervals.
  auto integerOp = [](Op relation) {
    switch (relation) {
    case Op::Eq:
      return core::IntegerOp::Equal;
    case Op::Ne:
      return core::IntegerOp::NotEqual;
    case Op::Lt:
      return core::IntegerOp::Less;
    case Op::Le:
      return core::IntegerOp::LessEqual;
    case Op::Gt:
      return core::IntegerOp::Greater;
    case Op::Ge:
    case Op::NonZero:
    case Op::And:
    case Op::Or:
      break;
    }
    return core::IntegerOp::GreaterEqual;
  };
  auto refineValues = [&](core::Sym x, Op relation,
                          const std::optional<core::IntegerRange> &other) {
    const core::SymInfo &xi = heap.info(state, x);
    if (!ok || !other || xi.type != core::SymInfo::Type::Int || !xi.intType ||
        other->type != *xi.intType || relation == Op::NonZero)
      return;
    core::IntegerRange range =
        rangeOf(state, x, *xi.intType).satisfying(integerOp(relation), *other);
    if (range.empty()) {
      ok = false;
      return;
    }
    boundBy(heap, state, x, range);
  };
  auto hasValues = [&](core::Sym x) {
    const core::SymInfo *xi = state.syms.find(x);
    return xi != nullptr && xi->values.has_value();
  };
  auto le = [&](core::Sym x, core::Sym y, std::int64_t c) {
    ok = ok && state.zone.addLE(x, y, c);
    // A linear operand (`i + 1 < n`) refines its base too.
    const core::SymInfo &xi = heap.info(state, x);
    // (A bound 64 bits cannot hold is not added.)
    std::int64_t shifted = 0;
    if (ok && xi.linear && xi.linear->scale == 1 &&
        xi.linear->var != core::ZeroSym &&
        !__builtin_sub_overflow(c, xi.linear->constant, &shifted))
      ok = state.zone.addLE(xi.linear->var, y, shifted);
    const core::SymInfo &yi = heap.info(state, y);
    if (ok && yi.linear && yi.linear->scale == 1 &&
        yi.linear->var != core::ZeroSym &&
        !__builtin_add_overflow(c, yi.linear->constant, &shifted))
      ok = state.zone.addLE(x, yi.linear->var, shifted);
  };
  if (cond.rightIsConstant) {
    std::int64_t c = cond.constant;
    // `x <= c - 1` and `x >= c + 1`, `x >= c`, where 64 bits hold the bound.
    auto below = [&] {
      if (c != INT64_MIN)
        le(cond.left, core::ZeroSym, c - 1);
    };
    auto above = [&] {
      if (c != INT64_MAX)
        le(core::ZeroSym, cond.left, -(c + 1));
    };
    auto atLeast = [&] {
      if (c != INT64_MIN)
        le(core::ZeroSym, cond.left, -c);
    };
    switch (op) {
    case Op::Eq:
      le(cond.left, core::ZeroSym, c);
      atLeast();
      break;
    case Op::Ne: {
      auto lo = state.zone.lower(cond.left);
      auto hi = state.zone.upper(cond.left);
      if (lo && hi && *lo == c && *hi == c)
        return false;
      if (lo && *lo == c)
        above();
      else if (hi && *hi == c)
        below();
      break;
    }
    case Op::Lt:
      below();
      break;
    case Op::Le:
      le(cond.left, core::ZeroSym, c);
      break;
    case Op::Gt:
      above();
      break;
    case Op::Ge:
      atLeast();
      break;
    case Op::NonZero:
    case Op::And:
    case Op::Or:
      break;
    }
    if (ok && left.pointerBehind != core::ZeroSym && c == 0) {
      if (op == Op::Eq)
        ok = setNull(left.pointerBehind, true);
      else if (op == Op::Ne)
        ok = setNull(left.pointerBehind, false);
    }
    if (auto type = heap.info(state, cond.left).intType;
        hasValues(cond.left) && type)
      refineValues(cond.left, op,
                   core::IntegerRange::singleton(core::IntegerValue::ofBits(
                       *type, static_cast<std::uint64_t>(c))));
    // A truth value compared with 0 or 1 (`(p != NULL) == 0`): the
    // comparison it stands for holds or fails.
    if (ok && (op == Op::Eq || op == Op::Ne) && (c == 0 || c == 1) &&
        heap.info(state, cond.left).condition)
      ok = refineCondition(run, state, cond.left, (op == Op::Eq) == (c == 1));
    return ok && !state.zone.isBottom();
  }
  core::Sym r = cond.right;
  // RFC 0017 §3: `x <= K / y` (a range guard) bounds the product `x * y`
  // by `K`.
  auto noteGuard = [&](core::Sym x, core::Sym quotient) {
    const core::SymInfo *q = state.syms.find(quotient);
    if (!ok || q == nullptr || !q->defined ||
        q->defined->op != core::IntegerOp::Divide ||
        q->defined->right == core::ZeroSym || q->defined->constant)
      return;
    std::optional<std::uint64_t> dividend;
    if (const auto &k = q->defined->leftValue; k && !k->negative())
      dividend = k->magnitude();
    const core::SymInfo *xi = state.syms.find(x);
    if (!dividend || xi == nullptr || xi->type != core::SymInfo::Type::Int)
      return;
    heap.infoMut(state, x).productAtMost =
        std::make_pair(q->defined->right, *dividend);
  };
  switch (op) {
  case Op::Lt:
  case Op::Le:
    noteGuard(cond.left, r);
    break;
  case Op::Gt:
  case Op::Ge:
    noteGuard(r, cond.left);
    break;
  default:
    break;
  }
  if (hasValues(cond.left) || hasValues(r)) {
    const core::SymInfo &li = heap.info(state, cond.left);
    const core::SymInfo &ri = heap.info(state, r);
    if (li.intType && ri.intType && *li.intType == *ri.intType) {
      core::IntegerRange leftRange = rangeOf(state, cond.left, *li.intType);
      core::IntegerRange rightRange = rangeOf(state, r, *ri.intType);
      refineValues(cond.left, op, rightRange);
      Op reversed = op;
      switch (op) {
      case Op::Lt:
        reversed = Op::Gt;
        break;
      case Op::Le:
        reversed = Op::Ge;
        break;
      case Op::Gt:
        reversed = Op::Lt;
        break;
      case Op::Ge:
        reversed = Op::Le;
        break;
      default:
        break;
      }
      refineValues(r, reversed, leftRange);
    }
  }
  switch (op) {
  case Op::Eq:
    le(cond.left, r, 0);
    le(r, cond.left, 0);
    break;
  case Op::Ne:
    break;
  case Op::Lt:
    le(cond.left, r, -1);
    break;
  case Op::Le:
    le(cond.left, r, 0);
    break;
  case Op::Gt:
    le(r, cond.left, -1);
    break;
  case Op::Ge:
    le(r, cond.left, 0);
    break;
  case Op::NonZero:
  case Op::And:
  case Op::Or:
    break;
  }
  return ok && !state.zone.isBottom();
}

} // namespace weavec::analysis::engine
