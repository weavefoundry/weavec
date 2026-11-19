//===- Fixer.cpp - weavec-cc --weavec-fix ---------------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0049 §6. The fixer reads the ledgers a survey wrote, merges the fixes
// their errors carry by declaration, writes the certain ones and the likely
// ones that agree, and runs the rules again (each unit's recorded command,
// with -fsyntax-only) on the units an edit reached. A likely fix that
// brings an error with no fix is taken out again and not proposed again. It
// stops when a round applies nothing, and marks the files left with no
// error WEAVEC_SAFE_FILE. Nothing it writes is believed: the rules check an
// annotation the fixer wrote as one a person did.
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/Driver.h"
#include "weavec/Frontend/EnforcementLedger.h"
#include "weavec/Frontend/RuleFix.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace weavec::frontend {

namespace {

/// One unit, as its latest ledger records it.
struct Unit {
  std::string source;
  std::string directory;
  std::vector<std::string> command;
  bool cxx = false;
  std::vector<ReadFile> files;
  std::vector<RuleError> errors;
  std::vector<RuleFix> evidence;
  /// Each function: file (absolute), line, name; safe, or surveyed.
  struct Function {
    std::string file;
    unsigned line = 0;
    std::string name;
    bool safe = false;
    bool surveyed = false;
  };
  std::vector<Function> functions;
  /// The unit's ledger as read, for the ledger the fixer leaves.
  llvm::json::Value raw = nullptr;
};

/// An insertion the fixer made, kept so that it can be taken out.
struct Inserted {
  unsigned offset = 0;
  std::string text;
  std::string key;
};

/// The fixer's state and options.
struct Fixer {
  std::string executable;
  bool dryRun = false;
  bool certainOnly = false;
  bool mark = true;
  std::vector<std::string> only;
  unsigned rounds = 16;
  unsigned jobs = 1;
  std::string work;
  /// The units, by directory and source.
  std::map<std::string, Unit> units;
  /// The files' current text, and what the fixer inserted in each.
  std::map<std::string, std::string> texts;
  std::map<std::string, std::vector<Inserted>> inserted;
  /// The fixes written, by declaration and aspect: their level, and what
  /// they stated; those taken out, and why.
  std::map<std::string, RuleFix> applied;
  std::map<std::string, std::string> withdrawn;
  llvm::json::Array log;
  std::vector<std::string> marked;
  unsigned run = 0;
  /// The first error of the last run that is not a rule's.
  std::string broken;

  static std::string absolute(llvm::StringRef path, llvm::StringRef base) {
    llvm::SmallString<256> full(path);
    if (!llvm::sys::path::is_absolute(full)) {
      full = base;
      llvm::sys::path::append(full, path);
    }
    llvm::sys::path::remove_dots(full, /*remove_dot_dot=*/true);
    return full.str().str();
  }

  //===--------------------------------------------------------------------===//
  // Ledgers
  //===--------------------------------------------------------------------===//

  bool read(const std::string &path) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if (!buffer) {
      llvm::errs() << "weavec: fix: cannot read '" << path
                   << "': " << buffer.getError().message() << '\n';
      return false;
    }
    llvm::Expected<llvm::json::Value> parsed =
        llvm::json::parse((*buffer)->getBuffer());
    if (!parsed) {
      llvm::consumeError(parsed.takeError());
      llvm::errs() << "weavec: fix: '" << path << "' is not a WeaveC ledger\n";
      return false;
    }
    const llvm::json::Object *document = parsed->getAsObject();
    const llvm::json::Array *list =
        document == nullptr ? nullptr : document->getArray("units");
    if (list == nullptr) {
      llvm::errs() << "weavec: fix: '" << path << "' is not a WeaveC ledger\n";
      return false;
    }
    for (const llvm::json::Value &value : *list) {
      const llvm::json::Object *object = value.getAsObject();
      if (object == nullptr)
        continue;
      Unit unit;
      unit.raw = value;
      unit.source = object->getString("source").value_or("").str();
      const llvm::json::Object *command = object->getObject("command");
      if (command != nullptr) {
        unit.directory = command->getString("directory").value_or("").str();
        unit.cxx = command->getBoolean("cxx").value_or(false);
        if (const llvm::json::Array *arguments = command->getArray("arguments"))
          for (const llvm::json::Value &each : *arguments)
            unit.command.push_back(each.getAsString().value_or("").str());
      }
      if (unit.command.empty()) {
        llvm::errs() << "weavec: fix: '" << path
                     << "' records no command; it was not written under "
                        "-fweavec-safe-survey\n";
        return false;
      }
      if (const llvm::json::Array *files = object->getArray("files"))
        for (const llvm::json::Value &each : *files)
          if (const llvm::json::Object *file = each.getAsObject())
            unit.files.push_back(
                {.path = file->getString("path").value_or("").str(),
                 .size = static_cast<std::uint64_t>(
                     file->getInteger("size").value_or(0)),
                 .hash = file->getString("hash").value_or("").str()});
      if (const llvm::json::Array *errors = object->getArray("errors"))
        for (const llvm::json::Value &each : *errors) {
          RuleError error;
          if (readError(each, error))
            unit.errors.push_back(std::move(error));
        }
      if (const llvm::json::Array *evidence = object->getArray("evidence"))
        for (const llvm::json::Value &each : *evidence) {
          RuleFix fix;
          if (readFix(each, fix))
            unit.evidence.push_back(std::move(fix));
        }
      if (const llvm::json::Array *functions = object->getArray("functions"))
        for (const llvm::json::Value &each : *functions)
          if (const llvm::json::Object *function = each.getAsObject())
            unit.functions.push_back(
                {.file = absolute(function->getString("file").value_or(""),
                                  unit.directory),
                 .line = static_cast<unsigned>(
                     function->getInteger("line").value_or(0)),
                 .name = function->getString("function").value_or("").str(),
                 .safe = function->getString("tier") == "safe",
                 .surveyed = function->getBoolean("surveyed").value_or(false)});
      for (RuleError &error : unit.errors)
        error.ownerFile = absolute(error.ownerFile, unit.directory);
      units[unit.directory + "\n" + absolute(unit.source, unit.directory)] =
          std::move(unit);
    }
    return true;
  }

