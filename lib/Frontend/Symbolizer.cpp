//===- Symbolizer.cpp - weavec-cc --weavec-symbolize ----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0040 §4.2, §4.4. Without an operand, `weavec-cc --weavec-symbolize`
// reads `CODE <module> <address>` lines and answers each as llvm-symbolizer
// does (a function line and a `file:line:column` line per frame, inlined
// calls first, then a blank line), from the module's line tables through
// LLVM's Symbolize library: the runtime's symbolizer on Linux. With a log's
// path it rewrites the report frames the runtime could only name by module
// and offset, `#<n> <pc> in <function> (<module>+<offset>)`, on a machine
// with the same binaries.
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/Driver.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/DebugInfo/Symbolize/Symbolize.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/raw_ostream.h"

#include <iostream>
#include <map>
#include <string>

namespace weavec::frontend {

using llvm::symbolize::LLVMSymbolizer;

/// A Mach-O module's offsets are from its header; its addresses start at
/// its `__TEXT` segment's.
static std::uint64_t textBase(const std::string &module) {
  auto binary = llvm::object::ObjectFile::createObjectFile(module);
  if (!binary) {
    llvm::consumeError(binary.takeError());
    return 0;
  }
  const auto *macho =
      llvm::dyn_cast<llvm::object::MachOObjectFile>(binary->getBinary());
  if (macho == nullptr)
    return 0;
  for (const auto &command : macho->load_commands())
    if (command.C.cmd == llvm::MachO::LC_SEGMENT_64) {
      const llvm::MachO::segment_command_64 segment =
          macho->getSegment64LoadCommand(command);
      if (llvm::StringRef(segment.segname) == "__TEXT")
        return segment.vmaddr;
    }
  return 0;
}

/// The frames of `address` in `module`, innermost first: each a function
/// and a location, `??` when unknown.
static std::vector<std::pair<std::string, std::string>>
frames(LLVMSymbolizer &symbolizer, const std::string &module,
       std::uint64_t address) {
  std::vector<std::pair<std::string, std::string>> out;
  auto inlined = symbolizer.symbolizeInlinedCode(
      module, {.Address = address,
               .SectionIndex = llvm::object::SectionedAddress::UndefSection});
  if (inlined) {
    for (std::uint32_t i = 0; i < inlined->getNumberOfFrames(); ++i) {
      const llvm::DILineInfo &frame = inlined->getFrame(i);
      std::string function = frame.FunctionName;
      if (function == llvm::DILineInfo::BadString)
        function = "??";
      std::string where = frame.FileName == llvm::DILineInfo::BadString
                              ? std::string("??:0:0")
                              : frame.FileName + ":" +
                                    std::to_string(frame.Line) + ":" +
                                    std::to_string(frame.Column);
      out.emplace_back(std::move(function), std::move(where));
    }
  } else {
    llvm::consumeError(inlined.takeError());
  }
  if (out.empty())
    out.emplace_back("??", "??:0:0");
  return out;
}

static LLVMSymbolizer::Options symbolizerOptions() {
  LLVMSymbolizer::Options options;
  options.Demangle = true;
  options.UseSymbolTable = true;
  options.RelativeAddresses = false;
  return options;
}

/// The protocol: `CODE <module> <address>`, the module quoted when it has
/// spaces.
static int answer() {
  LLVMSymbolizer symbolizer(symbolizerOptions());
  std::string line;
  while (std::getline(std::cin, line)) {
    llvm::StringRef rest = llvm::StringRef(line).trim();
    std::string module;
    if (!rest.consume_front("CODE "))
      continue;
    rest = rest.ltrim();
    if (rest.consume_front("\"")) {
      const std::size_t end = rest.find('"');
      module = rest.substr(0, end).str();
      rest = end == llvm::StringRef::npos ? "" : rest.substr(end + 1);
    } else {
      const std::size_t end = rest.find(' ');
      module = rest.substr(0, end).str();
      rest = end == llvm::StringRef::npos ? "" : rest.substr(end);
    }
    std::uint64_t address = 0;
    if (rest.trim().getAsInteger(0, address))
      address = 0;
    for (const auto &[function, where] : frames(symbolizer, module, address))
      llvm::outs() << function << '\n' << where << '\n';
    llvm::outs() << '\n';
    llvm::outs().flush();
  }
  return 0;
}

/// Rewrites a saved report's unnamed frames.
static int rewrite(llvm::StringRef path) {
  auto buffer = llvm::MemoryBuffer::getFileOrSTDIN(path, /*IsText=*/true);
  if (!buffer) {
    llvm::errs() << "weavec-cc: error: cannot read '" << path
                 << "': " << buffer.getError().message() << '\n';
    return 1;
  }
  LLVMSymbolizer symbolizer(symbolizerOptions());
  std::map<std::string, std::uint64_t> bases;
  const llvm::Regex frame(
      R"(^( +#[0-9]+ 0x[0-9a-f]+ in )[^ ]+ \((.+)\+0x([0-9a-f]+)\)$)");
  llvm::SmallVector<llvm::StringRef, 0> lines;
  (*buffer)->getBuffer().split(lines, '\n');
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const llvm::StringRef text = lines[i];
    llvm::SmallVector<llvm::StringRef, 4> parts;
    std::uint64_t offset = 0;
    if (frame.match(text, &parts) && !parts[3].getAsInteger(16, offset)) {
      const std::string module = parts[2].str();
      const auto [base, added] = bases.try_emplace(module, 0);
      if (added)
        base->second = textBase(module);
      const auto found = frames(symbolizer, module, base->second + offset);
      if (found.front().second != "??:0:0") {
        for (const auto &[function, where] : found)
          llvm::outs() << parts[1] << function << ' ' << where << '\n';
        continue;
      }
    }
    llvm::outs() << text;
    if (i + 1 < lines.size())
      llvm::outs() << '\n';
  }
  return 0;
}

int runSymbolizer(llvm::ArrayRef<const char *> operands) {
  if (operands.empty())
    return answer();
  int status = 0;
  for (const char *operand : operands)
    if (rewrite(operand) != 0)
      status = 1;
  return status;
}

} // namespace weavec::frontend
