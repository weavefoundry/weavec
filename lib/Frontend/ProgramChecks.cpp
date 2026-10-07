//===- ProgramChecks.cpp - What a whole program checks --------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/ProgramChecks.h"

#include "weavec/Frontend/ClangDiagnosticSink.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"

#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace weavec::frontend {

//===----------------------------------------------------------------------===//
// Paths
//===----------------------------------------------------------------------===//

/// `path` taken against `base` when relative, without `.` and `..`.
static std::string absoluteFrom(llvm::StringRef path, llvm::StringRef base) {
  if (path.empty())
    return {};
  llvm::SmallString<256> absolute(path);
  if (!llvm::sys::path::is_absolute(absolute)) {
    llvm::SmallString<256> joined(base);
    if (joined.empty())
      std::ignore = llvm::sys::fs::current_path(joined);
    llvm::sys::path::append(joined, path);
    absolute = std::move(joined);
  }
  llvm::sys::path::remove_dots(absolute, /*remove_dot_dot=*/true);
  return absolute.str().str();
}

std::string displayPath(llvm::StringRef path, llvm::StringRef base,
                        llvm::StringRef cwd) {
  if (path.empty())
    return {};
  std::string absolute = absoluteFrom(path, base);
  llvm::SmallString<256> directory(cwd);
  if (directory.empty())
    std::ignore = llvm::sys::fs::current_path(directory);
  const std::string root = absoluteFrom(directory, directory);
  llvm::StringRef prefix = root;
  if (prefix.size() > 1 && prefix.ends_with("/"))
    prefix = prefix.drop_back();
  const llvm::StringRef full = absolute;
  if (full == prefix)
    return ".";
  if (prefix == "/")
    return full.drop_front().str();
  if (full.starts_with(prefix) && full.size() > prefix.size() &&
      full[prefix.size()] == '/')
    return full.drop_front(prefix.size() + 1).str();
  return absolute;
}

//===----------------------------------------------------------------------===//
// Slots and boundaries
//===----------------------------------------------------------------------===//

core::SlotSolution solveProgramSlots(std::span<const ProgramMember> members,
                                     bool executable) {
  core::FnSlots constraints;
  core::SlotRules rules;
  rules.scope = core::SlotScope::Program;
  rules.executable = executable;
  for (const ProgramMember &member : members) {
    const SlotFacts &slots = member.facts.slots;
    constraints.merge(core::FnSlots::fromRows(slots.rows));
    rules.defined.insert(slots.defined.begin(), slots.defined.end());
    rules.exported.insert(slots.exported.begin(), slots.exported.end());
    rules.confinedRecords.insert(slots.confinedRecords.begin(),
                                 slots.confinedRecords.end());
    rules.escapedStatics.insert(slots.escapedStatics.begin(),
                                slots.escapedStatics.end());
  }
  return constraints.solve(rules);
}

std::vector<analysis::BoundaryRow>
programBoundaries(std::span<const ProgramMember> members) {
  std::vector<analysis::BoundaryRow> rows;
  for (const ProgramMember &member : members) {
    for (analysis::BoundaryRow row : member.facts.boundaries) {
      row.unit = member.source;
      rows.push_back(std::move(row));
    }
  }
  std::ranges::sort(rows);
  return rows;
}

void dumpProgramSlots(const core::SlotSolution &slots, llvm::raw_ostream &os) {
  os << "program slots:\n";
  for (const core::SlotKey &slot : slots.slots()) {
    const std::set<std::string> &targets = slots.targets(slot);
    const std::optional<core::OpenSource> open = slots.openSource(slot);
    if (slot.isLocal() || (targets.empty() && !open))
      continue;
    os << "  " << slot.toString() << ": {";
    bool first = true;
    for (const std::string &target : targets) {
      os << (first ? "" : ", ") << target;
      first = false;
    }
    os << "} ";
    if (open)
      os << "open (" << open->detail << ")";
    else
      os << "closed";
    os << '\n';
  }
}

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

/// The member that defines `name` with external linkage, other than
/// `except`.
static std::optional<std::size_t>
definerOf(std::span<const ProgramMember> members, const std::string &name,
          std::size_t except) {
  for (std::size_t m = 0; m < members.size(); ++m) {
    if (m == except)
      continue;
    const auto &functions = members[m].exports.functions;
    const auto it = functions.find(name);
    if (it != functions.end() && it->second.external)
      return m;
  }
  return std::nullopt;
}

/// What a definition's summary (RFC 0031 §6) says about argument `param`.
static bool hasEffect(const core::FunctionEffects &effects,
                      core::PathEffect::Kind kind, std::uint32_t param) {
  const core::SummaryPath object = core::SummaryPath::param(param).deref();
  return std::ranges::any_of(effects.effects, [&](const core::PathEffect &e) {
    return e.kind == kind && e.path == object;
  });
}

