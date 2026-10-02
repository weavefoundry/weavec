//===- EngineContexts.cpp - Contexts across units (RFC 0031 §7) -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0031 §6.6, §7 *Amendment (cross-unit contexts)*: a call into another
// unit asks for the callee's summary in the call's context (the arguments
// it makes one object, the integers it knows, the callbacks it passes);
// the defining unit runs the context and exports its summary, and reports
// what an aliased context finds inside the callee.
//
//===----------------------------------------------------------------------===//

#include "Engine.h"
#include "weavec/Analysis/ClangLocation.h"
#include "weavec/Core/EffectsIO.h"

#include "clang/Basic/SourceManager.h"

#include "llvm/ADT/StringExtras.h"

#include <cstdlib>

using namespace clang;

namespace weavec::analysis::engine {

/// A function's portable name with the characters the key uses escaped.
static std::string escapeName(llvm::StringRef name) {
  std::string out;
  for (char c : name) {
    if (c == '%' || c == ' ' || c == ',' || c == ':' || c == '=' || c == ';') {
      static constexpr const char *Hex = "0123456789ABCDEF";
      out += '%';
      const unsigned byte = static_cast<unsigned char>(c);
      out += Hex[(byte >> 4U) & 0xFU];
      out += Hex[byte & 0xFU];
    } else {
      out += c;
    }
  }
  return out;
}

static std::optional<std::string> unescapeName(llvm::StringRef text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') {
      out += text[i];
      continue;
    }
    unsigned value = 0;
    if (i + 2 >= text.size() || text.substr(i + 1, 2).getAsInteger(16, value))
      return std::nullopt;
    out += static_cast<char>(value);
    i += 2;
  }
  return out;
}

std::string
contextKeyText(const AliasContext &context,
               const std::function<std::string(const VarDecl &)> &globalName) {
  // `a=<param>:<rep>:<offset>;…`, `c=<param>:<value>;…`,
  // `m=<param>:<offset>:<value>;…`, `f=<param>:<name>,<name>;…`, each
  // section only when it says something.
  std::string aliases;
  for (unsigned i = 0; i < context.params.size(); ++i)
    if (context.params[i].first != i)
      aliases += std::to_string(i) + ":" +
                 std::to_string(context.params[i].first) + ":" +
                 std::to_string(context.params[i].second) + ";";
  std::string constants;
  for (unsigned i = 0; i < context.constants.size(); ++i)
    if (context.constants[i])
      constants +=
          std::to_string(i) + ":" + std::to_string(*context.constants[i]) + ";";
  std::string cells;
  for (const auto &[param, offset, value] : context.cells)
    cells += std::to_string(param) + ":" + std::to_string(offset) + ":" +
             std::to_string(value) + ";";
  std::string callbacks;
  for (const auto &[param, names] : context.callbacks) {
    callbacks += std::to_string(param) + ":";
    for (std::size_t i = 0; i < names.size(); ++i)
      callbacks += (i == 0 ? "" : ",") + escapeName(names[i]);
    callbacks += ';';
  }
  std::string globals;
  for (const auto &[var, rep, offset] : context.globals)
    globals += escapeName(globalName(*var)) + ":" + std::to_string(rep) + ":" +
               std::to_string(offset) + ";";
  std::string out = "n=" + std::to_string(context.params.size());
  if (!aliases.empty())
    out += " a=" + aliases;
  if (!constants.empty())
    out += " c=" + constants;
  if (!cells.empty())
    out += " m=" + cells;
  if (!callbacks.empty())
    out += " f=" + callbacks;
  if (!globals.empty())
    out += " g=" + globals;
  return out;
}

std::optional<AliasContext> parseContextKey(
    llvm::StringRef text, unsigned params,
    const std::function<const VarDecl *(const std::string &)> &global) {
  AliasContext context;
  for (unsigned i = 0; i < params; ++i)
    context.params.emplace_back(i, 0);
  context.constants.assign(params, std::nullopt);
  llvm::SmallVector<llvm::StringRef, 8> sections;
  text.split(sections, ' ', -1, false);
  auto number = [](llvm::StringRef part, auto &value) {
    return !part.getAsInteger(10, value);
  };
  for (llvm::StringRef section : sections) {
    auto [name, body] = section.split('=');
    llvm::SmallVector<llvm::StringRef, 8> items;
    body.split(items, ';', -1, false);
    if (name == "n") {
      unsigned count = 0;
      if (!number(body, count) || count != params)
        return std::nullopt;
      continue;
    }
    for (llvm::StringRef item : items) {
      if (name == "g") {
        // `<global>:<rep>:<offset>`, the name first (escaped).
        auto [spelled, rest] = item.split(':');
        auto [repText, offsetText] = rest.split(':');
        auto decoded = unescapeName(spelled);
        unsigned rep = 0;
        std::int64_t offset = 0;
        if (!decoded || !number(repText, rep) || !number(offsetText, offset) ||
            rep >= params)
          return std::nullopt;
        // (A global the callee's unit does not see is no binding.)
        if (const VarDecl *var = global(*decoded))
          context.globals.emplace_back(var, rep, offset);
        continue;
      }
      llvm::SmallVector<llvm::StringRef, 4> fields;
      item.split(fields, ':', name == "f" ? 1 : -1, true);
      unsigned param = 0;
      if (fields.empty() || !number(fields[0], param) || param >= params)
        return std::nullopt;
      if (name == "a" && fields.size() == 3) {
        unsigned rep = 0;
        std::int64_t offset = 0;
        if (!number(fields[1], rep) || !number(fields[2], offset) ||
            rep >= param)
          return std::nullopt;
        context.params[param] = {rep, offset};
      } else if (name == "c" && fields.size() == 2) {
        std::int64_t value = 0;
        if (!number(fields[1], value))
          return std::nullopt;
        context.constants[param] = value;
      } else if (name == "m" && fields.size() == 3) {
        std::int64_t offset = 0;
        std::int64_t value = 0;
        if (!number(fields[1], offset) || !number(fields[2], value))
          return std::nullopt;
        context.cells.emplace_back(param, offset, value);
      } else if (name == "f" && fields.size() == 2) {
        llvm::SmallVector<llvm::StringRef, 4> names;
        fields[1].split(names, ',', -1, false);
        std::vector<std::string> decoded;
        for (llvm::StringRef each : names) {
          auto unescaped = unescapeName(each);
          if (!unescaped)
            return std::nullopt;
          decoded.push_back(std::move(*unescaped));
        }
        context.callbacks.emplace_back(param, std::move(decoded));
      } else {
        return std::nullopt;
      }
    }
  }
  return context;
}

