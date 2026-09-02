//===- Summaries.cpp - Function summaries for Clang declarations ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/Summaries.h"

#include "weavec/Analysis/Annotations.h"

#include "llvm/ADT/STLExtras.h"

#include <utility>

using namespace clang;

namespace weavec::analysis {

// -- GlobalTable --------------------------------------------------------------

std::uint32_t GlobalTable::idFor(const VarDecl &var) {
  const VarDecl *canonical = var.getCanonicalDecl();
  const auto [it, inserted] =
      ids.try_emplace(canonical, static_cast<std::uint32_t>(decls.size()));
  if (inserted)
    decls.push_back(canonical);
  return it->second;
}

const VarDecl *GlobalTable::declFor(std::uint32_t id) const noexcept {
  return id < decls.size() ? decls[id] : nullptr;
}

llvm::StringRef GlobalTable::nameOf(std::uint32_t id) const {
  const VarDecl *decl = declFor(id);
  return decl == nullptr ? llvm::StringRef("<global>") : decl->getName();
}

// -- Annotations --------------------------------------------------------------

bool SignatureAnnotations::anyOwnership() const noexcept {
  const auto ownership = [](const AnnotationSet &set) {
    return set.owned || set.borrowed || set.mutBorrowed;
  };
  return ownership(result) || llvm::any_of(params, ownership);
}

static void merge(AnnotationSet &into, const AnnotationSet &from) {
  into.owned = into.owned || from.owned;
  into.borrowed = into.borrowed || from.borrowed;
  into.mutBorrowed = into.mutBorrowed || from.mutBorrowed;
  into.unsafe = into.unsafe || from.unsafe;
  into.invalid = into.invalid || from.invalid;
}

SignatureAnnotations collectAnnotations(const FunctionDecl &function) {
  SignatureAnnotations collected;
  collected.params.resize(function.getNumParams());
  for (const FunctionDecl *redecl : function.redecls()) {
    const AnnotationSet onFunction = getAnnotations(*redecl);
    merge(collected.result, onFunction);
    collected.unsafe = collected.unsafe || onFunction.unsafe;
    for (unsigned i = 0;
         i < redecl->getNumParams() && i < collected.params.size(); ++i)
      merge(collected.params[i], getAnnotations(*redecl->getParamDecl(i)));
  }
  return collected;
}

bool hasOwnershipAnnotations(const FunctionDecl &function) {
  return collectAnnotations(function).anyOwnership();
}

core::FunctionSummary summaryFromAnnotations(const FunctionDecl &function) {
  core::FunctionSummary summary;
  const SignatureAnnotations annotations = collectAnnotations(function);
  for (unsigned i = 0; i < annotations.params.size(); ++i) {
    if (!function.getParamDecl(i)->getType()->isPointerType())
      continue;
    const AnnotationSet &set = annotations.params[i];
    const core::SummaryPath root = core::SummaryPath::param(i);
    if (set.owned)
      summary.addEffect(root, core::PlaceEffect{.moved = true});
    else if (set.mutBorrowed)
      summary.addEffect(root.deref(), core::PlaceEffect{.written = true});
    else if (set.borrowed)
      summary.addEffect(root.deref(), core::PlaceEffect{.read = true});
  }
  if (function.getReturnType()->isPointerType()) {
    if (annotations.result.owned)
      summary.addReturn(core::ValueSource::fresh());
    else if (annotations.result.borrowed || annotations.result.mutBorrowed)
      summary.addReturn(core::ValueSource::unknown());
  }
  return summary;
}

/// Replaces the inferred facts about every annotated root with what the
/// annotation says (RFC 0003: annotations are authoritative per root).
static void applyAnnotations(core::FunctionSummary &summary,
                             const FunctionDecl &function,
                             const SignatureAnnotations &annotations) {
  const auto eraseRoot = [&summary](unsigned index, bool includeStores) {
    for (auto it = summary.effects.begin(); it != summary.effects.end();) {
      if (it->first.isParam() && it->first.index == index)
        it = summary.effects.erase(it);
      else
        ++it;
    }
    if (!includeStores)
      return;
    for (auto it = summary.stores.begin(); it != summary.stores.end();) {
      if (it->dest.isParam() && it->dest.index == index)
        it = summary.stores.erase(it);
      else
        ++it;
    }
  };

  for (unsigned i = 0; i < annotations.params.size(); ++i) {
    if (!function.getParamDecl(i)->getType()->isPointerType())
      continue;
    const AnnotationSet &set = annotations.params[i];
    const core::SummaryPath root = core::SummaryPath::param(i);
    if (set.owned) {
      // Whatever happens to a consumed object is the callee's business.
      eraseRoot(i, /*includeStores=*/true);
      summary.addEffect(root, core::PlaceEffect{.moved = true});
    } else if (set.mutBorrowed) {
      eraseRoot(i, /*includeStores=*/false);
      summary.addEffect(root.deref(), core::PlaceEffect{.written = true});
    } else if (set.borrowed) {
      eraseRoot(i, /*includeStores=*/false);
      summary.addEffect(root.deref(), core::PlaceEffect{.read = true});
    }
  }

  if (!function.getReturnType()->isPointerType())
    return;
  if (annotations.result.owned) {
    summary.returns.clear();
    summary.addReturn(core::ValueSource::fresh());
  } else if (annotations.result.borrowed || annotations.result.mutBorrowed) {
    // The signature promises a borrow: a fresh allocation the body may
    // return is a reported mismatch, and callers must not treat it as owned.
    summary.returns.erase(core::ValueSource::fresh());
    if (summary.returns.empty())
      summary.addReturn(core::ValueSource::unknown());
  }
}

// -- SummaryStore -------------------------------------------------------------

bool SummaryStore::setInferred(const FunctionDecl &function,
                               core::FunctionSummary summary) {
  const FunctionDecl *canonical = key(function);
  merged.erase(canonical);
  auto [it, inserted] = inferred.try_emplace(canonical, std::move(summary));
  if (inserted)
    return true;
  if (it->second == summary)
    return false;
  it->second = std::move(summary);
  return true;
}

const core::FunctionSummary *
SummaryStore::inferredFor(const FunctionDecl &function) const {
  const auto it = inferred.find(key(function));
  return it == inferred.end() ? nullptr : &it->second;
}

std::optional<ResolvedSummary>
SummaryStore::lookup(const FunctionDecl &callee) {
  const FunctionDecl *canonical = key(callee);
  if (const auto it = merged.find(canonical); it != merged.end()) {
    return ResolvedSummary{.summary = &it->second,
                           .source = hasOwnershipAnnotations(callee)
                                         ? SummarySource::Annotation
                                         : SummarySource::Inferred};
  }

  const SignatureAnnotations annotations = collectAnnotations(callee);
  const core::FunctionSummary *body = inferredFor(callee);
  // `WEAVEC_UNSAFE` on a definition means "the signature is the contract":
  // the body is not analysed and the user has opted out, so an empty
  // summary is what they asked for rather than something to warn about.
  const bool annotated = annotations.anyOwnership() || annotations.unsafe;

  if (body == nullptr && !annotated) {
    if (const core::FunctionSummary *builtin = builtinSummary(callee))
      return ResolvedSummary{.summary = builtin,
                             .source = SummarySource::Builtin};
    return std::nullopt;
  }

  core::FunctionSummary result =
      body != nullptr ? *body : core::FunctionSummary{};
  if (annotated)
    applyAnnotations(result, callee, annotations);
  const auto [it, inserted] = merged.try_emplace(canonical, std::move(result));
  return ResolvedSummary{.summary = &it->second,
                         .source = annotated ? SummarySource::Annotation
                                             : SummarySource::Inferred};
}

bool SummaryStore::noteUnknownCallee(const FunctionDecl &callee) {
  return unknownCallees.insert(key(callee)).second;
}

} // namespace weavec::analysis