  //===--------------------------------------------------------------------===//
  // Running the rules again
  //===--------------------------------------------------------------------===//

  /// A unit's command, made a survey that writes its ledger to `ledger`.
  std::vector<std::string> surveyCommand(const std::vector<std::string> &given,
                                         bool cxx, const std::string &ledger,
                                         const std::string &wants) const {
    std::vector<std::string> args{executable};
    if (cxx)
      args.emplace_back("--driver-mode=g++");
    static constexpr std::array<llvm::StringLiteral, 4> WithValue{"-o", "-MF",
                                                                  "-MT", "-MQ"};
    for (std::size_t i = 1; i < given.size(); ++i) {
      const llvm::StringRef arg = given[i];
      if (llvm::is_contained(WithValue, arg)) {
        ++i;
        continue;
      }
      if (arg == "-c" || arg == "-S" || arg == "-E" || arg == "-M" ||
          arg == "-MM" || arg == "-MD" || arg == "-MMD" || arg == "-MP" ||
          arg == "-fsyntax-only" || arg.starts_with("-fweavec-ledger=") ||
          arg.starts_with("-fweavec-fix-wants=") ||
          arg.starts_with("--driver-mode=") ||
          (arg.starts_with("-o") && arg.size() > 2) ||
          (arg.starts_with("-MF") && arg.size() > 3))
        continue;
      args.push_back(arg.str());
    }
    args.emplace_back("-fsyntax-only");
    args.emplace_back("-fweavec-safe-survey");
    args.emplace_back("-fno-weavec-summary");
    args.push_back("-fweavec-ledger=" + ledger);
    if (!wants.empty())
      args.push_back("-fweavec-fix-wants=" + wants);
    return args;
  }

  /// Runs each command (directory, arguments), `jobs` at a time; their
  /// ledgers replace the units'.
  using Commands =
      std::vector<std::pair<std::string, std::vector<std::string>>>;
  bool runAll(const Commands &commands) {
    // Emptied first: an earlier fixer run's ledgers are not this one's.
    const std::string dir = work + "/run-" + std::to_string(++run) + "/";
    std::ignore = llvm::sys::fs::remove_directories(dir);
    std::ignore = llvm::sys::fs::create_directories(dir);
    llvm::SmallString<256> start;
    std::ignore = llvm::sys::fs::current_path(start);
    std::vector<llvm::sys::ProcessInfo> running;
    std::vector<std::string> outputs;
    bool ok = true;
    const auto wait = [&](std::size_t keep) {
      while (running.size() > keep) {
        std::string error;
        const llvm::sys::ProcessInfo done = llvm::sys::Wait(
            running.front(), /*SecondsToWait=*/std::nullopt, &error);
        (void)done;
        running.erase(running.begin());
      }
    };
    for (const auto &[directory, arguments] : commands) {
      std::vector<llvm::StringRef> args(arguments.begin(), arguments.end());
      outputs.push_back(dir + std::to_string(outputs.size()) + ".log");
      const std::array<std::optional<llvm::StringRef>, 3> redirects{
          std::nullopt, llvm::StringRef(outputs.back()),
          llvm::StringRef(outputs.back())};
      std::ignore = llvm::sys::fs::set_current_path(directory);
      std::string error;
      bool failed = false;
      running.push_back(llvm::sys::ExecuteNoWait(
          executable, args, std::nullopt, redirects, 0, &error, &failed));
      std::ignore = llvm::sys::fs::set_current_path(start);
      if (failed) {
        llvm::errs() << "weavec: fix: the rules could not be run for '"
                     << directory << "': " << error << '\n';
        ok = false;
      }
      wait(jobs - 1);
    }
    wait(0);
    // A compile that fails other than by a rule: an edit broke it.
    broken.clear();
    for (const std::string &output : outputs)
      if (auto printed = llvm::MemoryBuffer::getFile(output))
        for (llvm::StringRef line : llvm::split((*printed)->getBuffer(), '\n'))
          if (line.contains(" error: ") && !line.contains("[weavec-safe:") &&
              broken.empty())
            broken = line.str();
    std::vector<std::string> paths;
    ledgersOf(dir, paths);
    for (const std::string &path : paths)
      ok = read(path) && ok;
    return ok;
  }