/// Whether the callee keeps a copy of argument `param` in memory the caller
/// can reach after the call.
static bool storesArgument(const core::FunctionEffects &effects,
                           std::uint32_t param) {
  const core::SummaryPath root = core::SummaryPath::param(param);
  if (hasEffect(effects, core::PathEffect::Kind::Escape, param))
    return true;
  return std::ranges::any_of(effects.stores, [&](const core::StoreEffect &s) {
    return s.value.kind == core::ValueDesc::Kind::Path && s.value.path &&
           *s.value.path == root;
  });
}

/// The result's non-null alternatives are all new objects.
static bool returnsOnlyFresh(const core::FunctionEffects &effects) {
  bool any = false;
  for (const core::ResultEffect &result : effects.results) {
    if (result.value.kind == core::ValueDesc::Kind::Null)
      continue;
    if (result.value.kind != core::ValueDesc::Kind::Fresh)
      return false;
    any = true;
  }
  return any;
}

/// The result may point into memory the caller already had.
static bool returnsBorrowed(const core::FunctionEffects &effects) {
  return std::ranges::any_of(effects.results, [](const core::ResultEffect &r) {
    return r.value.kind == core::ValueDesc::Kind::Path ||
           r.value.kind == core::ValueDesc::Kind::Static;
  });
}

static bool mayReturnNull(const core::FunctionEffects &effects) {
  return std::ranges::any_of(effects.results, [](const core::ResultEffect &r) {
    return r.value.kind == core::ValueDesc::Kind::Null || r.value.maybeNull;
  });
}

/// Not named `quoted`: with a `std::string_view` argument, argument-dependent
/// lookup would also find `std::quoted`, which returns a stream manipulator.
static std::string quotedName(std::string_view text) {
  return "'" + std::string(text) + "'";
}

/// The spelling of parameter `index` in messages.
static std::string parameterName(const DeclaredInterface &declared,
                                 std::size_t index) {
  if (index < declared.params.size() && !declared.params[index].name.empty())
    return quotedName(declared.params[index].name);
  return "parameter " + std::to_string(index + 1);
}

namespace {

/// One contradiction between a declaration and its definition: what the
/// declaration states and what the definition does.
struct Finding {
  std::string declared;
  std::string done;
};

} // namespace

/// §13.2 step 3 for one import against its definition.
static std::vector<Finding>
contradictions(const ImportInterface &import,
               const analysis::ExportedFunction &function,
               const FunctionInterface *definition) {
  std::vector<Finding> findings;
  const core::FunctionEffects &summary = function.effects;
  const DeclaredInterface &declared = import.declared;
  const auto definedKind =
      [&](std::size_t index) -> std::optional<core::PointerKind> {
    if (definition == nullptr || index >= definition->params.size() ||
        !definition->params[index])
      return std::nullopt;
    auto kind = parseKind(*definition->params[index]);
    if (!kind || kind->source != core::KindSource::Declared)
      return std::nullopt;
    return kind;
  };
  for (std::size_t i = 0; i < declared.params.size(); ++i) {
    const DeclaredParam &param = declared.params[i];
    const auto index = static_cast<std::uint32_t>(i);
    const std::string name = parameterName(declared, i);
    if (param.ownership == "WEAVEC_BORROWED" ||
        param.ownership == "WEAVEC_MUT") {
      std::string verb;
      if (hasEffect(summary, core::PathEffect::Kind::Release, index))
        verb = "frees";
      else if (hasEffect(summary, core::PathEffect::Kind::Move, index))
        verb = "takes ownership of";
      else if (storesArgument(summary, index))
        verb = "stores";
      if (!verb.empty()) {
        verb.append(" ").append(name);
        findings.push_back(
            Finding{.declared = *param.ownership, .done = std::move(verb)});
      }
    }
    if (!param.kind)
      continue;
    const auto stated = core::PointerKind::parse(*param.kind);
    const auto defined = definedKind(i);
    if (!stated || !defined)
      continue;
    const bool shape = defined->shape != core::PointerShape::Unknown &&
                       stated->shape != core::PointerShape::Unknown &&
                       !stated->sameShape(*defined);
    const bool nullability =
        defined->nullability == core::Nullability::Nonnull &&
        stated->nullability == core::Nullability::Nullable;
    if (shape || nullability)
      findings.push_back(
          Finding{.declared = "with " + name + " " + *param.kind,
                  .done = "declares " + name + " " + defined->toString()});
  }
  if (declared.ownership == "WEAVEC_OWNED") {
    if (returnsBorrowed(summary))
      findings.push_back(Finding{.declared = "WEAVEC_OWNED",
                                 .done = "returns a borrowed pointer"});
  } else if ((declared.ownership == "WEAVEC_BORROWED" ||
              declared.ownership == "WEAVEC_MUT") &&
             returnsOnlyFresh(summary)) {
    findings.push_back(Finding{.declared = *declared.ownership,
                               .done = "returns a fresh allocation"});
  }
  if (declared.result) {
    const auto stated = core::PointerKind::parse(*declared.result);
    if (stated && stated->nullability == core::Nullability::Nonnull &&
        mayReturnNull(summary))
      findings.push_back(Finding{.declared = "to return " + *declared.result,
                                 .done = "may return null"});
  }
  return findings;
}

