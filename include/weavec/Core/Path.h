//===- Path.h - Places relative to a function's interface -------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A *summary path* names a place a function can see from the outside: a
// parameter, a global or the result, followed by field, dereference and
// index steps (RFC 0003, RFC 0031 §1).
//
//   root ::= param(i) | global(g) | result
//   path ::= root ('*' | '.' field | '[' selector ']')*
//
// Summaries (`Effects.h`), boundary facts and pointer kinds name places this
// way. Roots are integers: the Analysis layer resolves them against a call's
// arguments or a unit's globals.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_PATH_H
#define WEAVEC_CORE_PATH_H

#include <compare>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::core {

/// `a == b` for the short names of path steps, compared in line: a call to
/// `memcmp` per name dominated the comparisons of paths.
[[nodiscard]] inline bool sameText(std::string_view a,
                                   std::string_view b) noexcept {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (a[i] != b[i])
      return false;
  return true;
}

/// `a <=> b` as `std::string` orders them (bytes as unsigned characters,
/// then length), compared in line (see `sameText`).
[[nodiscard]] inline std::strong_ordering
compareText(std::string_view a, std::string_view b) noexcept {
  const std::size_t common = a.size() < b.size() ? a.size() : b.size();
  for (std::size_t i = 0; i < common; ++i)
    if (a[i] != b[i])
      return static_cast<unsigned char>(a[i]) <=>
             static_cast<unsigned char>(b[i]);
  return a.size() <=> b.size();
}

/// One step of a path.
enum class PathStep : std::uint8_t {
  /// `parent.field` (or `parent->field` when the parent is a dereference).
  Field,
  /// `*parent`: the object the pointer stored in `parent` refers to.
  Deref,
  /// An element: every element (empty selector) or a named one.
  Index,
};

/// What a summary path is rooted at.
enum class SummaryRoot : std::uint8_t {
  /// The `index`-th parameter of the function.
  Param,
  /// A global variable, identified by an id interned per translation unit.
  Global,
  /// The returned value (RFCs 0008 and 0013). Record fields use `.field`;
  /// pointer-result heap fields use `*.field`. `index` is always zero.
  Result,
};

/// One step below a root, with its field name or selector. An anonymous
/// member is spelled by its index, `#2` (RFC 0031 §7).
struct PathElem {
  PathStep step = PathStep::Field;
  std::string field;

  friend bool operator==(const PathElem &a, const PathElem &b) noexcept {
    return a.step == b.step && sameText(a.field, b.field);
  }
  friend std::strong_ordering operator<=>(const PathElem &a,
                                          const PathElem &b) noexcept {
    if (const auto order = a.step <=> b.step; std::is_neq(order))
      return order;
    return compareText(a.field, b.field);
  }
};

/// The steps of a path.
class PathSteps {
public:
  using ConstIterator = std::vector<PathElem>::const_iterator;
  PathSteps() = default;
  PathSteps(std::initializer_list<PathElem> elements) : elements(elements) {}

  [[nodiscard]] std::size_t size() const noexcept { return elements.size(); }
  [[nodiscard]] bool empty() const noexcept { return elements.empty(); }
  [[nodiscard]] ConstIterator begin() const { return elements.begin(); }
  [[nodiscard]] ConstIterator end() const { return elements.end(); }
  [[nodiscard]] const PathElem *data() const { return elements.data(); }
  [[nodiscard]] const PathElem &front() const { return elements.front(); }
  [[nodiscard]] const PathElem &back() const { return elements.back(); }
  [[nodiscard]] const PathElem &operator[](std::size_t index) const {
    return elements[index];
  }
  void pushBack(PathElem element) { elements.push_back(std::move(element)); }
  void pushFront(PathElem element) {
    elements.insert(elements.begin(), std::move(element));
  }
  void popBack() { elements.pop_back(); }
  void truncate(std::size_t size) {
    if (size < elements.size())
      elements.resize(size);
  }
  void append(const PathSteps &other, std::size_t first = 0) {
    if (first < other.elements.size())
      elements.insert(elements.end(),
                      other.elements.begin() +
                          static_cast<std::ptrdiff_t>(first),
                      other.elements.end());
  }

  friend bool operator==(const PathSteps &, const PathSteps &) = default;
  friend std::strong_ordering operator<=>(const PathSteps &a,
                                          const PathSteps &b) {
    return std::lexicographical_compare_three_way(
        a.elements.begin(), a.elements.end(), b.elements.begin(),
        b.elements.end());
  }

private:
  std::vector<PathElem> elements;
};

/// RFC 0014: two pointer parameters that compare equal (or not).
struct ParamPairTest {
  std::uint32_t first = 0;
  std::uint32_t second = 0;
  bool equal = true;

  friend bool operator==(const ParamPairTest &,
                         const ParamPairTest &) = default;
  friend auto operator<=>(const ParamPairTest &,
                          const ParamPairTest &) = default;
};

/// A place relative to a function's interface: `param(0)`, `param(0)*`,
/// `param(0)*.data`, `global(3)`.
struct SummaryPath {
  SummaryRoot root = SummaryRoot::Param;
  std::uint32_t index = 0;
  PathSteps steps;

  [[nodiscard]] static SummaryPath param(std::uint32_t index) {
    return SummaryPath{.root = SummaryRoot::Param, .index = index, .steps = {}};
  }
  [[nodiscard]] static SummaryPath global(std::uint32_t id) {
    return SummaryPath{.root = SummaryRoot::Global, .index = id, .steps = {}};
  }
  [[nodiscard]] static SummaryPath result() {
    return SummaryPath{.root = SummaryRoot::Result, .index = 0, .steps = {}};
  }

  [[nodiscard]] SummaryPath deref() const;
  [[nodiscard]] SummaryPath field(std::string_view name) const;
  [[nodiscard]] SummaryPath indexed(std::string_view selector = {}) const;

  [[nodiscard]] bool isRoot() const noexcept { return steps.empty(); }
  [[nodiscard]] bool isParam() const noexcept {
    return root == SummaryRoot::Param;
  }
  [[nodiscard]] bool isGlobal() const noexcept {
    return root == SummaryRoot::Global;
  }
  [[nodiscard]] bool isResult() const noexcept {
    return root == SummaryRoot::Result;
  }
  /// True if `this` is a proper prefix of `other` (same root, fewer steps).
  [[nodiscard]] bool isProperPrefixOf(const SummaryPath &other) const;
  /// True if any step is a dereference: the path names caller memory rather
  /// than the callee's private copy of an argument.
  [[nodiscard]] bool hasDeref() const noexcept;
  /// The root path (`param(i)` / `global(g)`).
  [[nodiscard]] SummaryPath rootPath() const {
    return SummaryPath{.root = root, .index = index, .steps = {}};
  }

  /// Spells the path given the display name of the root: `p`, `*p`,
  /// `p->data`, `a[*]`.
  [[nodiscard]] std::string toString(std::string_view rootName) const;

  friend bool operator==(const SummaryPath &, const SummaryPath &) = default;
  friend std::strong_ordering operator<=>(const SummaryPath &,
                                          const SummaryPath &) = default;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_PATH_H
