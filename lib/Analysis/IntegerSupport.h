//===- IntegerSupport.h - Clang target types and numeric operators --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_LIB_ANALYSIS_INTEGERSUPPORT_H
#define WEAVEC_LIB_ANALYSIS_INTEGERSUPPORT_H

#include "weavec/Core/Integer.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/Type.h"
#include "clang/Basic/Builtins.h"

#include <optional>

namespace weavec::analysis {

inline std::uint64_t unsignedMagnitude(std::int64_t value) {
  const auto bits = static_cast<std::uint64_t>(value);
  return value < 0 ? std::uint64_t{0} - bits : bits;
}

[[nodiscard]] inline std::optional<core::IntegerOp>
checkedIntegerOp(const clang::CallExpr &call) {
  // The value rule of the overflow-checking compiler builtins, by their
  // builtin id (RFC 0030 §8: their rows give only the write through the
  // result pointer).
  const auto *callee = call.getDirectCallee();
  if (!callee || call.getNumArgs() != 3)
    return std::nullopt;
  switch (callee->getBuiltinID()) {
  case clang::Builtin::BI__builtin_add_overflow:
  case clang::Builtin::BI__builtin_sadd_overflow:
  case clang::Builtin::BI__builtin_saddl_overflow:
  case clang::Builtin::BI__builtin_saddll_overflow:
  case clang::Builtin::BI__builtin_uadd_overflow:
  case clang::Builtin::BI__builtin_uaddl_overflow:
  case clang::Builtin::BI__builtin_uaddll_overflow:
    return core::IntegerOp::Add;
  case clang::Builtin::BI__builtin_sub_overflow:
  case clang::Builtin::BI__builtin_ssub_overflow:
  case clang::Builtin::BI__builtin_ssubl_overflow:
  case clang::Builtin::BI__builtin_ssubll_overflow:
  case clang::Builtin::BI__builtin_usub_overflow:
  case clang::Builtin::BI__builtin_usubl_overflow:
  case clang::Builtin::BI__builtin_usubll_overflow:
    return core::IntegerOp::Subtract;
  case clang::Builtin::BI__builtin_mul_overflow:
  case clang::Builtin::BI__builtin_smul_overflow:
  case clang::Builtin::BI__builtin_smull_overflow:
  case clang::Builtin::BI__builtin_smulll_overflow:
  case clang::Builtin::BI__builtin_umul_overflow:
  case clang::Builtin::BI__builtin_umull_overflow:
  case clang::Builtin::BI__builtin_umulll_overflow:
    return core::IntegerOp::Multiply;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] inline std::optional<core::IntegerType>
integerTypeOf(clang::QualType type, const clang::ASTContext &context) {
  if (type.isNull())
    return std::nullopt;
  if (const auto *atomic = type->getAs<clang::AtomicType>())
    type = atomic->getValueType();
  if (!type->isIntegerType() || type->isDependentType())
    return std::nullopt;
  if (type->isBooleanType())
    return core::BooleanType;
  const unsigned width = context.getIntWidth(type);
  if (width == 0 || width > 64)
    return std::nullopt;
  return core::IntegerType{.width = width,
                           .isSigned = type->isSignedIntegerType(),
                           .isBoolean = false};
}

[[nodiscard]] inline std::optional<core::IntegerType>
integerTypeOf(const clang::ValueDecl &decl, const clang::ASTContext &context) {
  auto type = integerTypeOf(decl.getType(), context);
  const auto *field = llvm::dyn_cast<clang::FieldDecl>(&decl);
  if (type && field && field->isBitField() &&
      !field->getBitWidth()->isValueDependent()) {
    const auto width = field->getBitWidthValue();
    if (width == 0 || width > type->width)
      return std::nullopt;
    type->width = width;
  }
  return type;
}

[[nodiscard]] inline std::optional<core::IntegerOp>
integerOpOf(clang::BinaryOperatorKind op) {
  using enum core::IntegerOp;
  switch (op) {
  case clang::BO_Add:
  case clang::BO_AddAssign:
    return Add;
  case clang::BO_Sub:
  case clang::BO_SubAssign:
    return Subtract;
  case clang::BO_Mul:
  case clang::BO_MulAssign:
    return Multiply;
  case clang::BO_Div:
  case clang::BO_DivAssign:
    return Divide;
  case clang::BO_Rem:
  case clang::BO_RemAssign:
    return Remainder;
  case clang::BO_Shl:
  case clang::BO_ShlAssign:
    return ShiftLeft;
  case clang::BO_Shr:
  case clang::BO_ShrAssign:
    return ShiftRight;
  case clang::BO_And:
  case clang::BO_AndAssign:
    return BitAnd;
  case clang::BO_Or:
  case clang::BO_OrAssign:
    return BitOr;
  case clang::BO_Xor:
  case clang::BO_XorAssign:
    return BitXor;
  case clang::BO_EQ:
    return Equal;
  case clang::BO_NE:
    return NotEqual;
  case clang::BO_LT:
    return Less;
  case clang::BO_LE:
    return LessEqual;
  case clang::BO_GT:
    return Greater;
  case clang::BO_GE:
    return GreaterEqual;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] inline bool sameIntegerValue(core::IntegerValue a,
                                           core::IntegerValue b) {
  return a.negative() == b.negative() && a.magnitude() == b.magnitude();
}

[[nodiscard]] inline bool conversionPreserves(const core::IntegerRange &source,
                                              core::IntegerType destination) {
  if (source.empty())
    return false;
  return sameIntegerValue(*source.minimum(),
                          source.minimum()->converted(destination)) &&
         sameIntegerValue(*source.maximum(),
                          source.maximum()->converted(destination));
}

} // namespace weavec::analysis

#endif // WEAVEC_LIB_ANALYSIS_INTEGERSUPPORT_H
