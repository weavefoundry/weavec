//===- SidecarTest.cpp - Tests for the per-object summary file ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/Sidecar.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <gtest/gtest.h>

#include <string>

namespace weavec::frontend {
namespace {

using core::FunctionSummary;
using core::PlaceEffect;
using core::SummaryPath;
using core::ValueSource;

UnitRecord sample() {
  UnitRecord record;
  analysis::UnitExports &exports = record.exports;
  exports.source = "src/node.c";
  record.workingDirectory = "/work/build";
  record.command = {"-triple",   "arm64-apple-macosx14.0.0",
                    "-emit-obj", "-o",
                    "node.o",    "-x",
                    "c",         "../src/node.c"};
  exports.imports = {"free", "malloc"};
  exports.indirectTypes = {"void (void *)"};
  exports.unknownCallees = {"blob_open"};
  exports.unknownIndirectTypes = {"int (struct opaque *)"};
  // RFC 0010: a count field some function of the unit releases through.
  exports.countFields = {"struct node.rc"};
  record.reported.insert(ReportedDiagnostic{.id = "use-after-free",
                                            .file = "../src/node.c",
                                            .line = 17,
                                            .column = 10});

  const std::uint32_t cache = exports.globals.idFor("g_cache");
  FunctionSummary freeSummary;
  freeSummary.addEffect(SummaryPath::param(0), PlaceEffect{.freed = true});
  freeSummary.addEffect(SummaryPath::global(cache), PlaceEffect{.freed = true});
  exports.functions["node_free"] =
      analysis::ExportedFunction{.summary = freeSummary,
                                 .typeKey = "void (struct node *)",
                                 .external = true,
                                 .addressTaken = true};

  FunctionSummary newSummary;
  newSummary.addReturn(ValueSource::fresh());
  exports.functions["node_new"] =
      analysis::ExportedFunction{.summary = newSummary,
                                 .typeKey = "struct node *(void)",
                                 .external = true,
                                 .addressTaken = false};

  FunctionSummary helper;
  helper.addReturn(
      ValueSource::borrow(SummaryPath::param(0).deref().field("v")));
  exports.functions["vp"] = analysis::ExportedFunction{.summary = helper,
                                                       .typeKey = "",
                                                       .external = false,
                                                       .addressTaken = true};

  // RFC 0006: an outcome-conditional consumer and an interior copy.
  FunctionSummary grow;
  grow.addEffect(SummaryPath::param(0), PlaceEffect{.moved = true});
  grow.addReturn(ValueSource::fresh());
  grow.addReturn(ValueSource::null());
  grow.addReturn(ValueSource::interiorCopy(SummaryPath::param(0)));
  grow.addOutcome(core::Outcome::Null);
  grow.addOutcome(core::Outcome::NonNull, SummaryPath::param(0),
                  PlaceEffect{.moved = true});
  exports.functions["grow"] =
      analysis::ExportedFunction{.summary = grow,
                                 .typeKey = "char *(char *, unsigned long)",
                                 .external = true,
                                 .addressTaken = false};
  return record;
}

TEST(Sidecar, PathIsOutputPlusExtension) {
  EXPECT_EQ(sidecarPathFor("obj/node.o"), "obj/node.o.weavec");
}

TEST(Sidecar, PrintsStableText) {
  EXPECT_EQ(printUnitRecord(sample()),
            "weavec-summaries 7\n"
            "source src/node.c\n"
            "cwd /work/build\n"
            "arg -triple\n"
            "arg arm64-apple-macosx14.0.0\n"
            "arg -emit-obj\n"
            "arg -o\n"
            "arg node.o\n"
            "arg -x\n"
            "arg c\n"
            "arg ../src/node.c\n"
            "import free\n"
            "import malloc\n"
            "indirect void (void *)\n"
            "unknown blob_open\n"
            "unknown-indirect int (struct opaque *)\n"
            "count-field struct node.rc\n"
            "reported use-after-free 17 10 ../src/node.c\n"
            "function grow external plain char *(char *, unsigned long)\n"
            "summary\n"
            "  effect param 0 moved\n"
            "  return fresh\n"
            "  return copy param 0 @?\n"
            "  return null\n"
            "  outcome null\n"
            "  outcome nonnull param 0 moved\n"
            "end\n"
            "function node_free external address-taken void (struct node *)\n"
            "summary\n"
            "  effect param 0 freed\n"
            "  effect global g_cache freed\n"
            "end\n"
            "function node_new external plain struct node *(void)\n"
            "summary\n"
            "  return fresh\n"
            "end\n"
            "function vp internal address-taken\n"
            "summary\n"
            "  return borrow param 0 *.v\n"
            "end\n");
}

TEST(Sidecar, RoundTrips) {
  const UnitRecord original = sample();
  std::string error;
  const std::optional<UnitRecord> parsed =
      parseUnitRecord(printUnitRecord(original), &error);
  ASSERT_TRUE(parsed) << error;
  EXPECT_EQ(parsed->exports.source, original.exports.source);
  EXPECT_EQ(parsed->workingDirectory, original.workingDirectory);
  EXPECT_EQ(parsed->command, original.command);
  EXPECT_EQ(parsed->exports.imports, original.exports.imports);
  EXPECT_EQ(parsed->exports.indirectTypes, original.exports.indirectTypes);
  EXPECT_EQ(parsed->exports.unknownCallees, original.exports.unknownCallees);
  EXPECT_EQ(parsed->exports.unknownIndirectTypes,
            original.exports.unknownIndirectTypes);
  EXPECT_EQ(parsed->reported, original.reported);
  ASSERT_EQ(parsed->exports.functions.size(), 4U);
  for (const auto &[name, function] : original.exports.functions) {
    const auto it = parsed->exports.functions.find(name);
    ASSERT_NE(it, parsed->exports.functions.end()) << name;
    EXPECT_EQ(it->second.typeKey, function.typeKey) << name;
    EXPECT_EQ(it->second.external, function.external) << name;
    EXPECT_EQ(it->second.addressTaken, function.addressTaken) << name;
  }
  // Globals are interned in the order they are met, which is the print
  // order here, so the summaries compare equal directly.
  EXPECT_TRUE(parsed->exports.sameSummariesAs(original.exports));
  EXPECT_EQ(parsed->exports.globals.find("g_cache"),
            std::optional<std::uint32_t>(0));
}

TEST(Sidecar, RejectsOtherFormatsAndMalformedLines) {
  std::string error;
  EXPECT_FALSE(parseUnitRecord("weavec-summaries 1\n", &error));
  EXPECT_EQ(error, "unsupported format 1");
  EXPECT_FALSE(parseUnitRecord("weavec-summaries 8\n", &error));
  EXPECT_EQ(error, "unsupported format 8");
  EXPECT_FALSE(parseUnitRecord("ELF\x01\x02", &error));
  EXPECT_EQ(error, "not a weavec summary file");
  EXPECT_FALSE(parseUnitRecord("", &error));
  EXPECT_EQ(error, "empty file");
  EXPECT_FALSE(parseUnitRecord(
      "weavec-summaries 7\nsummary\n  return fresh\nend\n", &error));
  EXPECT_EQ(error, "line 2: summary record without a function");
  EXPECT_FALSE(parseUnitRecord("weavec-summaries 7\nfunction f\n", &error));
  EXPECT_EQ(error, "line 2: malformed 'function' line");
  EXPECT_FALSE(parseUnitRecord("weavec-summaries 7\nfunction f external "
                               "plain\nsummary\n  return fresh\n",
                               &error));
  EXPECT_EQ(error, "line 4: summary record without 'end'");
  EXPECT_FALSE(parseUnitRecord("weavec-summaries 7\nreported x y z\n", &error));
  EXPECT_EQ(error, "line 2: malformed 'reported' line");
}

TEST(Sidecar, SkipsUnknownLinesAndBlankOnes) {
  std::string error;
  const std::optional<UnitRecord> parsed = parseUnitRecord(
      "weavec-summaries 7\n\nfuture-thing 42\nsource a.c\n\n", &error);
  ASSERT_TRUE(parsed) << error;
  EXPECT_EQ(parsed->exports.source, "a.c");
  EXPECT_TRUE(parsed->exports.functions.empty());
}

TEST(Sidecar, WritesAndReadsFiles) {
  llvm::SmallString<128> dir;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("weavec-sidecar", dir));
  llvm::SmallString<128> path(dir);
  llvm::sys::path::append(path, "node.o.weavec");

  std::string error;
  ASSERT_TRUE(writeSidecar(path, sample(), &error)) << error;
  EXPECT_FALSE(llvm::sys::fs::exists(path + ".tmp"));
  const std::optional<UnitRecord> back = readSidecar(path, &error);
  ASSERT_TRUE(back) << error;
  EXPECT_EQ(printUnitRecord(*back), printUnitRecord(sample()));

  llvm::SmallString<128> missing(dir);
  llvm::sys::path::append(missing, "nothing.weavec");
  EXPECT_FALSE(readSidecar(missing, &error));
  EXPECT_FALSE(error.empty());

  (void)llvm::sys::fs::remove(path);
  (void)llvm::sys::fs::remove(dir);
}

} // namespace
} // namespace weavec::frontend
