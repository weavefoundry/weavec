//===- LinkStep.cpp - The whole-program link step (RFC 0030) --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/LinkStep.h"

#include "weavec/Analysis/KindTable.h"
#include "weavec/Core/Summary.h"
#include "weavec/Frontend/ClangDiagnosticSink.h"
#include "weavec/Frontend/LedgerWriter.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"

#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CheckedArithmetic.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <map>
#include <set>
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
    absolute = joined;
  }
  llvm::sys::path::remove_dots(absolute, /*remove_dot_dot=*/true);
  return absolute.str().str();
}

std::string displayPath(llvm::StringRef path, llvm::StringRef base,
                        llvm::StringRef cwd) {
  if (path.empty())
    return {};
  const std::string absolute = absoluteFrom(path, base);
  llvm::SmallString<256> directory(cwd);
  if (directory.empty())
    std::ignore = llvm::sys::fs::current_path(directory);
  return pathRelativeToRoot(absolute, directory, directory);
}

//===----------------------------------------------------------------------===//
// Step 2: slots
//===----------------------------------------------------------------------===//

core::SlotSolution solveProgramSlots(std::span<const ProgramMember> members,
                                     const LinkShape &shape) {
  core::FnSlots constraints;
  core::SlotRules rules;
  rules.scope = core::SlotScope::Link;
  rules.executable = shape.executable;
  rules.exportDynamic = shape.exportDynamic;
  rules.unanalyzedInputs = !shape.inputsWithoutRecords.empty();
  for (const ProgramMember &member : members) {
    const record::SlotFacts &slots = member.payload.facts.slots;
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
    for (analysis::BoundaryRow row : member.payload.facts.boundaries) {
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
// Step 3: declarations
//===----------------------------------------------------------------------===//

namespace {

/// The member that defines `name` with external linkage, other than
/// `except`.
std::optional<std::size_t> definerOf(std::span<const ProgramMember> members,
                                     const std::string &name,
                                     std::size_t except) {
  for (std::size_t m = 0; m < members.size(); ++m) {
    if (m == except)
      continue;
    const auto &functions = members[m].payload.exports.functions;
    const auto it = functions.find(name);
    if (it != functions.end() && it->second.external)
      return m;
  }
  return std::nullopt;
}

/// Whether the callee keeps a copy of argument `param` in memory the caller
/// can reach after the call.
bool storesArgument(const core::FunctionSummary &summary, std::uint32_t param) {
  const core::SummaryPath root = core::SummaryPath::param(param);
  if (summary.effectOf(root).escaped)
    return true;
  return std::ranges::any_of(summary.stores, [&](const core::Store &store) {
    return store.value.kind == core::ValueSource::Kind::Copy &&
           store.value.path == root;
  });
}

std::string quoted(const std::string &text) {
  return "'" + text + "'";
}

/// The spelling of parameter `index` in messages.
std::string parameterName(const record::DeclaredInterface &declared,
                          std::size_t index) {
  if (index < declared.params.size() && !declared.params[index].name.empty())
    return quoted(declared.params[index].name);
  return "parameter " + std::to_string(index + 1);
}

} // namespace

namespace {

/// One contradiction between a declaration and its definition: what the
/// declaration states and what the definition does.
struct Finding {
  std::string declared;
  std::string done;
};

/// §13.2 step 3 for one import against its definition.
std::vector<Finding>
contradictions(const record::ImportInterface &import,
               const analysis::ExportedFunction &function,
               const record::FunctionInterface *definition) {
  std::vector<Finding> findings;
  const core::FunctionSummary &summary = function.summary.get();
  const record::DeclaredInterface &declared = import.declared;
  const auto definedKind =
      [&](std::size_t index) -> std::optional<core::PointerKind> {
    if (definition == nullptr || index >= definition->params.size() ||
        !definition->params[index])
      return std::nullopt;
    auto kind = record::parseKind(*definition->params[index]);
    if (!kind || kind->source != core::KindSource::Declared)
      return std::nullopt;
    return kind;
  };
  for (std::size_t i = 0; i < declared.params.size(); ++i) {
    const record::DeclaredParam &param = declared.params[i];
    const auto index = static_cast<std::uint32_t>(i);
    const std::string name = parameterName(declared, i);
    if (param.ownership == "WEAVEC_BORROWED" ||
        param.ownership == "WEAVEC_MUT") {
      std::string verb;
      if (summary.frees(index))
        verb = "frees";
      else if (summary.consumes(index))
        verb = "takes ownership of";
      else if (storesArgument(summary, index))
        verb = "stores";
      if (!verb.empty())
        findings.push_back(
            Finding{.declared = *param.ownership, .done = verb + " " + name});
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
    const core::OwnershipKind kind = summary.inferredReturnKind();
    if (kind == core::OwnershipKind::Shared ||
        kind == core::OwnershipKind::Mutable)
      findings.push_back(Finding{.declared = "WEAVEC_OWNED",
                                 .done = "returns a borrowed pointer"});
  } else if ((declared.ownership == "WEAVEC_BORROWED" ||
              declared.ownership == "WEAVEC_MUT") &&
             summary.returnsOnlyFresh()) {
    findings.push_back(Finding{.declared = *declared.ownership,
                               .done = "returns a fresh allocation"});
  }
  if (declared.result) {
    const auto stated = core::PointerKind::parse(*declared.result);
    if (stated && stated->nullability == core::Nullability::Nonnull &&
        summary.mayReturnNull())
      findings.push_back(Finding{.declared = "to return " + *declared.result,
                                 .done = "may return null"});
  }
  return findings;
}

core::SourceLocation absoluteLocation(core::SourceLocation location,
                                      llvm::StringRef base) {
  location.file = absoluteFrom(location.file, base);
  location.opaque = 0;
  return location;
}

} // namespace

DeclarationCheck verifyDeclarations(std::span<const ProgramMember> members,
                                    llvm::StringRef cwd) {
  DeclarationCheck check;
  for (std::size_t m = 0; m < members.size(); ++m) {
    const ProgramMember &member = members[m];
    for (const auto &[name, import] : member.payload.facts.imports) {
      if (import.declared.empty())
        continue;
      const std::optional<std::size_t> definer = definerOf(members, name, m);
      if (!definer)
        continue;
      const ProgramMember &defining = members[*definer];
      const auto interface = defining.payload.facts.functions.find(name);
      const record::FunctionInterface *definition =
          interface == defining.payload.facts.functions.end()
              ? nullptr
              : &interface->second;
      const std::vector<Finding> findings = contradictions(
          import, defining.payload.exports.functions.at(name), definition);
      if (findings.empty())
        continue;
      const std::string file = displayPath(defining.source, defining.cwd, cwd);
      for (const Finding &finding : findings) {
        core::Diagnostic diagnostic{
            .severity = core::Severity::Error,
            .certainty = core::Certainty::Definite,
            .id = core::diag::AnnotationMismatch,
            .message = quoted(name) + " is declared " + finding.declared +
                       " here but its definition in " + quoted(file) + " " +
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
        check.diagnostics.push_back(std::move(diagnostic));
      }
      check.contradictions.push_back(
          Contradiction{.member = m,
                        .definer = *definer,
                        .function = name,
                        .detail = "the declaration of '" + name + "' (" +
                                  findings.front().declared +
                                  ") is contradicted by its "
                                  "definition, which " +
                                  findings.front().done});
    }
  }
  // §7.6, §13.2 step 5: a store that breaks a header-struct invariant
  // another unit relies on.
  for (std::size_t m = 0; m < members.size(); ++m) {
    for (const record::InvariantRow &row :
         members[m].payload.facts.invariants) {
      if (row.verdict != core::Verdict::Violated || !row.store)
        continue;
      for (std::size_t other = 0; other < members.size(); ++other) {
        const auto &rows = members[other].payload.facts.invariants;
        const bool relies =
            other != m &&
            std::ranges::any_of(rows, [&](const record::InvariantRow &r) {
              return r.relied && r.record == row.record &&
                     r.field == row.field && r.templ == row.templ;
            });
        if (!relies)
          continue;
        check.diagnostics.push_back(core::Diagnostic{
            .severity = core::Severity::Error,
            .certainty = core::Certainty::Definite,
            .id = core::diag::AnnotationMismatch,
            .message =
                "store to '" + row.record + "." + row.field + "' breaks '" +
                row.templ + "', which '" +
                displayPath(members[other].source, members[other].cwd, cwd) +
                "' relies on",
            .location = absoluteLocation(*row.store, members[m].cwd),
            .notes = {},
            .fixits = {}});
        break;
      }
    }
  }
  return check;
}

//===----------------------------------------------------------------------===//
// Step 5: exported requirements at cross-unit callers
//===----------------------------------------------------------------------===//

namespace {

/// An extent term evaluated at one call, from the constants the caller's
/// record kept for the arguments. `param n` reads argument `n`; a field
/// root, or an argument that was not a constant, is unknown. Terms are
/// mathematical integers, overflow-checked (§10.2).
std::optional<std::int64_t> termAt(const core::ExtentTerm &term,
                                   const record::ImportCall &call) {
  if (term.isConstant())
    return term.offset;
  if (term.path->root != core::ExtentPath::Root::Param)
    return std::nullopt;
  const std::size_t index = term.path->param;
  if (index >= call.evidence.size() || !call.evidence[index].value)
    return std::nullopt;
  const auto scaled = llvm::checkedMul(term.scale, *call.evidence[index].value);
  return scaled ? llvm::checkedAdd(*scaled, term.offset) : std::nullopt;
}

/// Whether the requirement's guard holds at this call: a loop that runs
/// zero times accesses nothing (§7.5 R2). None when it cannot be decided,
/// which includes R3's comparison of two pointer parameters.
std::optional<bool> guardHolds(const analysis::RequirementGuard &guard,
                               const record::ImportCall &call) {
  const std::optional<std::int64_t> lhs = termAt(guard.lhs, call);
  const std::optional<std::int64_t> rhs = termAt(guard.rhs, call);
  if (!lhs || !rhs)
    return std::nullopt;
  return guard.relation == analysis::RequirementGuard::Relation::Less
             ? *lhs < *rhs
             : *lhs <= *rhs;
}

/// The bytes a requirement needs of the argument at this call. `ended-by`
/// and `nul-terminated` are not decidable from the caller's record, so they
/// stay unknown.
std::optional<std::int64_t> neededBytes(const core::PointerKind &kind,
                                        std::uint64_t element,
                                        const record::ImportCall &call) {
  if (!core::hasExtent(kind.shape))
    return std::nullopt;
  const std::optional<std::int64_t> extent = termAt(kind.extent, call);
  if (!extent)
    return std::nullopt;
  if (*extent <= 0)
    return 0;
  if (kind.shape == core::PointerShape::Sized)
    return *extent;
  if (kind.shape != core::PointerShape::Counted || element == 0)
    return std::nullopt;
  return llvm::checkedMul(*extent, static_cast<std::int64_t>(element));
}

/// One requirement decided at one call.
struct Verdict {
  core::SiteOutcome outcome = core::SiteOutcome::Unresolved;
  std::optional<std::int64_t> need = std::nullopt;
  std::optional<std::uint64_t> have = std::nullopt;
};

Verdict decideRequirement(const record::ExportedRequirement &requirement,
                          const core::PointerKind &kind,
                          const record::ImportCall &call) {
  const std::optional<analysis::RequirementGuard> guard =
      requirement.guard ? analysis::RequirementGuard::parse(*requirement.guard)
                        : std::nullopt;
  const std::optional<bool> holds =
      requirement.guard ? (guard ? guardHolds(*guard, call) : std::nullopt)
                        : std::optional(true);
  if (holds == false)
    return Verdict{.outcome = core::SiteOutcome::Proven};
  const std::optional<std::int64_t> need =
      neededBytes(kind, requirement.element, call);
  if (!need)
    return Verdict{};
  if (*need == 0)
    return Verdict{.outcome = core::SiteOutcome::Proven, .need = need};
  if (requirement.param >= call.evidence.size())
    return Verdict{.outcome = core::SiteOutcome::Unresolved, .need = need};
  const record::ArgumentEvidence &argument = call.evidence[requirement.param];
  // A null argument under a non-empty requirement is the null facet's
  // question, which §7.5 never trusts; the extent stays undecided here.
  if (argument.null || !argument.bytes)
    return Verdict{.outcome = core::SiteOutcome::Unresolved, .need = need};
  if (*argument.bytes >= static_cast<std::uint64_t>(*need))
    return Verdict{.outcome = core::SiteOutcome::Proven,
                   .need = need,
                   .have = argument.bytes};
  // The judgement is a lower bound, so a shorter argument violates the
  // requirement only when the record says the width is the whole object's
  // and the guard is known to hold.
  return Verdict{.outcome = argument.exact && holds == true
                                ? core::SiteOutcome::Violation
                                : core::SiteOutcome::Unresolved,
                 .need = need,
                 .have = argument.bytes};
}

/// Whether callers outside the members can reach the definition: a
/// requirement is then never discharged, however the visible callers
/// decide it.
bool reachableFromOutside(const analysis::ExportedFunction &function,
                          const LinkShape &shape) {
  return function.addressTaken || !shape.executable || shape.exportDynamic;
}

/// How the visible calls decided one requirement.
struct Tally {
  std::uint64_t calls = 0;
  std::uint64_t met = 0;
  bool violated = false;
};

} // namespace

RequirementCheck verifyRequirements(std::span<const ProgramMember> members,
                                    const LinkShape &shape) {
  RequirementCheck check;
  std::map<std::pair<std::size_t, std::string>, std::vector<Tally>> tallies;
  for (std::size_t m = 0; m < members.size(); ++m) {
    for (const auto &[name, import] : members[m].payload.facts.imports) {
      const std::optional<std::size_t> definer = definerOf(members, name, m);
      if (!definer)
        continue;
      const ProgramMember &defining = members[*definer];
      const auto interface = defining.payload.facts.functions.find(name);
      if (interface == defining.payload.facts.functions.end() ||
          interface->second.requirements.empty())
        continue;
      const std::vector<record::ExportedRequirement> &requirements =
          interface->second.requirements;
      std::vector<Tally> &tally =
          tallies.try_emplace({*definer, name}, requirements.size())
              .first->second;
      for (const record::ImportCall &call : import.calls) {
        if (!call.site)
          continue;
        for (std::size_t r = 0; r < requirements.size(); ++r) {
          const record::ExportedRequirement &requirement = requirements[r];
          const auto kind = core::PointerKind::parse(requirement.kind);
          if (!kind)
            continue;
          const Verdict verdict = decideRequirement(requirement, *kind, call);
          ++tally[r].calls;
          RequirementDecision decision{.member = m,
                                       .function = call.function,
                                       .site = *call.site,
                                       .callee = name,
                                       .param = requirement.param,
                                       .decision = {},
                                       .need = std::nullopt,
                                       .have = std::nullopt};
          if (verdict.need)
            decision.need = std::to_string(*verdict.need) + " bytes";
          if (verdict.have)
            decision.have = std::to_string(*verdict.have) + " bytes";
          const std::string what =
              quoted(name) + " requires " + kind->toString() + " of " +
              parameterName(import.declared, requirement.param) +
              " from its callers";
          switch (verdict.outcome) {
          case core::SiteOutcome::Proven:
            ++tally[r].met;
            decision.decision = core::FacetDecision::proven();
            decision.decision.detail = what + ", which this call meets";
            break;
          case core::SiteOutcome::Violation:
            // The error is step 4's, at the argument: the caller's re-run
            // sees the callee's summary requirement. This row records what
            // the requirement needed and what the call gave.
            tally[r].violated = true;
            decision.decision = core::FacetDecision::violation(
                quoted(name) + " accesses " +
                decision.need.value_or("more bytes") + " of the argument for " +
                parameterName(import.declared, requirement.param) +
                ", which is " + decision.have.value_or("shorter") + " long");
            break;
          case core::SiteOutcome::Checked:
          case core::SiteOutcome::Unresolved:
          case core::SiteOutcome::Trusted:
            decision.decision = core::FacetDecision::unresolvedFor(
                core::UnresolvedReason::UnknownExtent,
                what + ", which is not known here");
            break;
          }
          check.decisions.push_back(std::move(decision));
        }
      }
    }
  }
  for (const auto &[key, rows] : tallies) {
    bool whole = !rows.empty() && shape.inputsWithoutRecords.empty();
    for (const Tally &row : rows) {
      if (row.calls != 0 && row.met == row.calls && !row.violated)
        ++check.verified;
      else
        whole = false;
    }
    const auto &functions = members[key.first].payload.exports.functions;
    const auto function = functions.find(key.second);
    if (whole && function != functions.end() &&
        !reachableFromOutside(function->second, shape))
      check.discharged.insert(key);
  }
  return check;
}

//===----------------------------------------------------------------------===//
// Step 5: the allocator
//===----------------------------------------------------------------------===//

std::optional<AllocatorFinding>
allocatorDefinedBy(std::span<const ProgramMember> members) {
  for (std::size_t m = 0; m < members.size(); ++m) {
    const record::InterfaceFacts &facts = members[m].payload.facts;
    if (!facts.allocator)
      continue;
    for (std::size_t other = 0; other < members.size(); ++other)
      if (other != m && members[other].payload.facts.loweredAllocations > 0)
        return AllocatorFinding{.member = m, .function = *facts.allocator};
  }
  return std::nullopt;
}

std::string allocatorWarning(std::span<const ProgramMember> members,
                             const AllocatorFinding &finding,
                             llvm::StringRef cwd) {
  const ProgramMember &member = members[finding.member];
  return "heap zero-initialisation assumes the system allocator, but '" +
         displayPath(member.source, member.cwd, cwd) + "' defines '" +
         finding.function + "'; rebuild with -fno-weavec-zero-init";
}

//===----------------------------------------------------------------------===//
// Step 6: the program ledger
//===----------------------------------------------------------------------===//

namespace {

void absolutize(core::SourceLocation &location, llvm::StringRef base) {
  location.file = absoluteFrom(location.file, base);
  location.opaque = 0;
}

void absolutize(core::UnitLedger &unit, llvm::StringRef base) {
  unit.source = absoluteFrom(unit.source, base);
  for (core::FunctionLedger &function : unit.functions) {
    function.file = absoluteFrom(function.file, base);
    for (core::Site &site : function.sites) {
      absolutize(site.location, base);
      for (std::optional<core::FacetRecord> &record : site.facets)
        if (record && record->fixit)
          absolutize(record->fixit->location, base);
    }
  }
}

void absolutize(core::LedgerDiagnostic &diagnostic, llvm::StringRef base) {
  absolutize(diagnostic.location, base);
  for (core::LedgerNote &note : diagnostic.notes)
    absolutize(note.location, base);
}

core::LedgerDiagnostic ledgerDiagnostic(const core::Diagnostic &diagnostic) {
  core::LedgerDiagnostic entry;
  entry.id = std::string(diagnostic.id);
  entry.severity = diagnostic.severity;
  entry.certainty = diagnostic.certainty;
  entry.message = diagnostic.message;
  entry.location = diagnostic.location;
  entry.location.opaque = 0;
  for (const core::Diagnostic &note : diagnostic.notes) {
    core::LedgerNote copy{.message = note.message, .location = note.location};
    copy.location.opaque = 0;
    entry.notes.push_back(std::move(copy));
  }
  return entry;
}

/// A member that was not run keeps what its compile reported, which its
/// record names without messages.
std::vector<core::LedgerDiagnostic>
reportedDiagnostics(const ProgramMember &member) {
  std::vector<core::LedgerDiagnostic> diagnostics;
  for (const ReportedDiagnostic &reported : member.payload.reported) {
    core::LedgerDiagnostic entry;
    entry.id = reported.id;
    entry.severity = core::Severity::Warning;
    entry.certainty = core::Certainty::Definite;
    entry.message = "reported when '" + member.source + "' was compiled";
    entry.location = core::SourceLocation{.file = reported.file,
                                          .line = reported.line,
                                          .column = reported.column,
                                          .opaque = 0};
    diagnostics.push_back(std::move(entry));
  }
  return diagnostics;
}

core::FunctionLedger *functionNamed(core::UnitLedger &unit,
                                    const std::string &name) {
  const auto it =
      std::ranges::find_if(unit.functions, [&](const core::FunctionLedger &f) {
        return f.name == name;
      });
  return it == unit.functions.end() ? nullptr : &*it;
}

constexpr std::array<core::Facet, 3> RecordFacets{
    core::Facet::Spatial, core::Facet::Null, core::Facet::Assertion};

/// §13.2 step 6: the spatial, null and assertion facets of `rows`, which
/// decided the emitted code, over those of the run. A function whose sites
/// the run does not match takes the record's rows whole.
void copyRecordFacets(core::UnitLedger &unit,
                      std::span<const record::FunctionRows> rows) {
  for (const record::FunctionRows &recorded : rows) {
    const core::UnitLedger compact = record::unitLedgerOf(
        std::span<const record::FunctionRows>(&recorded, 1));
    core::FunctionLedger *function = functionNamed(unit, recorded.function);
    if (function == nullptr) {
      unit.functions.push_back(compact.functions.front());
      continue;
    }
    const bool matches =
        function->sites.size() == recorded.rows.size() &&
        std::ranges::equal(
            function->sites, recorded.rows,
            [](const core::Site &site, const record::SiteRow &row) {
              return site.kind == row.kind && site.location.line == row.line &&
                     site.location.column == row.column;
            });
    if (!matches) {
      function->sites = compact.functions.front().sites;
      continue;
    }
    for (std::size_t s = 0; s < recorded.rows.size(); ++s) {
      core::Site &site = function->sites[s];
      for (const core::Facet facet : RecordFacets) {
        const auto index = static_cast<std::size_t>(facet);
        const std::optional<core::FacetDecision> &decided =
            recorded.rows[s].facets.at(index);
        std::optional<core::FacetRecord> &record = site.facets.at(index);
        if (!decided) {
          record.reset();
          continue;
        }
        if (record && record->decision.outcome == decided->outcome &&
            record->decision.unresolved == decided->unresolved &&
            record->decision.trusted == decided->trusted)
          continue;
        const std::optional<core::FacetCheck> check =
            record && decided->outcome == core::SiteOutcome::Checked
                ? record->check
                : std::nullopt;
        core::FacetRecord replaced;
        replaced.decided = true;
        replaced.decision = *decided;
        replaced.decision.detail = "decided when the unit was compiled";
        replaced.check = check;
        record = std::move(replaced);
      }
    }
  }
}

/// §7.3, §13.2 step 5: a cross-unit call whose argument for a
/// `reliesOnSingle` parameter was not Single-valid gets the Call site's
/// `unresolved(unknown-extent)` spatial row. Returns the rows added.
std::uint64_t addRelianceRows(core::Ledger &ledger,
                              std::span<const ProgramMember> members) {
  std::uint64_t added = 0;
  for (std::size_t m = 0; m < members.size(); ++m) {
    for (const auto &[name, import] : members[m].payload.facts.imports) {
      const std::optional<std::size_t> definer = definerOf(members, name, m);
      if (!definer)
        continue;
      const auto &functions = members[*definer].payload.facts.functions;
      const auto interface = functions.find(name);
      if (interface == functions.end() ||
          interface->second.reliesOnSingle.empty())
        continue;
      for (const record::ImportCall &call : import.calls) {
        if (!call.site)
          continue;
        std::optional<std::uint32_t> unsingle;
        for (const std::uint32_t param : interface->second.reliesOnSingle)
          if (param < call.args.size() && call.args[param] == false) {
            unsingle = param;
            break;
          }
        core::FunctionLedger *caller =
            unsingle ? functionNamed(ledger.units[m], call.function) : nullptr;
        core::Site *site =
            caller != nullptr ? caller->site(*call.site) : nullptr;
        if (site == nullptr || site->kind != core::SiteKind::Call)
          continue;
        core::FacetRecord &spatial = site->addFacet(core::Facet::Spatial);
        spatial.decide(core::FacetDecision::unresolvedFor(
            core::UnresolvedReason::UnknownExtent,
            quoted(name) + " relies on the argument for " +
                parameterName(import.declared, *unsingle) +
                " pointing to at least one element, which is not known "
                "here"));
        ++added;
      }
    }
  }
  return added;
}

/// Whether every call to `name` inside its own unit establishes what the
/// callee needs: a spatial facet that is unresolved or a violation there
/// leaves the requirement assumed, so it is not discharged (§13.2 step 5).
bool localCallersMeet(const core::UnitLedger &unit, const std::string &name) {
  for (const core::FunctionLedger &function : unit.functions)
    for (const core::Site &site : function.sites) {
      if (site.kind != core::SiteKind::Call || site.callee != name)
        continue;
      const core::FacetRecord *spatial = site.facet(core::Facet::Spatial);
      if (spatial != nullptr &&
          (spatial->outcome() == core::SiteOutcome::Unresolved ||
           spatial->outcome() == core::SiteOutcome::Violation))
        return false;
    }
  return true;
}

/// §13.2 step 5: the rows `verifyRequirements` decided, at the callers'
/// Call sites, and the `trusted(caller-contract)` rows its verdicts
/// discharge in the definitions.
void addRequirementRows(core::Ledger &ledger, const RequirementCheck &check,
                        std::span<const core::Ledger *const> runs) {
  for (const RequirementDecision &row : check.decisions) {
    if (row.member >= ledger.units.size())
      continue;
    core::FunctionLedger *caller =
        functionNamed(ledger.units[row.member], row.function);
    core::Site *site = caller != nullptr ? caller->site(row.site) : nullptr;
    if (site == nullptr || site->kind != core::SiteKind::Call)
      continue;
    site->addFacet(core::Facet::Spatial)
        .addRequirement(core::Requirement{.argument = row.param,
                                          .need = row.need,
                                          .have = row.have,
                                          .decision = row.decision,
                                          .check = std::nullopt});
  }
  for (const auto &[member, name] : check.discharged) {
    // The definition's own unit must have been analysed at link, so that
    // its own calls to the function are in view: a caller there that does
    // not establish the requirement leaves it assumed.
    if (member >= ledger.units.size() || member >= runs.size() ||
        runs[member] == nullptr)
      continue;
    core::FunctionLedger *definition =
        functionNamed(ledger.units[member], name);
    if (definition == nullptr || !localCallersMeet(ledger.units[member], name))
      continue;
    for (core::Site &site : definition->sites) {
      core::FacetRecord *spatial = site.facet(core::Facet::Spatial);
      if (spatial == nullptr ||
          spatial->decision.trusted !=
              std::optional(core::TrustReason::CallerContract))
        continue;
      spatial->decision = core::FacetDecision::proven();
      spatial->decision.detail =
          "every caller of '" + name + "' in the program meets the requirement";
      spatial->decided = true;
    }
  }
}

/// Temporal facets that rest on a contradicted declaration prove nothing:
/// in every function that calls the function (in the declaring member), and
/// in every function that calls one of those, transitively, the proven and
/// trusted temporal facets become `unresolved(unknown-callee)`.
void downgradeContradicted(core::Ledger &ledger,
                           std::span<const Contradiction> contradictions) {
  using Key = std::pair<std::size_t, std::string>;
  std::map<Key, std::string> tainted;
  // External functions some unit calls by name, with the detail to use.
  std::map<std::string, std::string> taintedExternal;
  const auto calls = [](const core::FunctionLedger &function,
                        const std::string &callee) {
    return std::ranges::any_of(function.sites, [&](const core::Site &site) {
      return site.kind == core::SiteKind::Call && site.callee == callee;
    });
  };
  const auto taint = [&](std::size_t unit, const core::FunctionLedger &function,
                         const std::string &detail) {
    if (!tainted.emplace(Key{unit, function.name}, detail).second)
      return false;
    if (function.linkage == core::Linkage::External)
      taintedExternal.try_emplace(function.name, detail);
    return true;
  };
  for (const Contradiction &contradiction : contradictions) {
    if (contradiction.member >= ledger.units.size())
      continue;
    for (const core::FunctionLedger &function :
         ledger.units[contradiction.member].functions)
      if (calls(function, contradiction.function))
        taint(contradiction.member, function, contradiction.detail);
  }
  for (bool changed = !tainted.empty(); changed;) {
    changed = false;
    for (std::size_t u = 0; u < ledger.units.size(); ++u) {
      for (const core::FunctionLedger &function : ledger.units[u].functions) {
        if (tainted.contains(Key{u, function.name}))
          continue;
        for (const core::Site &site : function.sites) {
          if (site.kind != core::SiteKind::Call || site.callee.empty())
            continue;
          const auto local = tainted.find(Key{u, site.callee});
          const auto external = taintedExternal.find(site.callee);
          const std::string *detail =
              local != tainted.end()
                  ? &local->second
                  : (external != taintedExternal.end() ? &external->second
                                                       : nullptr);
          if (detail != nullptr && taint(u, function, *detail)) {
            changed = true;
            break;
          }
        }
      }
    }
  }
  for (const auto &[key, detail] : tainted) {
    core::FunctionLedger *function =
        functionNamed(ledger.units[key.first], key.second);
    if (function == nullptr)
      continue;
    for (core::Site &site : function->sites) {
      core::FacetRecord *temporal = site.facet(core::Facet::Temporal);
      if (temporal == nullptr ||
          (temporal->outcome() != core::SiteOutcome::Proven &&
           temporal->outcome() != core::SiteOutcome::Trusted))
        continue;
      temporal->decision = core::FacetDecision::unresolvedFor(
          core::UnresolvedReason::UnknownCallee, detail);
      temporal->decided = true;
    }
  }
}

/// §13.2 step 1: with an input without a record in the link, a call into a
/// function no record defines is `trusted(external-unit)`, not an unknown
/// callee.
void trustExternalUnits(core::Ledger &ledger,
                        std::span<const ProgramMember> members) {
  std::set<std::string> defined;
  for (const ProgramMember &member : members)
    for (const auto &[name, function] : member.payload.exports.functions)
      if (function.external)
        defined.insert(name);
  for (std::size_t m = 0; m < members.size() && m < ledger.units.size(); ++m) {
    const std::set<std::string> &imports = members[m].payload.exports.imports;
    for (core::FunctionLedger &function : ledger.units[m].functions) {
      for (core::Site &site : function.sites) {
        core::FacetRecord *temporal = site.facet(core::Facet::Temporal);
        if (site.kind != core::SiteKind::Call || site.callee.empty() ||
            temporal == nullptr || !imports.contains(site.callee) ||
            defined.contains(site.callee) ||
            temporal->decision.unresolved !=
                std::optional(core::UnresolvedReason::UnknownCallee))
          continue;
        temporal->decision = core::FacetDecision::trustedFor(
            core::TrustReason::ExternalUnit,
            quoted(site.callee) +
                " is defined by a link input without a WeaveC record");
      }
    }
  }
}

/// §13.2 step 5: the A1 and A3 counts the records support.
core::Assumptions assumptionsOf(std::span<const ProgramMember> members,
                                const LinkShape &shape,
                                std::uint64_t relianceRows,
                                std::uint64_t verified) {
  core::Assumptions assumptions;
  for (const ProgramMember &member : members) {
    for (const auto &[name, function] : member.payload.exports.functions) {
      if (!function.external && !function.addressTaken)
        continue;
      const auto facts = member.payload.facts.functions.find(name);
      if (facts == member.payload.facts.functions.end())
        continue;
      assumptions.a1.exportedRequirements += facts->second.requirements.size();
      assumptions.a1.reliesOnSingle += facts->second.reliesOnSingle.size();
    }
  }
  // §13.2 step 5: what the callers the records contain decided.
  assumptions.a1.verified =
      std::min(verified, assumptions.a1.exportedRequirements);
  assumptions.a1.unverifiedCallers = relianceRows;

  std::map<std::string, std::vector<const record::InvariantRow *>> invariants;
  std::map<std::string, std::set<core::PointerShape>> slotShapes;
  for (const ProgramMember &member : members) {
    for (const record::InvariantRow &row : member.payload.facts.invariants)
      invariants[row.record + "." + row.field + ": " + row.templ].push_back(
          &row);
    for (const record::SlotKindRow &row : member.payload.facts.slotKinds)
      if (const auto kind = core::PointerKind::parse(row.kind))
        slotShapes[row.slot].insert(kind->shape);
  }
  for (const auto &[key, rows] : invariants) {
    if (std::ranges::none_of(
            rows, [](const record::InvariantRow *row) { return row->relied; }))
      continue;
    ++assumptions.a3.headerInvariants;
    if (!shape.inputsWithoutRecords.empty() ||
        std::ranges::any_of(rows, [](const record::InvariantRow *row) {
          return row->verdict != core::Verdict::Holds;
        }))
      ++assumptions.a3.unverified;
  }
  // A header slot Single in one unit and demoted in another (§7.3).
  for (const auto &[slot, shapes] : slotShapes)
    if (shapes.contains(core::PointerShape::Single) &&
        shapes.contains(core::PointerShape::Unknown))
      ++assumptions.a3.unverified;
  assumptions.a3.inputsWithoutRecords = shape.inputsWithoutRecords;
  return assumptions;
}

} // namespace

core::Ledger composeProgramLedger(const ProgramLedgerInput &input) {
  core::Ledger ledger;
  ledger.scope = core::LedgerScope::Program;
  const std::span<const ProgramMember> members = input.members;
  for (std::size_t m = 0; m < members.size(); ++m) {
    const ProgramMember &member = members[m];
    const core::Ledger *run = m < input.runs.size() ? input.runs[m] : nullptr;
    core::UnitLedger unit;
    std::vector<core::LedgerDiagnostic> diagnostics;
    if (run != nullptr && !run->units.empty()) {
      unit = run->units.front();
      diagnostics = run->diagnostics;
      if (input.copyRecordRows)
        copyRecordFacets(unit, member.payload.sites);
    } else {
      unit = record::unitLedgerOf(member.payload.sites);
      unit.a5 = member.payload.a5;
      diagnostics = reportedDiagnostics(member);
    }
    if (unit.source.empty())
      unit.source = member.source;
    if (!member.object.empty())
      unit.object = absoluteFrom(member.object, input.cwd);
    if (unit.target.empty())
      unit.target = member.target;
    absolutize(unit, member.cwd);
    // The unit's facets link to its diagnostics by index; they now follow
    // those of the units before it.
    const auto offset = static_cast<std::uint32_t>(ledger.diagnostics.size());
    for (core::FunctionLedger &function : unit.functions)
      for (core::Site &site : function.sites)
        for (std::optional<core::FacetRecord> &record : site.facets)
          if (record && record->diagnostic) {
            if (*record->diagnostic < diagnostics.size())
              *record->diagnostic += offset;
            else
              record->diagnostic.reset();
          }
    for (core::LedgerDiagnostic &diagnostic : diagnostics) {
      absolutize(diagnostic, member.cwd);
      diagnostic.unit = static_cast<std::uint32_t>(m);
      ledger.diagnostics.push_back(std::move(diagnostic));
    }
    ledger.units.push_back(std::move(unit));
  }

  if (input.declarations != nullptr) {
    for (const core::Diagnostic &diagnostic : input.declarations->diagnostics)
      ledger.diagnostics.push_back(ledgerDiagnostic(diagnostic));
    downgradeContradicted(ledger, input.declarations->contradictions);
  }
  for (const core::Diagnostic &diagnostic : input.linkDiagnostics)
    ledger.diagnostics.push_back(ledgerDiagnostic(diagnostic));
  const std::uint64_t relianceRows = addRelianceRows(ledger, members);
  if (input.requirements != nullptr)
    addRequirementRows(ledger, *input.requirements, input.runs);
  if (!input.shape.inputsWithoutRecords.empty())
    trustExternalUnits(ledger, members);

  ledger.assumptions = assumptionsOf(
      members, input.shape, relianceRows,
      input.requirements != nullptr ? input.requirements->verified : 0);
  core::deriveAssumptionCounts(ledger);
  if (const std::optional<AllocatorFinding> allocator =
          allocatorDefinedBy(members))
    ledger.assumptions->a5.allocatorDefinedBy =
        displayPath(members[allocator->member].source,
                    members[allocator->member].cwd, input.cwd);
  return ledger;
}

//===----------------------------------------------------------------------===//
// Printing
//===----------------------------------------------------------------------===//

struct LinkDiagnosticPrinter::Engines {
  explicit Engines(const std::string &prefix)
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
    unlocatedPrinter.setPrefix(prefix);
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

LinkDiagnosticPrinter::LinkDiagnosticPrinter(std::string prefix)
    : engines(std::make_unique<Engines>(prefix)) {}

LinkDiagnosticPrinter::~LinkDiagnosticPrinter() = default;

void LinkDiagnosticPrinter::report(const core::Diagnostic &diagnostic) {
  if (diagnostic.location.isValid())
    engines->locatedSink.report(diagnostic);
  else
    engines->unlocatedSink.report(diagnostic);
}

} // namespace weavec::frontend
