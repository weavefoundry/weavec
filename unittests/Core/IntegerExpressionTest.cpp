//===- IntegerExpressionTest.cpp - Bounded numeric expressions ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/IntegerExpression.h"

#include "gtest/gtest.h"

using namespace weavec::core;
using Expression = IntegerExpression<std::string>;
static constexpr IntegerType U8{.width = 8, .isSigned = false};
static constexpr IntegerType I8{.width = 8, .isSigned = true};

TEST(IntegerExpression,
     OperationsRetainWidthsAndCanonicalizeCommutativeValues) {
  const auto a = Expression::input("a", U8);
  const auto b = Expression::input("b", U8);
  const auto product = Expression::operation(IntegerOp::Multiply, a, b);
  ASSERT_TRUE(product);
  EXPECT_EQ(product, Expression::operation(IntegerOp::Multiply, b, a));
  EXPECT_TRUE(product->dependsOn("a"));
  EXPECT_TRUE(product->dependsOn("b"));
  EXPECT_FALSE(product->dependsOn("c"));
  const auto evaluated =
      product->evaluate([](const std::string &key, IntegerType type) {
        return IntegerRange::singleton(
            IntegerValue::ofBits(type, key == "a" ? 16 : 17));
      });
  EXPECT_EQ(evaluated.values.constant(), IntegerValue::ofBits(U8, 16));
  EXPECT_FALSE(evaluated.mayBeInvalid);
  const auto converted = product->converted({.width = 16, .isSigned = false});
  ASSERT_TRUE(converted);
  EXPECT_EQ(converted->type(), (IntegerType{16, false}));
}

TEST(IntegerExpression, InvalidArithmeticIsNotFoldedIntoAValue) {
  const auto product = Expression::operation(
      IntegerOp::Multiply, Expression::constant(IntegerValue::ofBits(I8, 16)),
      Expression::constant(IntegerValue::ofBits(I8, 17)));
  ASSERT_TRUE(product);
  EXPECT_FALSE(product->constantValue());
  const auto result =
      product->evaluate([](const std::string &, IntegerType type) {
        return IntegerRange::full(type);
      });
  EXPECT_TRUE(result.alwaysInvalid);
  EXPECT_EQ(result.error, IntegerError::SignedOverflow);
}

TEST(IntegerExpression, SerializationAndSubstitutionPreserveEveryDependency) {
  const auto expression = Expression::operation(
      IntegerOp::Minimum, Expression::input("global count.field", U8),
      Expression::input("param 1", U8));
  ASSERT_TRUE(expression);
  const auto text =
      expression->toString([](const std::string &key) { return key; });
  EXPECT_EQ(expression, Expression::parse(text, [](std::string_view key) {
              return std::optional(std::string(key));
            }));
  const auto replaced = expression->substitute<std::string>(
      [](const std::string &key,
         IntegerType type) -> std::optional<Expression> {
        if (key.starts_with("global"))
          return Expression::constant(IntegerValue::ofBits(type, 7));
        return Expression::input("n", type);
      });
  ASSERT_TRUE(replaced);
  EXPECT_TRUE(replaced->dependsOn("n"));
  EXPECT_FALSE(replaced->dependsOn("param 1"));
  EXPECT_FALSE(expression->substitute<std::string>(
      [](const std::string &key,
         IntegerType type) -> std::optional<Expression> {
        if (key.starts_with("global"))
          return std::nullopt;
        return Expression::input(key, type);
      }));
}

TEST(IntegerExpression, ParsingRejectsMalformedAndOversizedPrograms) {
  const auto parse = [](std::string_view text) {
    return Expression::parse(text, [](std::string_view key) {
      return std::optional(std::string(key));
    });
  };
  for (const auto *const text :
       {"", "u0,c,0", "u65,c,0", "u8,c,256", "u8,add", "u8,cast", "u8,c,1;",
        "u8,c,1;u8,c,2", "u8,v,1", "u8,v,zz", "u8,v,00", "u8,c,1;i8,c,2;u8,add",
        "u8,c,1;u8,c,2;u8,bogus", "u8,c,1;u8,cast,extra",
        "u8,c,1;u8,c,2;u8,add,extra"})
    EXPECT_FALSE(parse(text)) << text;
  EXPECT_FALSE(parse(std::string(MaxIntegerExpressionText + 1, 'a')));
  std::string deep = "u8,v,61";
  for (unsigned i = 1; i < MaxIntegerExpressionDepth; ++i)
    deep += ";u8,neg";
  ASSERT_TRUE(parse(deep));
  EXPECT_FALSE(parse(deep + ";u8,neg"));
}