static core::SourceLocation absoluteLocation(core::SourceLocation location,
                                             llvm::StringRef base) {
  location.file = absoluteFrom(location.file, base);
  location.opaque = 0;
  return location;
}

std::vector<core::Diagnostic>
verifyDeclarations(std::span<const ProgramMember> members,
                   llvm::StringRef cwd) {
  std::vector<core::Diagnostic> diagnostics;
  for (std::size_t m = 0; m < members.size(); ++m) {
    const ProgramMember &member = members[m];
    for (const auto &[name, import] : member.facts.imports) {
      if (import.declared.empty())
        continue;
      const std::optional<std::size_t> definer = definerOf(members, name, m);
      if (!definer)
        continue;
      const ProgramMember &defining = members[*definer];
      const auto interface = defining.facts.functions.find(name);
      const FunctionInterface *definition =
          interface == defining.facts.functions.end() ? nullptr
                                                      : &interface->second;
      const std::vector<Finding> findings = contradictions(
          import, defining.exports.functions.at(name), definition);
      if (findings.empty())
        continue;
      const std::string file = displayPath(defining.source, defining.cwd, cwd);
      for (const Finding &finding : findings) {
        core::Diagnostic diagnostic{
            .severity = core::Severity::Error,
            .certainty = core::Certainty::Definite,
            .id = core::diag::AnnotationMismatch,
            .message = quotedName(name) + " is declared " + finding.declared +
                       " here but its definition in " + quotedName(file) + " " +
                       finding.done,
            .location = import.location
                            ? absoluteLocation(*import.location, member.cwd)
                            : core::SourceLocation{},
            .notes = {},
            .fixits = {}};
        if (definition != nullptr && definition->location)
          diagnostic.addNote(
              "defined here",
              absoluteLocation(*definition->location, defining.cwd));
        diagnostics.push_back(std::move(diagnostic));
      }
    }
  }
  return diagnostics;
}

//===----------------------------------------------------------------------===//
// Printing
//===----------------------------------------------------------------------===//

struct ProgramDiagnosticPrinter::Engines {
  explicit Engines(std::string prefix)
      : locatedPrinter(llvm::errs(), options),
        located(llvm::makeIntrusiveRefCnt<clang::DiagnosticIDs>(), options,
                &locatedPrinter, /*ShouldOwnClient=*/false),
        files(clang::FileSystemOptions{}), sources(located, files),
        unlocatedPrinter(llvm::errs(), options),
        unlocated(llvm::makeIntrusiveRefCnt<clang::DiagnosticIDs>(), options,
                  &unlocatedPrinter, /*ShouldOwnClient=*/false),
        locatedSink(located), unlocatedSink(unlocated) {
    located.setSourceManager(&sources);
    locatedPrinter.BeginSourceFile(language, nullptr);
    unlocatedPrinter.setPrefix(std::move(prefix));
  }
  Engines(const Engines &) = delete;
  Engines &operator=(const Engines &) = delete;
  Engines(Engines &&) = delete;
  Engines &operator=(Engines &&) = delete;
  ~Engines() { locatedPrinter.EndSourceFile(); }

  clang::DiagnosticOptions options;
  clang::LangOptions language;
  clang::TextDiagnosticPrinter locatedPrinter;
  clang::DiagnosticsEngine located;
  clang::FileManager files;
  clang::SourceManager sources;
  clang::TextDiagnosticPrinter unlocatedPrinter;
  clang::DiagnosticsEngine unlocated;
  ClangDiagnosticSink locatedSink;
  ClangDiagnosticSink unlocatedSink;
};

ProgramDiagnosticPrinter::ProgramDiagnosticPrinter(std::string prefix)
    : engines(std::make_unique<Engines>(std::move(prefix))) {}

ProgramDiagnosticPrinter::~ProgramDiagnosticPrinter() = default;

void ProgramDiagnosticPrinter::report(const core::Diagnostic &diagnostic) {
  if (diagnostic.location.isValid())
    engines->locatedSink.report(diagnostic);
  else
    engines->unlocatedSink.report(diagnostic);
}

} // namespace weavec::frontend