  /// The units that read one of `changed`, run again.
  bool rerun(const std::set<std::string> &changed) {
    std::set<std::pair<std::string, std::vector<std::string>>> commands;
    for (const auto &[key, unit] : units)
      if (llvm::any_of(unit.files, [&](const ReadFile &file) {
            return changed.contains(file.path);
          }))
        commands.insert(
            {unit.directory,
             surveyCommand(unit.command, unit.cxx,
                           work + "/run-" + std::to_string(run + 1) + "/",
                           wantsFile())});
    if (commands.empty())
      return true;
    return runAll({commands.begin(), commands.end()});
  }

  /// §5: the declarations whose bound an error wants, for every unit to
  /// say what it sees of them.
  std::string wantsFile() const {
    std::set<std::string> wanted;
    for (const auto &[key, unit] : units)
      for (const RuleError &error : unit.errors)
        for (const RuleFix &fix : error.fixes)
          if (fix.aspect == "bound")
            wanted.insert(fix.declaration);
    const std::string path = work + "/wants.txt";
    std::error_code ec;
    llvm::raw_fd_ostream out(path, ec);
    for (const std::string &each : wanted)
      out << each << '\n';
    return path;
  }

  //===--------------------------------------------------------------------===//
  // Merging and applying
  //===--------------------------------------------------------------------===//

  /// A fix's key: its declaration and what it states of it; an agreement
  /// (C2) by what it writes too, since one base agrees with each override.
  static std::string keyOf(const RuleFix &fix) {
    return fix.declaration + "|" + fix.aspect +
           (fix.aspect == "copy" ? "|" + fix.fact : "");
  }

  bool allowed(const std::string &file) const {
    if (only.empty())
      return true;
    return llvm::any_of(only, [&](const std::string &prefix) {
      return llvm::StringRef(file).starts_with(prefix);
    });
  }

  /// §3.2: what the units say of each declaration, merged: one fix to
  /// write, or a choice.
  std::map<std::string, RuleFix>
  merge(std::map<std::string, std::string> &choices) {
    std::map<std::string, std::vector<const RuleFix *>> proposed;
    for (const auto &[key, unit] : units)
      for (const RuleError &error : unit.errors)
        if (!error.fixes.empty()) {
          const RuleFix &first = error.fixes.front();
          if (first.level != "choice" && !withdrawn.contains(keyOf(first)) &&
              !applied.contains(keyOf(first)) &&
              (!certainOnly || first.level == "certain"))
            proposed[keyOf(first)].push_back(&first);
        }
    // Every unit's evidence, once every proposal is known (§5).
    for (const auto &[key, unit] : units)
      for (const RuleFix &fix : unit.evidence)
        if (const auto found = proposed.find(keyOf(fix));
            found != proposed.end())
          found->second.push_back(&fix);
    std::map<std::string, RuleFix> merged;
    for (auto &[key, fixes] : proposed) {
      std::map<std::string, const RuleFix *> facts;
      for (const RuleFix *fix : fixes)
        facts.emplace(fix->fact, fix);
      // A string that may be null is one that is.
      if (facts.contains("null_terminated") &&
          facts.contains("null_terminated_or_null"))
        facts.erase("null_terminated");
      // Two facts, certain ones too, are a choice.
      if (facts.size() != 1) {
        std::string text;
        for (const auto &[fact, fix] : facts)
          text += (text.empty() ? "" : ", or ") + fact;
        choices[key] = "'" + facts.begin()->second->name + "': " + text;
        continue;
      }
      // The edits of every unit (a header's declaration and a source's),
      // once each, with an include if any unit needs one there.
      RuleFix fix = *facts.begin()->second;
      for (const RuleFix *each : fixes)
        for (const FixEdit &edit : each->edits) {
          if (each->fact != fix.fact)
            break;
          auto have = llvm::find_if(fix.edits, [&](const FixEdit &other) {
            return other.file == edit.file && other.offset == edit.offset &&
                   other.text == edit.text;
          });
          if (have == fix.edits.end())
            fix.edits.push_back(edit);
          else
            have->needsInclude = have->needsInclude || edit.needsInclude;
        }
      if (!fix.edits.empty() &&
          llvm::all_of(fix.edits,
                       [&](const FixEdit &edit) { return allowed(edit.file); }))
        merged.emplace(key, std::move(fix));
    }
    return merged;
  }

