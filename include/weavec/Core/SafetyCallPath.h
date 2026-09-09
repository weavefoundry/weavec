//===- SafetyCallPath.h - Shared provenance (RFC 0020) ---------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef WEAVEC_CORE_SAFETYCALLPATH_H
#define WEAVEC_CORE_SAFETYCALLPATH_H

#include "weavec/Core/SourceLocation.h"

#include <initializer_list>
#include <memory>
#include <vector>

namespace weavec::core {

/// RFC 0020: read-only views of shared paths, with explicit detaching edits.
/// Normalization is cached only in immutable storage; no mutable references
/// or iterators escape and invalidate that marker behind another owner's back.
class SafetyCallPath {
public:
  using ConstIterator = std::vector<SourceLocation>::const_iterator;
  SafetyCallPath() = default;
  explicit SafetyCallPath(std::vector<SourceLocation> locations);
  SafetyCallPath(std::initializer_list<SourceLocation> locations);

  [[nodiscard]] const std::vector<SourceLocation> &entries() const;
  [[nodiscard]] bool empty() const { return entries().empty(); }
  [[nodiscard]] std::size_t size() const { return entries().size(); }
  [[nodiscard]] ConstIterator begin() const { return entries().begin(); }
  [[nodiscard]] ConstIterator end() const { return entries().end(); }
  [[nodiscard]] const SourceLocation &front() const {
    return entries().front();
  }
  [[nodiscard]] const SourceLocation &back() const { return entries().back(); }
  void pushBack(SourceLocation location);
  void resize(std::size_t count);
  void insert(ConstIterator position, SourceLocation location);
  void normalize();
  friend bool operator==(const SafetyCallPath &, const SafetyCallPath &);

private:
  struct Storage {
    std::vector<SourceLocation> locations;
    bool normalized = false;
  };
  std::shared_ptr<Storage> storage;
  std::vector<SourceLocation> &writable();
};

} // namespace weavec::core
#endif // WEAVEC_CORE_SAFETYCALLPATH_H
