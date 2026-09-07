//===- IntegerExpression.h - Bounded C value expressions -------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// RFC 0017. A canonical postfix expression has no graph references, target
// headers or local AST identities. Substitution reconstructs and validates
// every node. Limits apply before allocating or evaluating an input payload.
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_INTEGEREXPRESSION_H
#define WEAVEC_CORE_INTEGEREXPRESSION_H

#include "weavec/Core/Integer.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace weavec::core {

inline constexpr std::size_t MaxIntegerExpressionNodes = 64;
inline constexpr unsigned MaxIntegerExpressionDepth = 12;
inline constexpr std::size_t MaxIntegerExpressionText = 32768;

enum class IntegerNodeKind : std::uint8_t {
  Constant,
  Input,
  Convert,
  Operation,
  Overflow
};

template <typename Key>
struct IntegerNode {
  IntegerNodeKind kind = IntegerNodeKind::Constant;
  IntegerType type;
  std::uint64_t bits = 0;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<Key> key = {};
  IntegerOp op = IntegerOp::Add;
  bool wrapSigned = false;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<IntegerType> checkedType = {};
  friend auto operator<=>(const IntegerNode &, const IntegerNode &) = default;
};

template <typename Key>
class IntegerExpression {
public:
  using Node = IntegerNode<Key>;
  [[nodiscard]] static IntegerExpression constant(IntegerValue value) {
    return IntegerExpression({Node{.kind = IntegerNodeKind::Constant,
                                   .type = value.type,
                                   .bits = value.bits}});
  }
  [[nodiscard]] static IntegerExpression input(Key key, IntegerType type) {
    return IntegerExpression({Node{
        .kind = IntegerNodeKind::Input, .type = type, .key = std::move(key)}});
  }
  [[nodiscard]] IntegerType type() const { return nodes.back().type; }
  [[nodiscard]] const std::vector<Node> &all() const { return nodes; }
  [[nodiscard]] std::optional<IntegerValue> constantValue() const {
    if (nodes.size() != 1 || nodes.front().kind != IntegerNodeKind::Constant)
      return std::nullopt;
    return IntegerValue::ofBits(type(), nodes.front().bits);
  }
  [[nodiscard]] std::optional<Key> inputKey() const {
    return nodes.size() == 1 && nodes.front().kind == IntegerNodeKind::Input
               ? nodes.front().key
               : std::nullopt;
  }
  /// Direct operands in source-independent postfix order. Subexpressions
  /// retain their validated types and limits.
  [[nodiscard]] std::vector<IntegerExpression> operands() const {
    const auto &root = nodes.back();
    if (root.kind == IntegerNodeKind::Constant ||
        root.kind == IntegerNodeKind::Input)
      return {};
    const unsigned arity =
        root.kind == IntegerNodeKind::Convert || isUnary(root.op) ? 1 : 2;
    std::vector<IntegerExpression> result;
    auto end = nodes.size() - 1;
    for (unsigned i = 0; i < arity; ++i) {
      auto begin = end;
      unsigned pending = 1;
      while (pending != 0) {
        const auto &node = nodes[--begin];
        --pending;
        if (node.kind == IntegerNodeKind::Convert)
          ++pending;
        else if (node.kind == IntegerNodeKind::Operation ||
                 node.kind == IntegerNodeKind::Overflow)
          pending += isUnary(node.op) ? 1U : 2U;
      }
      result.push_back(IntegerExpression(
          std::vector<Node>(nodes.begin() + static_cast<std::ptrdiff_t>(begin),
                            nodes.begin() + static_cast<std::ptrdiff_t>(end))));
      end = begin;
    }
    std::reverse(result.begin(), result.end());
    return result;
  }
  [[nodiscard]] bool dependsOn(const Key &key) const {
    return std::ranges::any_of(
        nodes, [&](const Node &node) { return node.key == key; });
  }
  [[nodiscard]] std::optional<IntegerExpression>
  converted(IntegerType to) const {
    if (!to.valid())
      return std::nullopt;
    if (to == type())
      return *this;
    if (const auto value = constantValue())
      return constant(value->converted(to));
    auto result = nodes;
    result.push_back(Node{.kind = IntegerNodeKind::Convert, .type = to});
    return checked(std::move(result));
  }
  [[nodiscard]] static std::optional<IntegerExpression>
  operation(IntegerOp op, IntegerExpression lhs, IntegerExpression rhs,
            bool wrapSigned = false) {
    if (const auto a = lhs.constantValue()) {
      const auto b = isUnary(op) ? a : rhs.constantValue();
      if (b) {
        const auto evaluated = evaluateInteger(op, *a, *b, wrapSigned);
        if (evaluated.value)
          return constant(*evaluated.value);
      }
    }
    // Canonicalize only operations whose C evaluation is commutative. This
    // compares already captured values; it never reorders source side effects.
    if (!isUnary(op) &&
        (op == IntegerOp::Add || op == IntegerOp::Multiply ||
         op == IntegerOp::BitAnd || op == IntegerOp::BitOr ||
         op == IntegerOp::BitXor || op == IntegerOp::Equal ||
         op == IntegerOp::NotEqual || op == IntegerOp::Minimum ||
         op == IntegerOp::Maximum) &&
        rhs < lhs)
      std::swap(lhs, rhs);
    const auto type = isComparison(op) || op == IntegerOp::LogicalNot
                          ? BooleanType
                          : lhs.type();
    auto result = lhs.nodes;
    if (!isUnary(op))
      result.insert(result.end(), rhs.nodes.begin(), rhs.nodes.end());
    result.push_back(Node{.kind = IntegerNodeKind::Operation,
                          .type = type,
                          .op = op,
                          .wrapSigned = wrapSigned});
    return checked(std::move(result));
  }
  [[nodiscard]] static std::optional<IntegerExpression>
  overflow(IntegerOp op, IntegerExpression lhs, IntegerExpression rhs,
           IntegerType destination) {
    if (const auto a = lhs.constantValue())
      if (const auto b = rhs.constantValue())
        if (const auto value = evaluateCheckedInteger(op, *a, *b, destination))
          return constant(IntegerValue::ofBits(BooleanType, value->overflow));
    if (op != IntegerOp::Subtract && rhs < lhs)
      std::swap(lhs, rhs);
    auto nodes = lhs.nodes;
    nodes.insert(nodes.end(), rhs.nodes.begin(), rhs.nodes.end());
    nodes.push_back(Node{.kind = IntegerNodeKind::Overflow,
                         .type = BooleanType,
                         .op = op,
                         .checkedType = destination});
    return checked(std::move(nodes));
  }
  [[nodiscard]] static std::optional<IntegerExpression>
  checked(std::vector<Node> nodes) {
    if (nodes.empty() || nodes.size() > MaxIntegerExpressionNodes)
      return std::nullopt;
    struct Entry {
      IntegerType type;
      unsigned depth{};
    };
    std::vector<Entry> stack;
    for (const auto &node : nodes) {
      if (!node.type.valid())
        return std::nullopt;
      if ((node.kind == IntegerNodeKind::Overflow) !=
          node.checkedType.has_value())
        return std::nullopt;
      if (node.kind == IntegerNodeKind::Input ||
          node.kind == IntegerNodeKind::Constant) {
        if ((node.kind == IntegerNodeKind::Input) != node.key.has_value() ||
            node.bits > node.type.mask())
          return std::nullopt;
        stack.push_back({.type = node.type, .depth = 1});
        continue;
      }
      if (node.key || node.bits != 0 || stack.empty())
        return std::nullopt;
      auto last = stack.back();
      stack.pop_back();
      if (node.kind == IntegerNodeKind::Convert) {
        ++last.depth;
      } else if (node.kind == IntegerNodeKind::Overflow) {
        if (!node.checkedType->valid() || node.type != BooleanType ||
            node.wrapSigned ||
            (node.op != IntegerOp::Add && node.op != IntegerOp::Subtract &&
             node.op != IntegerOp::Multiply) ||
            stack.empty())
          return std::nullopt;
        last.depth = std::max(last.depth, stack.back().depth) + 1;
        stack.pop_back();
      } else if (node.kind == IntegerNodeKind::Operation) {
        if (!parseIntegerOp(core::toString(node.op)))
          return std::nullopt;
        auto lhs = last;
        if (!isUnary(node.op)) {
          if (stack.empty())
            return std::nullopt;
          lhs = stack.back();
          stack.pop_back();
          if (node.op != IntegerOp::ShiftLeft &&
              node.op != IntegerOp::ShiftRight && lhs.type != last.type)
            return std::nullopt;
        }
        const auto expected =
            isComparison(node.op) || node.op == IntegerOp::LogicalNot
                ? BooleanType
                : lhs.type;
        if (node.type != expected)
          return std::nullopt;
        last.depth = std::max(lhs.depth, last.depth) + 1;
      } else {
        return std::nullopt;
      }
      if (last.depth > MaxIntegerExpressionDepth)
        return std::nullopt;
      stack.push_back({.type = node.type, .depth = last.depth});
    }
    if (stack.size() != 1)
      return std::nullopt;
    return IntegerExpression(std::move(nodes));
  }
  template <typename Read>
  [[nodiscard]] IntegerRangeEvaluation evaluate(Read read) const {
    std::vector<IntegerRangeEvaluation> stack;
    for (const auto &node : nodes) {
      if (node.kind == IntegerNodeKind::Constant) {
        stack.push_back({.values = IntegerRange::singleton(
                             IntegerValue::ofBits(node.type, node.bits))});
      } else if (node.kind == IntegerNodeKind::Input) {
        stack.push_back(
            {.values = read(*node.key, node.type).converted(node.type)});
      } else {
        auto rhs = stack.back();
        stack.pop_back();
        if (node.kind == IntegerNodeKind::Convert) {
          rhs.values = rhs.values.converted(node.type);
          stack.push_back(std::move(rhs));
          continue;
        }
        auto lhs = rhs;
        if (!isUnary(node.op)) {
          lhs = stack.back();
          stack.pop_back();
        }
        auto result =
            node.kind == IntegerNodeKind::Overflow
                ? IntegerRangeEvaluation{.values =
                                             evaluateCheckedInteger(
                                                 node.op, lhs.values,
                                                 rhs.values, *node.checkedType)
                                                 .overflow}
                : evaluateInteger(node.op, lhs.values, rhs.values,
                                  node.wrapSigned);
        if (lhs.mayBeInvalid || rhs.mayBeInvalid) {
          result.values = IntegerRange::full(node.type);
          result.mayBeInvalid = true;
          if (lhs.alwaysInvalid || rhs.alwaysInvalid) {
            result.alwaysInvalid = true;
            result.error = lhs.alwaysInvalid ? lhs.error : rhs.error;
          } else if (!result.alwaysInvalid) {
            result.error = lhs.mayBeInvalid ? lhs.error : rhs.error;
          }
        }
        stack.push_back(std::move(result));
      }
    }
    return stack.back();
  }
  template <typename OtherKey, typename Substitute>
  [[nodiscard]] std::optional<IntegerExpression<OtherKey>>
  substitute(Substitute substitute) const {
    using Other = IntegerExpression<OtherKey>;
    std::vector<Other> stack;
    for (const auto &node : nodes) {
      if (node.kind == IntegerNodeKind::Constant) {
        stack.push_back(
            Other::constant(IntegerValue::ofBits(node.type, node.bits)));
      } else if (node.kind == IntegerNodeKind::Input) {
        const auto replacement = substitute(*node.key, node.type);
        if (!replacement)
          return std::nullopt;
        const auto value = replacement->converted(node.type);
        if (!value)
          return std::nullopt;
        stack.push_back(*value);
      } else {
        auto rhs = stack.back();
        stack.pop_back();
        std::optional<Other> result;
        if (node.kind == IntegerNodeKind::Convert) {
          result = rhs.converted(node.type);
        } else {
          auto lhs = rhs;
          if (!isUnary(node.op)) {
            lhs = stack.back();
            stack.pop_back();
          }
          result = node.kind == IntegerNodeKind::Overflow
                       ? Other::overflow(node.op, lhs, rhs, *node.checkedType)
                       : Other::operation(node.op, lhs, rhs, node.wrapSigned);
        }
        if (!result)
          return std::nullopt;
        stack.push_back(*result);
      }
    }
    return stack.back();
  }
  /// A nonzero modular add/subtract/xor changes the bit pattern. This is
  /// not an affine equality and establishes no ordering or sign fact.
  [[nodiscard]] std::optional<Key> differsFromInput() const {
    if (nodes.size() != 3 || nodes.back().kind != IntegerNodeKind::Operation ||
        type().isBoolean)
      return std::nullopt;
    const auto op = nodes.back().op;
    if (op != IntegerOp::Add && op != IntegerOp::Subtract &&
        op != IntegerOp::BitXor)
      return std::nullopt;
    const Node *input = nodes.data();
    const Node *constant = &nodes[1];
    if (input->kind == IntegerNodeKind::Constant && op != IntegerOp::Subtract)
      std::swap(input, constant);
    if (input->kind != IntegerNodeKind::Input ||
        constant->kind != IntegerNodeKind::Constant || constant->bits == 0 ||
        input->type != type())
      return std::nullopt;
    return input->key;
  }
  /// Guaranteed power-of-two byte alignment, including modular wrap. This
  /// permits copying whole pointer cells without equating n*sizeof(T) to
  /// its unbounded mathematical product (RFC 0017).
  [[nodiscard]] bool divisibleBy(std::uint64_t divisor) const {
    if (divisor == 0 || !std::has_single_bit(divisor))
      return false;
    std::vector<unsigned> stack;
    for (const auto &node : nodes) {
      if (node.kind == IntegerNodeKind::Constant) {
        stack.push_back(static_cast<unsigned>(std::countr_zero(node.bits)));
      } else if (node.kind == IntegerNodeKind::Input) {
        stack.push_back(0);
      } else {
        auto rhs = stack.back();
        stack.pop_back();
        if (node.kind == IntegerNodeKind::Convert) {
          stack.push_back(node.type.isBoolean ? 0
                                              : std::min(rhs, node.type.width));
          continue;
        }
        auto lhs = rhs;
        if (!isUnary(node.op)) {
          lhs = stack.back();
          stack.pop_back();
        }
        unsigned zeros = 0;
        switch (node.op) {
        case IntegerOp::Add:
        case IntegerOp::Subtract:
        case IntegerOp::BitOr:
        case IntegerOp::BitXor:
        case IntegerOp::Minimum:
        case IntegerOp::Maximum:
          zeros = std::min(lhs, rhs);
          break;
        case IntegerOp::Multiply:
          zeros = std::min(64U, lhs + rhs);
          break;
        case IntegerOp::BitAnd:
          zeros = std::max(lhs, rhs);
          break;
        case IntegerOp::Negate:
          zeros = lhs;
          break;
        default:
          break;
        }
        stack.push_back(node.kind == IntegerNodeKind::Overflow
                            ? 0
                            : std::min(zeros, node.type.width));
      }
    }
    return std::cmp_greater_equal(stack.back(), std::countr_zero(divisor));
  }
  template <typename Print>
  [[nodiscard]] std::string describe(Print print) const {
    std::vector<std::string> stack;
    for (const auto &node : nodes) {
      if (node.kind == IntegerNodeKind::Input) {
        stack.push_back(print(*node.key));
      } else if (node.kind == IntegerNodeKind::Constant) {
        stack.push_back(IntegerValue::ofBits(node.type, node.bits).toString());
      } else {
        auto rhs = stack.back();
        stack.pop_back();
        if (node.kind == IntegerNodeKind::Convert) {
          stack.push_back(node.type.toString() + "(" + rhs + ")");
        } else if (isUnary(node.op)) {
          stack.push_back(std::string(core::toString(node.op)) + "(" + rhs +
                          ")");
        } else {
          auto lhs = stack.back();
          stack.pop_back();
          auto operation = node.kind == IntegerNodeKind::Overflow
                               ? "overflow-" +
                                     std::string(core::toString(node.op)) +
                                     "-" + node.checkedType->toString()
                               : std::string(core::toString(node.op));
          if (node.kind == IntegerNodeKind::Operation &&
              node.op == IntegerOp::Add) {
            if (!lhs.empty() && lhs.front() >= '0' && lhs.front() <= '9')
              std::swap(lhs, rhs);
            lhs += '+';
            lhs += rhs;
            stack.push_back(std::move(lhs));
          } else if (node.kind == IntegerNodeKind::Operation &&
                     node.op == IntegerOp::Subtract) {
            lhs.insert(0, "(");
            lhs += '-';
            lhs += rhs;
            lhs += ')';
            stack.push_back(std::move(lhs));
          } else {
            operation += '(';
            operation += lhs;
            operation += ", ";
            operation += rhs;
            operation += ')';
            stack.push_back(std::move(operation));
          }
        }
      }
    }
    return stack.back();
  }
  template <typename Print>
  [[nodiscard]] std::string toString(Print print) const {
    static constexpr std::string_view Hex = "0123456789abcdef";
    std::string result;
    for (const auto &node : nodes) {
      if (!result.empty())
        result += ';';
      result += node.type.toString() + ',';
      switch (node.kind) {
      case IntegerNodeKind::Constant:
        result += "c," + std::to_string(node.bits);
        break;
      case IntegerNodeKind::Input:
        result += "v,";
        for (const char character : print(*node.key)) {
          const auto byte = static_cast<unsigned char>(character);
          result += Hex[byte >> 4U];
          result += Hex[byte & 15U];
        }
        break;
      case IntegerNodeKind::Convert:
        result += "cast";
        break;
      case IntegerNodeKind::Operation:
        result += std::string(core::toString(node.op));
        if (node.wrapSigned)
          result += ",wrap";
        break;
      case IntegerNodeKind::Overflow:
        result += "overflow-" + std::string(core::toString(node.op)) + "," +
                  node.checkedType->toString();
        break;
      }
    }
    return result;
  }
  template <typename Parse>
  [[nodiscard]] static std::optional<IntegerExpression>
  parse(std::string_view text, Parse parse) {
    if (text.empty() || text.back() == ';' ||
        text.size() > MaxIntegerExpressionText)
      return std::nullopt;
    std::vector<Node> nodes;
    const auto take = [](std::string_view &input, char separator) {
      const auto pos = input.find(separator);
      const auto part = input.substr(0, pos);
      input = pos == std::string_view::npos ? std::string_view()
                                            : input.substr(pos + 1);
      return part;
    };
    while (!text.empty()) {
      if (nodes.size() == MaxIntegerExpressionNodes)
        return std::nullopt;
      auto record = take(text, ';');
      if (record.empty() || record.back() == ',')
        return std::nullopt;
      const auto type = IntegerType::parse(take(record, ','));
      if (!type)
        return std::nullopt;
      const auto op = take(record, ',');
      Node node{.type = *type};
      if (op == "c") {
        const auto [end, error] = std::from_chars(
            record.data(), record.data() + record.size(), node.bits);
        if (error != std::errc{} || end != record.data() + record.size())
          return std::nullopt;
      } else if (op == "v") {
        if (record.empty() || record.size() % 2 != 0)
          return std::nullopt;
        std::string name;
        for (std::size_t i = 0; i < record.size(); i += 2) {
          unsigned byte = 0;
          const auto [end, error] = std::from_chars(
              record.data() + i, record.data() + i + 2, byte, 16);
          if (error != std::errc{} || end != record.data() + i + 2 || byte == 0)
            return std::nullopt;
          name += static_cast<char>(byte);
        }
        node.key = parse(name);
        if (!node.key)
          return std::nullopt;
        node.kind = IntegerNodeKind::Input;
      } else if (op == "cast") {
        if (!record.empty())
          return std::nullopt;
        node.kind = IntegerNodeKind::Convert;
      } else if (op.starts_with("overflow-")) {
        const auto operation = parseIntegerOp(op.substr(9));
        const auto destination = IntegerType::parse(record);
        if (!operation || !destination)
          return std::nullopt;
        node.kind = IntegerNodeKind::Overflow;
        node.op = *operation;
        node.checkedType = *destination;
      } else {
        const auto operation = parseIntegerOp(op);
        if (!operation || (!record.empty() && record != "wrap"))
          return std::nullopt;
        node.kind = IntegerNodeKind::Operation;
        node.op = *operation;
        node.wrapSigned = record == "wrap";
      }
      nodes.push_back(std::move(node));
    }
    return checked(std::move(nodes));
  }
  friend auto operator<=>(const IntegerExpression &,
                          const IntegerExpression &) = default;

private:
  explicit IntegerExpression(std::vector<Node> nodes)
      : nodes(std::move(nodes)) {}
  std::vector<Node> nodes;
};