  bool load(const std::string &file) {
    if (texts.contains(file))
      return true;
    auto buffer = llvm::MemoryBuffer::getFile(file);
    if (!buffer)
      return false;
    texts[file] = (*buffer)->getBuffer().str();
    return true;
  }

  /// The hash the units recorded for `file` matches what is on disk.
  bool unchanged(const std::string &file) {
    if (!load(file))
      return false;
    const std::string hash = llvm::utohexstr(llvm::xxh3_64bits(texts[file]));
    for (const auto &[key, unit] : units)
      for (const ReadFile &read : unit.files)
        if (read.path == file && read.hash != hash) {
          llvm::errs() << "weavec: fix: '" << file
                       << "' changed since its ledger was written; build "
                          "again with -fweavec-safe-survey\n";
          return false;
        }
    return true;
  }

  /// Inserts at each offset of the current text, keeping the record of
  /// what the fixer inserted in step.
  void insert(const std::string &file,
              std::vector<std::pair<unsigned, Inserted>> edits) {
    std::ranges::sort(edits, [](const auto &a, const auto &b) {
      return a.first != b.first ? a.first > b.first
                                : a.second.text > b.second.text;
    });
    std::string &text = texts[file];
    std::vector<Inserted> &record = inserted[file];
    for (auto &[offset, edit] : edits) {
      if (offset > text.size())
        continue;
      text.insert(offset, edit.text);
      for (Inserted &each : record)
        if (each.offset >= offset)
          each.offset += static_cast<unsigned>(edit.text.size());
      edit.offset = offset;
      record.push_back(edit);
    }
  }

  /// Where WeaveC's include (and the file's mark) goes: after the file's
  /// last `#include` (before `limit`) outside every conditional but an
  /// include guard; without one, after the guard's `#define` or `#pragma
  /// once`, or at the start. An include inside a platform's `#if` is
  /// never the place (yaml-cpp's `dragonbox.h` has only such includes).
  static unsigned includeAt(const std::string &text,
                            std::size_t limit = std::string::npos) {
    unsigned depth = 0;
    unsigned guarded = 0;
    unsigned after = 0;
    unsigned guard = 0;
    bool first = true;
    std::size_t at = 0;
    while (at < text.size()) {
      std::size_t end = text.find('\n', at);
      end = end == std::string::npos ? text.size() : end + 1;
      llvm::StringRef line = llvm::StringRef(text).slice(at, end).trim();
      at = end;
      if (!line.consume_front("#"))
        continue;
      line = line.ltrim();
      // An include guard: the file's first directive, `#ifndef X`.
      if (first && line.starts_with("ifndef"))
        guarded = 1;
      first = false;
      if (line.starts_with("if")) {
        ++depth;
      } else if (line.starts_with("endif") && depth > 0) {
        --depth;
      } else if (end > limit) {
        continue;
      } else if (line.starts_with("include") && depth == guarded) {
        after = static_cast<unsigned>(end);
      } else if (guard == 0 &&
                 (line.starts_with("pragma once") ||
                  (line.starts_with("define") && depth == 1 && guarded == 1))) {
        guard = static_cast<unsigned>(end);
      }
    }
    return after != 0 ? after : guard;
  }

  /// Whether a line of the file says WEAVEC_SAFE_FILE.
  static bool marks(const std::string &text) {
    return llvm::any_of(llvm::split(text, '\n'), [](llvm::StringRef line) {
      line = line.trim();
      return line == "WEAVEC_SAFE_FILE" ||
             line.starts_with("WEAVEC_SAFE_FILE;");
    });
  }

  /// Whether a directive of the file includes weavec.h.
  static bool includesWeaveC(const std::string &text) {
    for (llvm::StringRef line : llvm::split(text, '\n')) {
      line = line.trim();
      if (!line.consume_front("#"))
        continue;
      line = line.ltrim();
      if (line.consume_front("include") &&
          (line.ltrim().starts_with("<weavec.h>") ||
           line.ltrim().starts_with("\"weavec.h\"")))
        return true;
    }
    return false;
  }

  /// Writes the fixes of round `round`; the files it changed.
  std::set<std::string> apply(const std::map<std::string, RuleFix> &fixes,
                              unsigned round) {
    std::map<std::string, std::vector<std::pair<unsigned, Inserted>>> byFile;
    std::set<std::string> include;
    for (const auto &[key, fix] : fixes)
      for (const FixEdit &edit : fix.edits) {
        byFile[edit.file].push_back(
            {edit.offset, {.offset = 0, .text = edit.text, .key = key}});
        if (edit.needsInclude)
          include.insert(edit.file);
      }
    std::set<std::string> changed;
    for (auto &[file, edits] : byFile) {
      // Identical edits from several units, once.
      std::ranges::sort(edits, [](const auto &a, const auto &b) {
        return std::tie(a.first, a.second.text) <
               std::tie(b.first, b.second.text);
      });
      const auto same = [](const auto &a, const auto &b) {
        return a.first == b.first && a.second.text == b.second.text;
      };
      edits.erase(std::ranges::unique(edits, same).begin(), edits.end());
      if (include.contains(file) && !includesWeaveC(texts[file]))
        edits.push_back({includeAt(texts[file], edits.front().first),
                         {.offset = 0,
                          .text = "#include <weavec.h>\n",
                          .key = "include@" + std::to_string(round)}});
      insert(file, std::move(edits));
      changed.insert(file);
    }
    for (const auto &[key, fix] : fixes)
      applied[key] = fix;
    return changed;
  }