TEST(IntegerExpression, SubstitutionEnforcesTheExpressionBudget) {
  auto expression = Expression::input("a", U8);
  for (unsigned i = 0; i < 5; ++i) {
    auto doubled =
        Expression::operation(IntegerOp::Add, expression, expression);
    ASSERT_TRUE(doubled);
    expression = *doubled;
  }
  EXPECT_EQ(expression.all().size(), 63U);
  EXPECT_FALSE(Expression::operation(IntegerOp::Add, expression, expression));
  EXPECT_FALSE(expression.substitute<std::string>(
      [](const std::string &key, IntegerType type) {
        return Expression::operation(
            IntegerOp::Add, Expression::input(key, type),
            Expression::constant(IntegerValue::ofBits(type, 1)));
      }));
}

TEST(IntegerExpression, CheckedOverflowRetainsOperandAndDestinationTypes) {
  const auto parse = [](std::string_view text) {
    return Expression::parse(text, [](std::string_view key) {
      return std::optional(std::string(key));
    });
  };
  const IntegerType narrow{.width = 8, .isSigned = false};
  const IntegerType wide{.width = 64, .isSigned = false};
  const IntegerType signedType{.width = 32, .isSigned = true};
  const auto expression =
      Expression::overflow(IntegerOp::Add, Expression::input("n", wide),
                           Expression::input("delta", signedType), narrow);
  ASSERT_TRUE(expression);
  const auto text = expression->toString([](const auto &key) { return key; });
  EXPECT_EQ(parse(text), expression);
  const auto overflow =
      expression->evaluate([&](const auto &key, IntegerType type) {
        return IntegerRange::singleton(
            IntegerValue::ofBits(type, key == "n" ? 256 : UINT64_MAX));
      });
  ASSERT_TRUE(overflow.values.constant());
  EXPECT_EQ(overflow.values.constant()->bits, 0U);
  EXPECT_FALSE(parse("u8,v,6e;u8,c,1;b1,overflow-div,u8"));
  EXPECT_FALSE(parse("u8,v,6e;u8,c,1;u8,overflow-add,u8"));
  EXPECT_FALSE(parse("u8,v,6e;u8,c,1;b1,overflow-add,u65"));
}

TEST(IntegerExpression, OperandsRetainValidatedSubexpressions) {
  using Expr = weavec::core::IntegerExpression<unsigned>;
  using namespace weavec::core;
  const auto a = Expr::input(1, {.width = 32, .isSigned = false});
  const auto b = Expr::input(2, {.width = 32, .isSigned = false});
  const auto sum = Expr::operation(IntegerOp::Add, a, b);
  ASSERT_TRUE(sum);
  const auto cast = sum->converted({.width = 8, .isSigned = false});
  ASSERT_TRUE(cast);
  EXPECT_EQ(cast->operands(), std::vector<Expr>{*sum});
  EXPECT_EQ(sum->operands(), (std::vector<Expr>{a, b}));
  EXPECT_TRUE(a.operands().empty());
}

TEST(IntegerExpression, EnclosingOperationsPreserveDefiniteInvalidity) {
  const auto zero = Expression::constant(IntegerValue::ofBits(I8, 0));
  const auto one = Expression::constant(IntegerValue::ofBits(I8, 1));
  const auto invalid = Expression::operation(IntegerOp::Divide, one, zero);
  ASSERT_TRUE(invalid);
  const auto read = [](const std::string &, IntegerType type) {
    return IntegerRange::full(type);
  };
  for (const auto op : {IntegerOp::Add, IntegerOp::BitAnd, IntegerOp::Equal,
                        IntegerOp::Negate, IntegerOp::LogicalNot}) {
    const auto enclosing = Expression::operation(op, *invalid, zero);
    ASSERT_TRUE(enclosing);
    const auto result = enclosing->evaluate(read);
    EXPECT_TRUE(result.alwaysInvalid);
    EXPECT_TRUE(result.mayBeInvalid);
    EXPECT_TRUE(result.values.isFull());
    EXPECT_EQ(result.error, IntegerError::DivisionByZero);
  }
  const auto overflow =
      Expression::overflow(IntegerOp::Add, zero, *invalid, U8);
  ASSERT_TRUE(overflow);
  const auto result = overflow->evaluate(read);
  EXPECT_TRUE(result.alwaysInvalid);
  EXPECT_EQ(result.error, IntegerError::DivisionByZero);
}