template <typename Key>
struct IntegerPredicate {
  IntegerExpression<Key> lhs;
  IntegerOp op = IntegerOp::Equal;
  IntegerExpression<Key> rhs;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<IntegerRange> range = {};
  [[nodiscard]] bool dependsOn(const Key &key) const {
    return lhs.dependsOn(key) || rhs.dependsOn(key);
  }
  template <typename Read>
  [[nodiscard]] std::optional<bool> evaluate(Read read) const {
    if (!isComparison(op))
      return std::nullopt;
    const auto a = lhs.evaluate(read);
    const auto b = rhs.evaluate(read);
    if (range && !a.mayBeInvalid) {
      if (range->contains(a.values))
        return true;
      if (range->disjoint(a.values))
        return false;
      return std::nullopt;
    }
    if (a.mayBeInvalid || b.mayBeInvalid)
      return std::nullopt;
    if (lhs == rhs)
      return op == IntegerOp::Equal || op == IntegerOp::LessEqual ||
             op == IntegerOp::GreaterEqual;
    const auto result = evaluateInteger(op, a.values, b.values);
    const auto value = result.values.constant();
    return value && !result.mayBeInvalid ? std::optional(value->bits != 0)
                                         : std::nullopt;
  }
  template <typename OtherKey, typename Substitute>
  [[nodiscard]] std::optional<IntegerPredicate<OtherKey>>
  substitute(Substitute substitute) const {
    const auto a = lhs.template substitute<OtherKey>(substitute);
    const auto b = rhs.template substitute<OtherKey>(substitute);
    if (!a || !b)
      return std::nullopt;
    return IntegerPredicate<OtherKey>{
        .lhs = *a, .op = op, .rhs = *b, .range = range};
  }
  friend auto operator<=>(const IntegerPredicate &,
                          const IntegerPredicate &) = default;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_INTEGEREXPRESSION_H