  bool save(const std::set<std::string> &files) {
    for (const std::string &file : files) {
      std::error_code ec;
      llvm::raw_fd_ostream out(file, ec);
      if (ec) {
        llvm::errs() << "weavec: fix: cannot write '" << file
                     << "': " << ec.message() << '\n';
        return false;
      }
      out << texts[file];
    }
    return true;
  }

  /// Takes out what the fixer inserted for `key` (in `inFile`, if given); the
  /// files it changed.
  std::set<std::string> takeOut(const std::string &key,
                                const std::string &inFile = "") {
    std::set<std::string> changed;
    for (auto &[file, record] : inserted) {
      if (!inFile.empty() && file != inFile)
        continue;
      std::string &text = texts[file];
      for (std::size_t i = record.size(); i-- > 0;) {
        if (record[i].key != key)
          continue;
        const Inserted gone = record[i];
        if (text.compare(gone.offset, gone.text.size(), gone.text) == 0)
          text.erase(gone.offset, gone.text.size());
        record.erase(record.begin() + static_cast<std::ptrdiff_t>(i));
        for (Inserted &each : record)
          if (each.offset > gone.offset)
            each.offset -= static_cast<unsigned>(gone.text.size());
        changed.insert(file);
      }
    }
    return changed;
  }

  //===--------------------------------------------------------------------===//
  // Errors before and after
  //===--------------------------------------------------------------------===//

  /// The units' errors, each once (a header's are every includer's).
  std::map<std::string, const RuleError *> errors() const {
    std::map<std::string, const RuleError *> all;
    for (const auto &[key, unit] : units)
      for (const RuleError &error : unit.errors)
        all.emplace(error.file + ":" + std::to_string(error.line) + ":" +
                        std::to_string(error.column) + " " + error.rule + " " +
                        error.message,
                    &error);
    return all;
  }

  /// An error's identity across edits, which move lines and columns.
  static std::string identity(const RuleError &error) {
    return error.rule + "|" + error.message + "|" + error.owner + "|" +
           error.ownerFile;
  }

  std::multiset<std::string> withoutFix() const {
    std::multiset<std::string> found;
    for (const auto &[key, error] : errors())
      if (error->fixes.empty())
        found.insert(identity(*error));
    return found;
  }

  /// §6 step 5: the likely fixes a new error with no fix names, by its
  /// subjects or by a name its message quotes.
  std::set<std::string> blamed(const std::multiset<std::string> &before,
                               llvm::json::Array &reasons) const {
    std::multiset<std::string> seen;
    std::set<std::string> blame;
    for (const auto &[key, error] : errors()) {
      if (!error->fixes.empty())
        continue;
      const std::string id = identity(*error);
      seen.insert(id);
      if (seen.count(id) <= before.count(id))
        continue;
      for (const auto &[fixKey, fix] : applied) {
        if (fix.level != "likely")
          continue;
        std::string name = fix.name;
        if (const std::size_t scope = name.rfind("::");
            scope != std::string::npos)
          name = name.substr(scope + 2);
        const bool subject =
            llvm::is_contained(error->subjects, fix.declaration);
        // A member made a view's: the errors of the rules for views name
        // the member or its class, not the declaration (RFC 0049,
        // unresolved question 3).
        const std::string owner =
            fix.name.find("::") == std::string::npos
                ? ""
                : fix.name.substr(0, fix.name.rfind("::"));
        const bool quoted =
            fix.aspect == "borrowed" &&
            (error->message.find("'" + name + "'") != std::string::npos ||
             (!owner.empty() &&
              error->message.find("'" + owner + "'") != std::string::npos));
        if (subject || quoted) {
          blame.insert(fixKey);
          reasons.push_back(
              llvm::json::Object{{.K = "fix", .V = fixKey},
                                 {.K = "error", .V = errorJson(*error)}});
        }
      }
    }
    return blame;
  }

  //===--------------------------------------------------------------------===//
  // Marking ready files (C5)
  //===--------------------------------------------------------------------===//

