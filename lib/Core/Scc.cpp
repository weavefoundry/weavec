//===- Scc.cpp - Strongly connected components ----------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Scc.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace weavec::core {

namespace {

/// Iterative Tarjan: emits components in reverse topological order of the
/// condensation.
class SccFinder {
public:
  explicit SccFinder(const std::vector<std::vector<unsigned>> &adjacency)
      : graph(adjacency), index(adjacency.size(), Unvisited),
        low(adjacency.size(), 0), onStack(adjacency.size(), false) {}

  std::vector<std::vector<unsigned>> run() {
    for (unsigned root = 0; root < graph.size(); ++root) {
      if (index[root] == Unvisited)
        visit(root);
    }
    return components;
  }

private:
  static constexpr unsigned Unvisited = ~0U;

  const std::vector<std::vector<unsigned>> &graph;
  std::vector<unsigned> index;
  std::vector<unsigned> low;
  std::vector<bool> onStack;
  std::vector<unsigned> stack;
  std::vector<std::vector<unsigned>> components;
  unsigned counter = 0;

  void visit(unsigned root) {
    struct Frame {
      unsigned node;
      std::size_t nextEdge;
    };
    std::vector<Frame> frames{Frame{.node = root, .nextEdge = 0}};
    enter(root);
    while (!frames.empty()) {
      Frame &frame = frames.back();
      const unsigned node = frame.node;
      if (frame.nextEdge < graph[node].size()) {
        const unsigned succ = graph[node][frame.nextEdge++];
        if (index[succ] == Unvisited) {
          enter(succ);
          frames.push_back(Frame{.node = succ, .nextEdge = 0});
        } else if (onStack[succ]) {
          low[node] = std::min(low[node], index[succ]);
        }
        continue;
      }
      if (low[node] == index[node]) {
        std::vector<unsigned> component;
        unsigned member = 0;
        do {
          member = stack.back();
          stack.pop_back();
          onStack[member] = false;
          component.push_back(member);
        } while (member != node);
        std::ranges::sort(component);
        components.push_back(std::move(component));
      }
      frames.pop_back();
      if (!frames.empty()) {
        const unsigned parent = frames.back().node;
        low[parent] = std::min(low[parent], low[node]);
      }
    }
  }

  void enter(unsigned node) {
    index[node] = low[node] = counter++;
    stack.push_back(node);
    onStack[node] = true;
  }
};

} // namespace

std::vector<std::vector<unsigned>> stronglyConnectedComponents(
    const std::vector<std::vector<unsigned>> &adjacency) {
  return SccFinder(adjacency).run();
}

} // namespace weavec::core