TEST(IntegerExpression, PossiblyInvalidSubexpressionsCannotProvePredicates) {
  const auto input = Expression::input("n", I8);
  const auto one = Expression::constant(IntegerValue::ofBits(I8, 1));
  const auto sum = Expression::operation(IntegerOp::Add, input, one);
  ASSERT_TRUE(sum);
  const auto zero = Expression::constant(IntegerValue::ofBits(I8, 0));
  const auto masked = Expression::operation(IntegerOp::BitAnd, *sum, zero);
  ASSERT_TRUE(masked);
  const auto read = [](const std::string &, IntegerType type) {
    return IntegerRange::between(IntegerValue::ofBits(type, 126),
                                 IntegerValue::ofBits(type, 127));
  };
  const auto evaluated = masked->evaluate(read);
  EXPECT_TRUE(evaluated.mayBeInvalid);
  EXPECT_FALSE(evaluated.alwaysInvalid);
  EXPECT_TRUE(evaluated.values.isFull());
  EXPECT_EQ(evaluated.error, IntegerError::SignedOverflow);
  const IntegerPredicate<std::string> equality{
      .lhs = *masked, .op = IntegerOp::Equal, .rhs = zero};
  EXPECT_FALSE(equality.evaluate(read).has_value());
  const IntegerPredicate<std::string> identity{
      .lhs = *sum, .op = IntegerOp::Equal, .rhs = *sum};
  EXPECT_FALSE(identity.evaluate(read).has_value());

  // A possible child overflow must not replace the definite parent's reason.
  const auto divide = Expression::operation(IntegerOp::Divide, *sum, zero);
  ASSERT_TRUE(divide);
  const auto invalid = divide->evaluate(read);
  EXPECT_TRUE(invalid.alwaysInvalid);
  EXPECT_EQ(invalid.error, IntegerError::DivisionByZero);
}

TEST(IntegerExpression,
     ParsingRejectsEmptyFieldsAndMalformedCheckedOperations) {
  const auto parse = [](std::string_view text) {
    return Expression::parse(text, [](std::string_view key) {
      return std::optional(std::string(key));
    });
  };
  for (const auto *const text :
       {"u8,v,61;u8,neg,", "u8,v,61;u16,cast,", "u8,c,1;u8,c,2;u8,add,",
        "u8,v,61;;u8,neg", "u8,c,1;b1,overflow-add,u8",
        "u8,c,1;u8,c,2;b1,overflow-neg,u8",
        "u8,c,1;u8,c,2;b1,overflow-add,u8,wrap",
        "u8,c,1;u8,c,2;b1,overflow-add,b8", "u8,v,610062", "u8,v,6",
        "u4294967297,c,0", "i18446744073709551616,c,0",
        "u64,c,18446744073709551616", "u8,c,1;i8,c,2;b1,eq"})
    EXPECT_FALSE(parse(text)) << text;
}

TEST(IntegerExpression, CheckedNodesRejectMalformedStacksTypesAndMetadata) {
  using Node = Expression::Node;
  const Node input{.kind = IntegerNodeKind::Input, .type = U8, .key = "a"};
  EXPECT_FALSE(Expression::checked({}));
  EXPECT_FALSE(Expression::checked({input, input}));
  EXPECT_FALSE(
      Expression::checked({Node{.kind = IntegerNodeKind::Input, .type = U8}}));
  EXPECT_FALSE(Expression::checked(
      {Node{.kind = IntegerNodeKind::Constant, .type = U8, .bits = 256}}));
  EXPECT_FALSE(Expression::checked(
      {Node{.kind = IntegerNodeKind::Constant, .type = U8, .key = "a"}}));
  EXPECT_FALSE(
      Expression::checked({input, Node{.kind = IntegerNodeKind::Operation,
                                       .type = U8,
                                       .op = IntegerOp::Add}}));
  // Malformed opcode regression deliberately constructs an unknown enumerator.
  // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
  EXPECT_FALSE(
      Expression::checked({input, Node{.kind = IntegerNodeKind::Operation,
                                       .type = U8,
                                       .op = static_cast<IntegerOp>(255)}}));
  // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
  EXPECT_FALSE(Expression::checked(
      {input,
       Node{.kind = IntegerNodeKind::Convert, .type = U8, .checkedType = U8}}));
  EXPECT_FALSE(Expression::checked({input, input,
                                    Node{.kind = IntegerNodeKind::Overflow,
                                         .type = BooleanType,
                                         .op = IntegerOp::Add,
                                         .wrapSigned = true,
                                         .checkedType = U8}}));
}

TEST(IntegerExpression, NodeLimitAcceptsExactly64AndRejects65BeforeEvaluation) {
  auto expression = Expression::input("a", U8);
  for (unsigned i = 0; i < 5; ++i) {
    const auto next =
        Expression::operation(IntegerOp::BitXor, expression, expression);
    ASSERT_TRUE(next);
    expression = *next;
  }
  const auto atLimit = expression.converted({.width = 16, .isSigned = false});
  ASSERT_TRUE(atLimit);
  ASSERT_EQ(atLimit->all().size(), MaxIntegerExpressionNodes);
  const auto text = atLimit->toString([](const auto &key) { return key; });
  const auto parse = [](std::string_view input) {
    return Expression::parse(input, [](std::string_view key) {
      return std::optional(std::string(key));
    });
  };
  EXPECT_EQ(parse(text), atLimit);
  EXPECT_FALSE(parse(text + ";u32,cast"));
  EXPECT_FALSE(atLimit->converted({32, false}));
  auto nodes = atLimit->all();
  nodes.push_back({.kind = IntegerNodeKind::Convert,
                   .type = {.width = 32, .isSigned = false}});
  EXPECT_FALSE(Expression::checked(std::move(nodes)));
}
