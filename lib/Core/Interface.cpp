//===- Interface.cpp - Validated interface storage (RFC 0028) -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Core/Interface.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <functional>
#include <limits>
#include <set>

namespace weavec::core {

static bool interfaceText(std::string_view text) {
  return text.size() <= 16384 && std::ranges::all_of(text, [](unsigned char c) {
           return c >= 32 && c < 127;
         });
}

static bool interfaceIdentifier(std::string_view text) {
  if (text.empty())
    return false;
  const auto letter = [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  return letter(static_cast<unsigned char>(text.front())) &&
         std::ranges::all_of(text, [&](unsigned char c) {
           return letter(c) || (c >= '0' && c <= '9');
         });
}

bool InterfaceType::valid() const {
  if (nodes.empty() || nodes.size() > MaxInterfaceNodes)
    return false;
  std::size_t textBytes = 0;
  for (const auto &node : nodes) {
    if (node.kind > InterfaceKind::Array || node.qualifiers > 7 ||
        !interfaceText(node.name) || !interfaceText(node.view) ||
        !interfaceText(node.typedefName) ||
        node.fields.size() > MaxInterfaceFields ||
        node.parameters.size() > MaxInterfaceFields ||
        node.bytes > std::numeric_limits<std::int64_t>::max())
      return false;
    if ((node.qualifiers & 4U) && node.kind != InterfaceKind::Pointer)
      return false;
    if (!node.typedefName.empty() &&
        (node.kind != InterfaceKind::Record || !node.name.empty() ||
         !node.bytes || !interfaceIdentifier(node.typedefName)))
      return false;
    if (node.kind == InterfaceKind::Record) {
      if ((!node.name.empty() && !interfaceIdentifier(node.name)) ||
          (node.bytes && node.view.empty()) ||
          (!node.bytes && !node.view.empty()))
        return false;
    } else if (!node.view.empty()) {
      return false;
    }
    if (node.kind != InterfaceKind::Record &&
        node.kind != InterfaceKind::Integer &&
        node.kind != InterfaceKind::Floating && !node.name.empty())
      return false;
    textBytes += node.name.size() + node.view.size() + node.typedefName.size();
    if (textBytes > MaxInterfaceBytes)
      return false;
    const bool incomplete =
        node.kind == InterfaceKind::Record && node.bytes == 0;
    const bool unsized = node.kind == InterfaceKind::Void ||
                         node.kind == InterfaceKind::Function || incomplete;
    if (unsized
            ? (node.bytes || node.alignment)
            : (!node.bytes || !std::has_single_bit(node.alignment) ||
               node.alignment > node.bytes || node.bytes % node.alignment != 0))
      return false;
    if (node.kind != InterfaceKind::Record && !node.fields.empty())
      return false;
    if (node.kind != InterfaceKind::Function &&
        (!node.parameters.empty() || node.variadic || !node.prototype))
      return false;
    if (node.kind != InterfaceKind::Array && node.count)
      return false;
    const bool reference = node.kind == InterfaceKind::Pointer ||
                           node.kind == InterfaceKind::Array ||
                           node.kind == InterfaceKind::Function;
    if (reference && node.element >= nodes.size())
      return false;
    if (!reference && node.element)
      return false;
    if (node.kind == InterfaceKind::Array) {
      const auto &element = nodes[node.element];
      if (!node.count || !element.bytes ||
          node.count > node.bytes / element.bytes ||
          node.count * element.bytes != node.bytes ||
          node.alignment != element.alignment)
        return false;
    }
    if (node.kind == InterfaceKind::Pointer &&
        nodes[node.element].kind == InterfaceKind::Void &&
        !nodes[node.element].name.empty())
      return false;
    if (node.kind == InterfaceKind::Function) {
      if (nodes[node.element].kind == InterfaceKind::Function ||
          nodes[node.element].kind == InterfaceKind::Array ||
          (!node.prototype && (!node.parameters.empty() || node.variadic)))
        return false;
      for (const auto parameter : node.parameters)
        if (parameter >= nodes.size() || !nodes[parameter].bytes ||
            nodes[parameter].kind == InterfaceKind::Array)
          return false;
    }
    std::set<std::string> names;
    std::uint64_t end = 0;
    for (const auto &field : node.fields) {
      if (incomplete || !interfaceIdentifier(field.name) ||
          !interfaceText(field.name) || !names.insert(field.name).second ||
          field.type >= nodes.size())
        return false;
      const auto &type = nodes[field.type];
      if (!type.bytes || field.offset < end || field.offset > node.bytes ||
          type.bytes > node.bytes - field.offset)
        return false;
      end = field.offset + type.bytes;
      textBytes += field.name.size();
    }
  }
  if (textBytes > MaxInterfaceBytes)
    return false;
  std::vector<unsigned> colors(nodes.size());
  const auto visit = [&](auto &&self, std::size_t id) -> bool {
    if (colors[id] == 1)
      return false;
    if (colors[id] == 2)
      return true;
    colors[id] = 1;
    const auto &node = nodes[id];
    if (node.kind == InterfaceKind::Array && !self(self, node.element))
      return false;
    for (const auto &field : node.fields)
      if (!self(self, field.type))
        return false;
    colors[id] = 2;
    return true;
  };
  for (std::size_t i = 0; i < nodes.size(); ++i)
    if (!visit(visit, i))
      return false;
  return true;
}

std::string InterfaceType::encode() const {
  if (!valid())
    return {};
  std::string result = "it2;";
  const auto number = [&](std::uint64_t value) {
    result += std::to_string(value);
    result += ';';
  };
  const auto text = [&](std::string_view value) {
    number(value.size());
    result += value;
  };
  number(nodes.size());
  for (const auto &node : nodes) {
    number(static_cast<unsigned>(node.kind));
    number(node.bytes);
    number(node.alignment);
    number(node.count);
    number(node.element);
    number(node.qualifiers);
    number(node.variadic ? 1U : 0U);
    number(node.prototype ? 1U : 0U);
    text(node.name);
    text(node.view);
    text(node.typedefName);
    number(node.parameters.size());
    for (const auto parameter : node.parameters)
      number(parameter);
    number(node.fields.size());
    for (const auto &field : node.fields) {
      text(field.name);
      number(field.type);
      number(field.offset);
    }
  }
  return result.size() <= MaxInterfaceBytes ? result : std::string{};
}

std::optional<InterfaceType> InterfaceType::decode(std::string_view input) {
  if (input.size() > MaxInterfaceBytes || !input.starts_with("it2;"))
    return std::nullopt;
  const auto original = input;
  input.remove_prefix(4);
  bool ok = true;
  const auto number = [&](std::uint64_t maximum) {
    const auto delimiter = input.find(';');
    std::uint64_t value = 0;
    if (delimiter == std::string_view::npos || !delimiter || delimiter > 20) {
      ok = false;
      return value;
    }
    const auto token = input.substr(0, delimiter);
    const auto parsed =
        std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
        value > maximum || token != std::to_string(value))
      ok = false;
    input.remove_prefix(delimiter + 1);
    return value;
  };
  const auto text = [&] {
    const auto size = number(MaxInterfaceBytes);
    if (!ok || size > input.size()) {
      ok = false;
      return std::string{};
    }
    std::string result(input.substr(0, static_cast<std::size_t>(size)));
    input.remove_prefix(static_cast<std::size_t>(size));
    return result;
  };
  InterfaceType result;
  const auto count = number(MaxInterfaceNodes);
  for (std::uint64_t i = 0; ok && i < count; ++i) {
    InterfaceNode node;
    node.kind = static_cast<InterfaceKind>(
        number(static_cast<unsigned>(InterfaceKind::Array)));
    node.bytes = number(std::numeric_limits<std::int64_t>::max());
    node.alignment = number(std::numeric_limits<std::int64_t>::max());
    node.count = number(std::numeric_limits<std::int64_t>::max());
    node.element = static_cast<std::uint32_t>(number(MaxInterfaceNodes - 1));
    node.qualifiers = static_cast<unsigned>(number(7));
    node.variadic = number(1) != 0;
    node.prototype = number(1) != 0;
    node.name = text();
    node.view = text();
    node.typedefName = text();
    const auto parameters = number(MaxInterfaceFields);
    for (std::uint64_t j = 0; ok && j < parameters; ++j)
      node.parameters.push_back(
          static_cast<std::uint32_t>(number(MaxInterfaceNodes - 1)));
    const auto fields = number(MaxInterfaceFields);
    for (std::uint64_t j = 0; ok && j < fields; ++j) {
      InterfaceField field;
      field.name = text();
      field.type = static_cast<std::uint32_t>(number(MaxInterfaceNodes - 1));
      field.offset = number(std::numeric_limits<std::int64_t>::max());
      node.fields.push_back(std::move(field));
    }
    result.nodes.push_back(std::move(node));
  }
  return ok && input.empty() && result.valid() && result.encode() == original
             ? std::optional(std::move(result))
             : std::nullopt;
}

void mergeInterfaceTypes(InterfaceTypes &into, const InterfaceTypes &from) {
  for (const auto &[name, type] : from) {
    const auto [found, inserted] = into.try_emplace(name, type);
    if (!inserted && found->second != type)
      found->second.reset();
  }
}

} // namespace weavec::core