  std::set<std::string> ready() const {
    std::map<std::string, std::pair<bool, bool>> files; // any open, all clean
    std::set<std::tuple<std::string, unsigned, std::string>> erring;
    std::set<std::string> globals;
    for (const auto &[key, error] : errors()) {
      if (!error->surveyed)
        continue;
      if (error->global)
        globals.insert(error->ownerFile);
      else
        erring.insert({error->ownerFile, error->ownerLine, error->owner});
    }
    for (const auto &[key, unit] : units)
      for (const Unit::Function &function : unit.functions) {
        auto &[open, clean] =
            files.try_emplace(function.file, false, true).first->second;
        if (function.safe)
          continue;
        open = true;
        clean = clean && function.surveyed &&
                !erring.contains({function.file, function.line, function.name});
      }
    std::set<std::string> found;
    for (const auto &[file, state] : files)
      if (state.first && state.second && !globals.contains(file) &&
          allowed(file) && llvm::sys::fs::exists(file))
        found.insert(file);
    return found;
  }

  std::set<std::string> markReady() {
    std::set<std::string> changed;
    for (const std::string &file : ready()) {
      if (!load(file) || !unchanged(file) || marks(texts[file]))
        continue;
      const std::string text =
          std::string(includesWeaveC(texts[file]) ? ""
                                                  : "#include <weavec.h>\n") +
          "WEAVEC_SAFE_FILE\n";
      insert(file, {{includeAt(texts[file]),
                     {.offset = 0, .text = text, .key = "mark"}}});
      marked.push_back(file);
      changed.insert(file);
    }
    return changed;
  }

  //===--------------------------------------------------------------------===//
  // The dry run's diff
  //===--------------------------------------------------------------------===//

  void diff(const std::map<std::string, RuleFix> &fixes) {
    std::map<std::string, std::map<unsigned, std::vector<std::string>>> byFile;
    for (const auto &[key, fix] : fixes)
      for (const FixEdit &edit : fix.edits)
        byFile[edit.file][edit.offset].push_back(edit.text);
    for (auto &[file, edits] : byFile) {
      if (!load(file))
        continue;
      const std::string &text = texts[file];
      llvm::outs() << "--- " << file << "\n+++ " << file << '\n';
      std::map<std::size_t, std::size_t> lines; // start -> end of each line
      for (const auto &[offset, inserts] : edits) {
        const std::size_t start =
            offset == 0 ? 0 : text.rfind('\n', offset - 1) + 1;
        lines.try_emplace(start, text.find('\n', offset));
      }
      for (const auto &[start, end] : lines) {
        const std::string old = text.substr(
            start, (end == std::string::npos ? text.size() : end) - start);
        std::string now;
        std::size_t at = start;
        for (const auto &[offset, inserts] : edits) {
          if (offset < start || offset > start + old.size())
            continue;
          now += text.substr(at, offset - at);
          for (const std::string &each : inserts)
            now += each;
          at = offset;
        }
        now += text.substr(at, start + old.size() - at);
        const unsigned line = static_cast<unsigned>(
            std::count(text.begin(),
                       text.begin() + static_cast<std::ptrdiff_t>(start),
                       '\n') +
            1);
        llvm::outs() << "@@ -" << line << " +" << line << " @@\n-" << old
                     << "\n+" << now << '\n';
      }
    }
  }
};

} // namespace

