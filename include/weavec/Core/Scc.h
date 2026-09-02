//===- Scc.h - Strongly connected components -------------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Tarjan's algorithm over a small adjacency list. Used to order functions
// callees-first within a translation unit (RFC 0003) and translation units
// dependencies-first within a program (RFC 0005).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_SCC_H
#define WEAVEC_CORE_SCC_H

#include <vector>

namespace weavec::core {

/// The strongly connected components of the directed graph `adjacency`
/// (node `i`'s successors are `adjacency[i]`), in reverse topological order
/// of the condensation: a component comes after every component it has an
/// edge to. Members within a component are sorted.
[[nodiscard]] std::vector<std::vector<unsigned>> stronglyConnectedComponents(
    const std::vector<std::vector<unsigned>> &adjacency);

} // namespace weavec::core

#endif // WEAVEC_CORE_SCC_H