std::string UnitRun::portableName(const FunctionDecl &fn) const {
  if (fn.isExternallyVisible())
    return fn.getNameAsString();
  const SourceManager &sm = context().getSourceManager();
  std::string source;
  if (const auto entry = sm.getFileEntryRefForID(sm.getMainFileID()))
    source = entry->getName().str();
  return source + "#" + fn.getNameAsString();
}

const VarDecl *UnitRun::globalNamed(const std::string &portable) const {
  if (!globalsByName) {
    globalsByName.emplace();
    for (const Decl *decl : context().getTranslationUnitDecl()->decls())
      if (const auto *var = dyn_cast<VarDecl>(decl);
          var != nullptr && var->hasGlobalStorage())
        globalsByName->try_emplace(portableName(*var), var->getCanonicalDecl());
  }
  auto it = globalsByName->find(portable);
  return it != globalsByName->end() ? it->second : nullptr;
}

const FunctionDecl *UnitRun::functionNamed(const std::string &portable) const {
  if (!functionsByName) {
    functionsByName.emplace();
    for (const Decl *decl : context().getTranslationUnitDecl()->decls())
      if (const auto *fn = dyn_cast<FunctionDecl>(decl);
          fn != nullptr && fn->getIdentifier() != nullptr) {
        // The definition when there is one, else a declaration.
        auto [it, inserted] =
            functionsByName->try_emplace(portableName(*fn), fn);
        if (!inserted && fn->doesThisDeclarationHaveABody())
          it->second = fn;
      }
  }
  auto it = functionsByName->find(portable);
  return it != functionsByName->end() ? it->second : nullptr;
}

void UnitRun::serveContextRequests(
    const std::function<bool(const FunctionDecl &)> &shouldReport) {
  // A callee's contexts, at most as many as a unit's own (§6.6), and only
  // of a callee whose own run was small, as a unit's own contexts
  // (`Transfer::contextSummary`): a context run costs what that run did,
  // twice (the caller keeps the callee's summary).
  static constexpr std::size_t MaxContextsPerCallee = 16;
  static constexpr std::uint64_t MaxCalleeTransfers = 64;
  if (input.database == nullptr)
    return;
  std::map<std::string, std::size_t> served;
  for (const ContextRequest &request : input.database->requests()) {
    const FunctionDecl *fn = functionNamed(request.callee);
    if (fn == nullptr || !hasBody(*fn))
      continue;
    if (auto cost = transfersOf.find(fn->getCanonicalDecl());
        cost != transfersOf.end() && cost->second > MaxCalleeTransfers)
      continue;
    if (++served[request.callee] > MaxContextsPerCallee)
      continue;
    std::optional<AliasContext> parsed = parseContextKey(
        request.key, fn->getNumParams(),
        [&](const std::string &name) { return globalNamed(name); });
    if (!parsed)
      continue;
    // The summary run: what the caller's unit instantiates.
    FunctionRun run(*this, *fn, discarding, RunMode::Summary, &*parsed, 1);
    RunResult result = run.run();
    if (!result.overBudget && !result.effects.incomplete)
      servedContexts[request] = std::move(result.effects);
    // §6.6: a context that makes arguments one object reports what that
    // finds inside the callee, here.
    if (parsed->trivial() || !shouldReport(*fn))
      continue;
    LedgerAdapter collector(context(), LedgerAdapter::Mode::Collecting);
    FunctionRun checked(*this, *fn, collector, RunMode::Context, &*parsed, 1);
    (void)checked.run();
    for (core::Diagnostic diagnostic : collector.diagnostics()) {
      if (diagnostic.id != core::diag::UseAfterFree &&
          diagnostic.id != core::diag::DoubleFree &&
          diagnostic.id != core::diag::UseAfterMove)
        continue;
      core::Certainty certainty = diagnostic.certainty;
      diagnostic.addNote(
          "called from another unit with related pointer "
          "arguments",
          toCoreLocation(context().getSourceManager(), fn->getLocation()));
      authoritative.report(std::move(diagnostic), certainty);
    }
  }
}

} // namespace weavec::analysis::engine