int runFixer(llvm::ArrayRef<const char *> operands, const char *argv0,
             void *mainAddress) {
  Fixer fixer;
  fixer.executable = llvm::sys::fs::getMainExecutable(argv0, mainAddress);
  std::vector<std::string> paths;
  std::vector<std::string> command;
  for (std::size_t i = 0; i < operands.size(); ++i) {
    const llvm::StringRef arg = operands[i];
    if (arg == "--") {
      for (++i; i < operands.size(); ++i)
        command.emplace_back(operands[i]);
      break;
    }
    llvm::StringRef value = arg;
    unsigned n = 0;
    if (arg == "--dry-run") {
      fixer.dryRun = true;
    } else if (arg == "--level=certain" || arg == "--level=likely") {
      fixer.certainOnly = arg == "--level=certain";
    } else if (arg == "--no-mark") {
      fixer.mark = false;
    } else if (value.consume_front("--only=")) {
      llvm::SmallString<256> here;
      std::ignore = llvm::sys::fs::current_path(here);
      for (llvm::StringRef each : llvm::split(value, ','))
        fixer.only.push_back(Fixer::absolute(each, here));
    } else if (value.consume_front("--rounds=")) {
      if (llvm::to_integer(value, n) && n != 0)
        fixer.rounds = n;
    } else if (value.consume_front("-j")) {
      if (llvm::to_integer(value, n) && n != 0)
        fixer.jobs = n;
    } else {
      ledgersOf(arg, paths);
    }
  }
  llvm::SmallString<256> work;
  if (!paths.empty()) {
    // Beside the ledgers' directory (a directory operand, or the first
    // ledger's), so that neither the fixer nor the audit reads its runs as
    // the project's ledgers.
    llvm::StringRef first = llvm::sys::path::parent_path(paths.front());
    for (const char *operand : operands)
      if (llvm::sys::fs::is_directory(operand) &&
          first.starts_with(llvm::StringRef(operand).rtrim("/")))
        first = llvm::StringRef(operand).rtrim("/");
    work = llvm::sys::path::parent_path(first);
    llvm::sys::path::append(work, "weavec-fix");
  } else {
    std::ignore = llvm::sys::fs::createUniqueDirectory("weavec-fix", work);
  }
  // Absolute: the compiles run in their own directories.
  std::ignore = llvm::sys::fs::make_absolute(work);
  std::ignore = llvm::sys::fs::create_directories(work);
  fixer.work = work.str().str();
  // What an earlier run took out stays out.
  if (auto earlier =
          llvm::MemoryBuffer::getFile(fixer.work + "/weavec-fix.json")) {
    llvm::Expected<llvm::json::Value> log =
        llvm::json::parse((*earlier)->getBuffer());
    if (!log)
      llvm::consumeError(log.takeError());
    const llvm::json::Object *object = log ? log->getAsObject() : nullptr;
    if (const llvm::json::Array *out =
            object == nullptr ? nullptr : object->getArray("withdrawn"))
      for (const llvm::json::Value &each : *out)
        if (const llvm::json::Object *entry = each.getAsObject())
          fixer.withdrawn[entry->getString("fix").value_or("").str()] =
              entry->getString("fact").value_or("").str();
  }
  if (!command.empty()) {
    // The `--` form: the command itself, surveyed first.
    llvm::SmallString<256> here;
    std::ignore = llvm::sys::fs::current_path(here);
    const bool cxx = llvm::sys::path::stem(command.front()).ends_with("++") ||
                     llvm::is_contained(command, "--driver-mode=g++");
    if (!fixer.runAll(
            {{here.str().str(),
              fixer.surveyCommand(command, cxx, fixer.work + "/run-1/", "")}}))
      return 1;
  }
  for (const std::string &path : paths)
    if (!fixer.read(path))
      return 1;
  if (fixer.units.empty()) {
    llvm::errs() << "weavec: fix: no ledger to read\n";
    return 1;
  }
  const std::size_t before = fixer.errors().size();
  std::map<std::string, std::string> choices;
  // §6: on every exit once a round may have written, the units as the
  // rules last left them (one ledger the audit reads) and the log.
  const std::string log = fixer.work + "/weavec-fix.json";
  const auto leave = [&](int status) {
    llvm::json::Array latest;
    for (auto &[key, unit] : fixer.units)
      latest.push_back(unit.raw);
    std::ignore = llvm::sys::fs::create_directories(fixer.work + "/ledger");
    std::error_code ec;
    llvm::raw_fd_ostream ledger(fixer.work + "/ledger/fixed.ledger.json", ec);
    ledger << llvm::formatv("{0:2}",
                            llvm::json::Value(llvm::json::Object{
                                {.K = "schema", .V = "weavec-ledger"},
                                {.K = "version", .V = EnforcementLedgerVersion},
                                {.K = "units", .V = std::move(latest)}}))
           << '\n';
    llvm::json::Array withdrawn;
    for (const auto &[key, fact] : fixer.withdrawn)
      withdrawn.push_back(
          llvm::json::Object{{.K = "fix", .V = key}, {.K = "fact", .V = fact}});
    llvm::json::Object chosen; // each declaration's facts
    for (const auto &[key, text] : choices)
      chosen[key] = text;
    llvm::raw_fd_ostream out(log, ec);
    out << llvm::formatv(
               "{0:2}",
               llvm::json::Value(llvm::json::Object{
                   {.K = "schema", .V = "weavec-fix"},
                   {.K = "version", .V = 1},
                   {.K = "status", .V = status},
                   {.K = "rounds", .V = llvm::json::Array(fixer.log)},
                   {.K = "withdrawn", .V = std::move(withdrawn)},
                   {.K = "choices", .V = std::move(chosen)},
                   {.K = "marked", .V = llvm::json::Array(fixer.marked)},
                   {.K = "errorsBefore",
                    .V = static_cast<std::int64_t>(before)},
                   {.K = "errorsAfter",
                    .V = static_cast<std::int64_t>(fixer.errors().size())}}))
        << '\n';
    return status;
  };
  for (unsigned round = 1; round <= fixer.rounds; ++round) {
    choices.clear();
    const std::map<std::string, RuleFix> fixes = fixer.merge(choices);
    if (fixer.dryRun) {
      fixer.diff(fixes);
      return 0;
    }
    if (fixes.empty()) {
      llvm::outs() << "weavec: fix: round " << round << ": nothing to apply\n";
      break;
    }
    // §6 step 2: a file changed since its ledger stops the fixer before
    // anything of the round is written.
    for (const auto &[key, fix] : fixes)
      for (const FixEdit &edit : fix.edits)
        if (!fixer.unchanged(edit.file))
          return leave(1);
    const std::multiset<std::string> open = fixer.withoutFix();
    const std::set<std::string> changed = fixer.apply(fixes, round);
    if (!fixer.save(changed))
      return leave(1);
    if (!fixer.rerun(changed)) {
      // The rules could not be run: the round is taken out again.
      for (const auto &[key, fix] : fixes)
        std::ignore = fixer.takeOut(key);
      std::ignore = fixer.takeOut("include@" + std::to_string(round));
      std::ignore = fixer.save(changed);
      return leave(1);
    }
    // §6 step 5: a likely fix that brought an error with no fix.
    llvm::json::Array reasons;
    std::set<std::string> blame = fixer.blamed(open, reasons);
    std::set<std::string> back;
    if (!fixer.broken.empty()) {
      llvm::errs() << "weavec: fix: round " << round
                   << " broke a compile, and is taken out: " << fixer.broken
                   << '\n';
      // What it broke, for the log's reader.
      const std::string kept =
          fixer.work + "/broken-" + std::to_string(round) + "/";
      std::ignore = llvm::sys::fs::create_directories(kept);
      for (const std::string &file : changed) {
        std::error_code error;
        llvm::raw_fd_ostream out(kept + llvm::sys::path::filename(file).str(),
                                 error);
        out << fixer.texts[file];
      }
      // Its likely fixes are withdrawn and its certain ones tried again
      // without them; certain ones that break a compile alone, withdrawn.
      const bool likely = llvm::any_of(fixes, [](const auto &each) {
        return each.second.level == "likely";
      });
      for (const auto &[key, fix] : fixes) {
        if (likely && fix.level == "certain") {
          fixer.applied.erase(key);
          back.merge(fixer.takeOut(key));
          continue;
        }
        blame.insert(key);
        reasons.push_back(llvm::json::Object{
            {.K = "fix", .V = key}, {.K = "broke", .V = fixer.broken}});
      }
      back.merge(fixer.takeOut("include@" + std::to_string(round)));
    }
    for (const std::string &key : blame) {
      fixer.withdrawn[key] =
          fixer.applied.contains(key) ? fixer.applied[key].fact : "";
      fixer.applied.erase(key);
      back.merge(fixer.takeOut(key));
    }
    // An include the fixer added where no fix of its is left.
    for (const std::string &file : back)
      if (llvm::all_of(fixer.inserted[file], [](const Inserted &each) {
            return llvm::StringRef(each.key).starts_with("include@");
          }))
        for (unsigned each = 1; each <= round; ++each)
          std::ignore = fixer.takeOut("include@" + std::to_string(each), file);
    if (!back.empty() && (!fixer.save(back) || !fixer.rerun(back)))
      return leave(1);
    if (!fixer.broken.empty()) {
      llvm::errs() << "weavec: fix: a compile still fails: " << fixer.broken
                   << '\n';
      return leave(1);
    }
    unsigned certain = 0;
    unsigned edits = 0;
    llvm::json::Array written;
    for (const auto &[key, fix] : fixes) {
      certain += fix.level == "certain" ? 1 : 0;
      edits += static_cast<unsigned>(fix.edits.size());
      written.push_back(fixJson(fix));
    }
    fixer.log.push_back(
        llvm::json::Object{{.K = "round", .V = round},
                           {.K = "applied", .V = std::move(written)},
                           {.K = "withdrawn", .V = std::move(reasons)}});
    llvm::outs() << "weavec: fix: round " << round << ": " << edits
                 << " edits in " << changed.size() << " files (" << certain
                 << " certain, " << fixes.size() - certain << " likely)";
    if (!blame.empty())
      llvm::outs() << ", " << blame.size() << " withdrawn";
    llvm::outs() << "; " << fixer.errors().size() << " errors after\n";
  }
  if (fixer.mark) {
    const std::set<std::string> changed = fixer.markReady();
    if (!changed.empty() && (!fixer.save(changed) || !fixer.rerun(changed)))
      return leave(1);
    llvm::outs() << "weavec: fix: marked " << changed.size()
                 << " files WEAVEC_SAFE_FILE\n";
  }
  unsigned choice = 0;
  unsigned none = 0;
  const auto after = fixer.errors();
  for (const auto &[key, error] : after)
    ++(error->fixes.empty() ? none : choice);
  std::size_t edits = 0;
  std::set<std::string> files;
  for (const auto &[file, record] : fixer.inserted) {
    edits += record.size();
    if (!record.empty())
      files.insert(file);
  }
  llvm::outs() << "weavec: fix: " << edits << " edits in " << files.size()
               << " files; " << before << " errors before, " << after.size()
               << " after: " << choice << " have a choice or a fix withdrawn, "
               << none << " have no fix\n";
  llvm::outs() << "weavec: fix: '" << llvm::sys::path::filename(argv0)
               << " --weavec-audit " << fixer.work << "/ledger"
               << "' prints what is left; " << log << " is the log\n";
  return leave(0);
}

} // namespace weavec::frontend
