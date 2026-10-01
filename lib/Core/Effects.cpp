//===- Effects.cpp - Format-30 function summaries (RFC 0031) --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Effects.h"

namespace weavec::core {

/// A path with its root spelled: `paramN`, `globalN` or `result`.
static std::string pathText(const SummaryPath &path) {
  switch (path.root) {
  case SummaryRoot::Param:
    return path.toString("param" + std::to_string(path.index));
  case SummaryRoot::Global:
    return path.toString("global" + std::to_string(path.index));
  case SummaryRoot::Result:
    return path.toString("result");
  }
  return path.toString("?");
}

std::string_view toString(ResultClass value) noexcept {
  switch (value) {
  case ResultClass::Null:
    return "null";
  case ResultClass::NonNull:
    return "nonnull";
  case ResultClass::Zero:
    return "zero";
  case ResultClass::Positive:
    return "positive";
  case ResultClass::Negative:
    return "negative";
  }
  return "?";
}

std::optional<ResultClass> parseResultClass(std::string_view text) {
  for (ResultClass value :
       {ResultClass::Null, ResultClass::NonNull, ResultClass::Zero,
        ResultClass::Positive, ResultClass::Negative})
    if (toString(value) == text)
      return value;
  return std::nullopt;
}

/// `null|nonnull`, for the store text.
static std::string classesText(const std::vector<ResultClass> &classes) {
  std::string out;
  for (std::size_t i = 0; i < classes.size(); ++i)
    out += (i == 0 ? "" : "|") + std::string(core::toString(classes[i]));
  return out;
}

std::string EffectCase::toString() const {
  if (always())
    return "always";
  std::string out;
  if (!classes.empty()) {
    out = "result";
    for (std::size_t i = 0; i < classes.size(); ++i) {
      out += i == 0 ? " " : "|";
      out += core::toString(classes[i]);
    }
  }
  if (paramZero)
    out += std::string(out.empty() ? "" : " and ") + "param " +
           std::to_string(paramZero->first) +
           (paramZero->second ? " =0" : " !=0");
  if (entryZero)
    out += std::string(out.empty() ? "" : " and ") + "entry " +
           pathText(entryZero->first) + (entryZero->second ? " =0" : " !=0");
  if (paramsEqual)
    out += std::string(out.empty() ? "" : " and ") + "param " +
           std::to_string(paramsEqual->first) +
           (paramsEqual->equal ? " == " : " != ") + "param " +
           std::to_string(paramsEqual->second);
  return out;
}

std::string PathTerm::toString() const {
  if (!path)
    return std::to_string(constant);
  std::string out = pathText(*path);
  if (scale != 1)
    out += " scale " + std::to_string(scale);
  if (constant != 0)
    out += " plus " + std::to_string(constant);
  return out;
}

std::string ElementRange::toString() const {
  return "[" + from.toString() + ", " + to.toString() + ")";
}

std::string ValueDesc::toString() const {
  switch (kind) {
  case Kind::Null:
    return "null";
  case Kind::Fresh:
    return "fresh#" + std::to_string(object) + " " +
           (family.empty() ? std::string("free") : family) +
           (extent ? " extent " + extent->toString() : std::string()) +
           (zeroed ? " zeroed" : "") + (many ? " many" : "") +
           (maybeNull ? " maybe-null" : "") + (interior ? " interior" : "") +
           (offset && *offset != 0 ? " offset " + std::to_string(*offset)
                                   : std::string());
  case Kind::Path:
    return "path " + (path ? pathText(*path) : std::string("?")) +
           (offset && *offset != 0 ? " offset " + std::to_string(*offset)
                                   : std::string()) +
           (maybeNull ? " maybe-null" : "");
  case Kind::Static:
    return "static";
  case Kind::Int:
    return "int [" + (lo ? std::to_string(*lo) : std::string("-inf")) + ", " +
           (hi ? std::to_string(*hi) : std::string("inf")) + "]" +
           (range ? " " + range->toString() : std::string());
  case Kind::Dangling:
    return "dangling";
  case Kind::Unknown:
    return raw ? (rawSome ? "unknown raw-some" : "unknown raw") : "unknown";
  case Kind::Function: {
    std::string out = "function";
    for (std::size_t i = 0; i < functions.size(); ++i)
      out += (i == 0 ? " " : ",") + functions[i];
    return out;
  }
  }
  return "?";
}

std::string toText(const FunctionEffects &effects) {
  std::string out;
  switch (effects.returns) {
  case FunctionEffects::Returns::Always:
    out += "  always-returns\n";
    break;
  case FunctionEffects::Returns::May:
    out += "  may-not-return\n";
    break;
  case FunctionEffects::Returns::Never:
    out += "  never-returns\n";
    break;
  }
  if (effects.incomplete)
    out += "  incomplete " + *effects.incomplete + "\n";
  if (effects.unknownGlobals)
    out += "  unknown globals\n";
  for (const ResultEffect &result : effects.results) {
    out += "  result " + result.value.toString();
    if (result.value.maybeNull && result.value.kind != ValueDesc::Kind::Fresh)
      out += " maybe-null";
    out += " when";
    for (ResultClass c : result.classes)
      out += " " + std::string(toString(c));
    if (result.paramZero)
      out += " and param " + std::to_string(result.paramZero->first) +
             (result.paramZero->second ? " =0" : " !=0");
    out += "\n";
  }
  for (const PathEffect &effect : effects.effects) {
    static constexpr const char *Names[] = {"release", "move",     "unknown",
                                            "escape",  "share -1", "share +1"};
    out += "  " + std::string(Names[static_cast<int>(effect.kind)]) + " " +
           pathText(effect.path);
    if (effect.elements)
      out += " elements " + effect.elements->toString();
    if (!effect.family.empty())
      out += " " + effect.family;
    if (effect.anyOffset)
      out += " offset unknown";
    else if (effect.offset != 0)
      out += " offset " + std::to_string(effect.offset);
    if (effect.lossy)
      out += " lossy";
    if (effect.may)
      out += " may";
    out += " when " + effect.when.toString() + "\n";
  }
  for (const StoreEffect &store : effects.stores)
    out +=
        "  store " + std::string(store.contents ? "(new) " : "") +
        pathText(store.dest) +
        (store.elements ? " elements " + store.elements->toString()
                        : std::string()) +
        (store.bytes ? " bytes " + std::to_string(store.bytes->first) + ".." +
                           std::to_string(store.bytes->second)
                     : std::string()) +
        " := " + store.value.toString() + (store.may ? " may" : "") +
        (store.when.always() ? std::string()
                             : " when " + store.when.toString()) +
        (store.absentOn.empty() ? std::string()
                                : " absent on " + classesText(store.absentOn)) +
        "\n";
  for (const auto &[resultClass, paths] : effects.nonNullOn)
    for (const SummaryPath &path : paths)
      out += "  nonnull-on " + std::string(toString(resultClass)) + " " +
             pathText(path) + "\n";
  for (const StringEffect &string : effects.strings)
    out += "  string " + std::string(string.contents ? "(new) " : "") +
           pathText(string.path) + " nul-within " +
           string.nulWithin.toString() +
           (string.nulFrom ? " from " + string.nulFrom->toString()
                           : std::string()) +
           "\n";
  return out;
}

} // namespace weavec::core
