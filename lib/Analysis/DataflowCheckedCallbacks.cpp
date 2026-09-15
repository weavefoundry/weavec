//===- DataflowCheckedCallbacks.cpp - Checked target joins (RFC 0022) -----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

#include <algorithm>

using namespace clang;

namespace weavec::analysis {

bool FunctionDataflow::checkedAlternatives(const CallExpr &call,
                                           const CallEffects &effects,
                                           core::AnalysisState &state) {
  const auto found = checkedCallAlternatives.find(&call);
  const auto targets = callTargetsSeen.find(&call);
  if (found == checkedCallAlternatives.end() || found->second.size() < 2 ||
      targets == callTargetsSeen.end() || targets->second.unknown ||
      targets->second.null ||
      found->second.size() != targets->second.functions.size())
    return false;

  std::vector<CheckedPost> posts;
  std::vector<CheckedPositionPost> positions;
  std::vector<CheckedProgressPost> progress;
  std::vector<ContainerPost> containers;
  std::vector<core::CheckedRequirement> tails;
  std::vector<core::CheckedRequirement> separation;
  std::vector<core::CheckedRequirement> footprints;
  bool readOnly = true;
  std::optional<core::AnalysisState> returning;
  const auto intersect = [](auto &common, const auto &alternative) {
    std::erase_if(common, [&](const auto &fact) {
      return std::ranges::find(alternative, fact) == alternative.end();
    });
  };
  for (const auto &[symbol, actual] : found->second) {
    auto input = state;
    auto targetEffects = effects;
    targetEffects.source = actual.source;
    targetEffects.summary = actual.summary;
    checkedCall(call, &targetEffects, input, symbol);
    // A target that cannot return still owes its input obligations, but it
    // cannot weaken guarantees about the state after a returning invocation.
    if (actual.summary->neverReturns)
      continue;
    auto outputs = checkedPosts[&call];
    if (const auto writes = checkedWrites.find(&call);
        writes != checkedWrites.end())
      for (const auto &memory : writes->second)
        outputs.push_back(
            {.path = core::SummaryPath::param(0),
             .range = {.begin = memory.begin,
                       .end = memory.end,
                       .zeroed =
                           symbol == "memset" && call.getNumArgs() > 1 &&
                           integerConstant(*call.getArg(1), context) == 0},
             .on = {},
             .storage = memory.storage,
             .objectType = {}});
    // Once an input object's identity is captured, its parameter spelling
    // does not distinguish the bytes established by two target contracts.
    for (auto &post : outputs)
      if (post.storage && !post.ifNonNull && !post.path.isResult())
        post.path = core::SummaryPath::param(0);
    // RFC 0027: invalidation is a may fact, while a structural or conservation
    // output must be guaranteed by every returning target. Each target captures
    // its actual input identities before its effects are joined.
    checkedContainersAfterCall(call, &targetEffects, input);
    readOnly &= containerReadOnlyCalls.contains(&call);
    if (!returning) {
      returning = std::move(input);
      posts = std::move(outputs);
      positions = checkedPositionPosts[&call];
      progress = checkedProgressPosts[&call];
      containers = containerPosts[&call];
      tails = containerTailPosts[&call];
      separation = containerSeparationPosts[&call];
      footprints = footprintPosts[&call];
    } else {
      returning->join(input, &places, false);
      intersect(posts, outputs);
      intersect(positions, checkedPositionPosts[&call]);
      intersect(progress, checkedProgressPosts[&call]);
      intersect(containers, containerPosts[&call]);
      intersect(tails, containerTailPosts[&call]);
      intersect(separation, containerSeparationPosts[&call]);
      intersect(footprints, footprintPosts[&call]);
    }
  }
  checkedWrites.erase(&call);
  checkedPosts[&call] = std::move(posts);
  checkedPositionPosts[&call] = std::move(positions);
  checkedProgressPosts[&call] = std::move(progress);
  checkedCallAssignedPointers.clear();
  containerReleases.erase(&call);
  containerPayloadReleases.erase(&call);
  containerPosts[&call] = std::move(containers);
  containerTailPosts[&call] = std::move(tails);
  containerSeparationPosts[&call] = std::move(separation);
  footprintPosts[&call] = std::move(footprints);
  if (!readOnly)
    containerReadOnlyCalls.erase(&call);
  if (returning)
    state = std::move(*returning);
  return true;
}

} // namespace weavec::analysis
