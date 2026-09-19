#!/usr/bin/env python3
"""Tests for scripts/check-hygiene.py (RFC 0030, gate H2).

Each test builds a small fake WeaveC tree in a temporary directory. Those
trees are not git work trees, so the checker walks them; GitSelectionTest
makes a git repository to check that ignored files are not scanned.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("check_hygiene", HERE / "check-hygiene.py")
hygiene = importlib.util.module_from_spec(SPEC)
sys.modules["check_hygiene"] = hygiene
SPEC.loader.exec_module(hygiene)

# The real grammar: comments, continuation lines, directives, chk aliases.
LIBRARY_SPEC = r"""
# LibrarySpec.txt - a small table in the real grammar.
#   fake (int) -> void;     <- a commented-out entry is no entry
header stdlib.h;   # a directive with a comment
malloc (int) -> fresh(free):extent(a0):null-on-failure:zero-init;
free (none:null-ok:release(free)) -> void;
header string.h;
memcpy (w:bytes(a2), r:bytes(a2), int) -> arg(0) disjoint(0,1,a2) \
    copies(0,1,a2) chk(__builtin___memcpy_chk: 0,1,2,-1) \
    chk(__memcpy_chk: 0,1,2,-1);
fgets (w:bytes(a1), int, rw) -> \
    arg(0):null-on-failure:str chk(__fgets_chk: 0,-1,1,2);
builtins;
__builtin_expect (int, int) -> int;
"""

DATAFLOW_H = """\
#ifndef WEAVEC_LIB_ANALYSIS_DATAFLOW_H
#define WEAVEC_LIB_ANALYSIS_DATAFLOW_H

#include "weavec/Analysis/FunctionAnalysis.h"
#include "weavec/Analysis/LedgerAdapter.h"

namespace weavec::analysis {

/// Everything goes through `ledgerAdapter`; see RFC 0030 section 14.
class FunctionDataflow {
public:
  FunctionDataflow(clang::ASTContext &ctx, LedgerAdapter &ledgerAdapter,
                   const AnalysisOptions &analysisOptions, bool emitDiags);
  void run();

private:
  // Not a DiagnosticSink: the adapter reports. A closing brace: }
  const char *closing = "}";
  const AnalysisOptions &options;
  // MEMBERS
};

void describe(const FunctionDataflow &dataflow);

} // namespace weavec::analysis

#endif // WEAVEC_LIB_ANALYSIS_DATAFLOW_H
"""

DATAFLOW_CPP = """\
#include "Dataflow.h"

using namespace weavec::analysis;

FunctionDataflow::FunctionDataflow(ASTContext &ctx, LedgerAdapter &ledgerAdapter,
                                   const AnalysisOptions &analysisOptions,
                                   bool emitDiags)
    : options(analysisOptions) {}

void FunctionDataflow::run() {}
"""

FUNCTION_ANALYSIS_H = """\
#pragma once
namespace weavec::analysis {
struct AnalysisOptions {
  bool strictExterns = false;
  // OPTIONS
};
} // namespace weavec::analysis
"""

# A tree that passes every check.
CLEAN_TREE = {
    "lib/CMakeLists.txt": "add_subdirectory(Core)\nadd_subdirectory(Analysis)\n",
    "lib/Core/LibrarySpec.txt": LIBRARY_SPEC,
    "lib/Core/LibrarySpec.cpp": 'bool isFree(std::string_view n) { return n == "free"; }\n',
    "lib/Analysis/Dataflow.h": DATAFLOW_H,
    "lib/Analysis/Dataflow.cpp": DATAFLOW_CPP,
    "lib/Analysis/LedgerAdapter.cpp": '#include "weavec/Analysis/LedgerAdapter.h"\n',
    "include/weavec/Analysis/LedgerAdapter.h": (
        '#pragma once\n#include "weavec/Core/Diagnostic.h"\n'
        "// The adapter owns the sink: that is the seam.\n"
        "class LedgerAdapter {\n  weavec::core::DiagnosticSink &sink;\n};\n"),
    "include/weavec/Analysis/FunctionAnalysis.h": FUNCTION_ANALYSIS_H,
    "include/weavec/Core/Diagnostic.h": "#pragma once\nclass DiagnosticSink {};\n",
    "tools/weavec/main.cpp": "int main() { return 0; }\n",
    "docs/rfcs/0030-prove-or-trap.md": "The --checked flag and SafetyState are retired.\n",
    "docs/pages/reference/cli.md": "# CLI\n\nweavec [options] files\n",
}


class TreeTest(unittest.TestCase):
    """A clean fake tree in a temporary directory, which tests then break."""

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="weavec-hygiene-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        for path, text in CLEAN_TREE.items():
            self.write(path, text)

    def write(self, path, text):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(textwrap.dedent(text).lstrip("\n"))
        return target

    def edit(self, path, old, new):
        target = self.root / path
        text = target.read_text()
        self.assertIn(old, text)
        target.write_text(text.replace(old, new, 1))

    def found(self, check):
        """(path, line, message) of each violation of `check`."""
        return [(v.path, v.line, v.message) for v in hygiene.run_checks(self.root).violations
                if v.check == check]

    def run_main(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = hygiene.main(["--root", str(self.root), *args])
        return code, out.getvalue()


class CleanTreeTest(TreeTest):
    def test_clean_tree_passes_every_check(self):
        result = hygiene.run_checks(self.root)
        self.assertEqual(result.violations, [])
        self.assertEqual(result.file_source, "walk")
        self.assertEqual((result.library_entries, result.library_aliases), (5, 3))
        code, output = self.run_main()
        self.assertEqual(code, 0)
        self.assertTrue(output.startswith("Summary (RFC 0030, gate H2)\n"), output)
        self.assertEqual(output.splitlines()[-1], "H2: PASS (0 violations)")

    def test_walk_skips_build_and_tool_output(self):
        for skipped in ("node_modules/pkg", "dist", ".generated", ".astro", "test-results",
                        "playwright-report", "__pycache__", "build-release"):
            self.write(f"docs/{skipped}/notes.md", "--checked\n")
            self.write(f"lib/{skipped}/Notes.cpp", "SafetyState state;\n")
        self.assertEqual(self.found("retired-name"), [])


class RetiredNameTest(TreeTest):
    def test_docs_rfcs_are_exempt_but_other_docs_are_not(self):
        self.write("docs/rfcs/0018-checked-code.md", "Use --checked and checkContracts.\n")
        self.write("docs/pages/reference/cli.md", """
            # CLI

            --checked enables checked mode.
            See `--checked-report` too.
            """)
        self.assertEqual(self.found("retired-name"), [
            ("docs/pages/reference/cli.md", 3, "'--checked' at column 1"),
            ("docs/pages/reference/cli.md", 4, "'--checked' at column 6"),
        ])

    def test_lib_and_tools_count_but_include_is_not_scanned(self):
        self.write("lib/Core/Safety.cpp", "// safetystate is fine\nSafetyState a; CheckedContract b;\n")
        self.write("tools/weavec/Options.cpp", "bool checkContracts = false;\n")
        self.write("include/weavec/Core/Safety.h", "struct CheckedContract;\n")
        self.assertEqual(self.found("retired-name"), [
            ("lib/Core/Safety.cpp", 2, "'SafetyState' at column 1"),
            ("lib/Core/Safety.cpp", 2, "'CheckedContract' at column 16"),
            ("tools/weavec/Options.cpp", 1, "'checkContracts' at column 6"),
        ])


class LibrarySpecNamesTest(unittest.TestCase):
    def test_logical_lines_drop_comments_and_join_continuations(self):
        lines = {first: " ".join(text.split()) for first, text in hygiene.library_spec_lines(LIBRARY_SPEC)}
        self.assertEqual(lines[3], "")  # a comment line
        self.assertEqual(lines[4], "header stdlib.h;")
        self.assertEqual(lines[8], "memcpy (w:bytes(a2), r:bytes(a2), int) -> arg(0) disjoint(0,1,a2) "
                                   "copies(0,1,a2) chk(__builtin___memcpy_chk: 0,1,2,-1) "
                                   "chk(__memcpy_chk: 0,1,2,-1);")
        self.assertEqual(lines[11], "fgets (w:bytes(a1), int, rw) -> arg(0):null-on-failure:str "
                                    "chk(__fgets_chk: 0,-1,1,2);")
        self.assertNotIn(9, lines)  # joined to line 8

    def test_entries_aliases_and_builtin_spellings(self):
        library = hygiene.library_names(LIBRARY_SPEC)
        self.assertEqual((library.entries, library.aliases), (5, 3))
        names = library.names
        for entry in ("malloc", "free", "memcpy", "fgets", "__builtin_expect"):
            self.assertEqual(names[entry], "entry")
        self.assertEqual(names["__memcpy_chk"], "chk alias (of memcpy)")
        self.assertEqual(names["__builtin___memcpy_chk"], "chk alias (of memcpy)")
        self.assertEqual(names["__builtin_free"], "__builtin_ spelling (of free)")
        self.assertEqual(names["__builtin___fgets_chk"],
                         "__builtin_ spelling (of __fgets_chk, a chk alias of fgets)")
        for word in ("header", "builtins", "stdlib", "string", "fake", "arg", "chk", "copies"):
            self.assertNotIn(word, names)
        self.assertEqual(len(names), 15)  # 5 entries, 3 aliases, 7 new __builtin_ spellings


class LibraryNameTestTest(TreeTest):
    def test_comparisons_in_either_order_outside_comments(self):
        self.write("lib/Analysis/Calls.cpp", r'''
            bool a(llvm::StringRef name) { return name == "free"; }
            bool b(llvm::StringRef name) { return "free" == name; }
            bool c(llvm::StringRef name) { return name == "freedom" || name != "free"; }
            bool d(llvm::StringRef name) { return name=="malloc"; }
            // return name == "free";
            /* name == "malloc" */
            bool e(llvm::StringRef name) { return name ==
                                                  "memcpy"; }
            const char *f = "name == \"free\"";
            bool g(std::string_view n) { return n == "__memcpy_chk" || n == "__builtin_memcpy"; }
            bool h(std::string_view n) { return "fgets"sv == n || n == u8"free"; }
            ''')
        entry = "compares against a LibrarySpec entry"
        self.assertEqual(self.found("library-name-test"), [
            ("lib/Analysis/Calls.cpp", 1, f'== "free" {entry}'),
            ("lib/Analysis/Calls.cpp", 2, f'"free" == {entry}'),
            ("lib/Analysis/Calls.cpp", 4, f'== "malloc" {entry}'),
            ("lib/Analysis/Calls.cpp", 8, f'== "memcpy" {entry}'),
            ("lib/Analysis/Calls.cpp", 10,
             '== "__memcpy_chk" compares against a LibrarySpec chk alias (of memcpy)'),
            ("lib/Analysis/Calls.cpp", 10,
             '== "__builtin_memcpy" compares against a LibrarySpec __builtin_ spelling (of memcpy)'),
            ("lib/Analysis/Calls.cpp", 11, f'"fgets" == {entry}'),
            ("lib/Analysis/Calls.cpp", 11, f'== "free" {entry}'),
        ])

    def test_only_c_sources_outside_library_spec_count(self):
        self.write("lib/Core/LibrarySpecTable.inc", 'if (name == "free") return;\n')
        self.write("lib/Analysis/notes.md", 'name == "free"\n')
        self.write("include/weavec/Analysis/Names.def", 'CASE(name == "malloc")\n')
        self.write("tools/weavec/main.cc", 'bool x = arg == "fgets";\n')
        self.write("docs/examples/names.c", 'int y = arg == "free";\n')
        self.assertEqual([(path, line) for path, line, _ in self.found("library-name-test")],
                         [("include/weavec/Analysis/Names.def", 1), ("tools/weavec/main.cc", 1)])

    def test_lexing_keeps_literals_comments_and_numbers_apart(self):
        self.write("lib/Analysis/Lexing.cpp", r'''
            auto raw = R"x(a)" == "free")x";
            char quote = '"'; bool a = n == "malloc";
            long big = 1'000; bool b = n == "free";
            bool c = n == "fr" "ee";
            const char *url = "http://example.org"; bool d = n == "fgets";
            /* don't */ bool e = n == "memcpy";
            ''')
        self.assertEqual([(line, message.split()[1]) for _, line, message in self.found("library-name-test")],
                         [(2, '"malloc"'), (3, '"free"'), (5, '"fgets"'), (6, '"memcpy"')])

    def test_missing_library_spec_is_a_violation(self):
        (self.root / "lib/Core/LibrarySpec.txt").unlink()
        self.write("lib/Analysis/Calls.cpp", 'bool a = n == "free";\n')
        code, output = self.run_main()
        self.assertEqual(code, 1)
        self.assertEqual(output.splitlines()[0], "lib/Core/LibrarySpec.txt: library-name-test: "
                                                 "missing or unreadable, so the LibrarySpec names are unknown")
        self.assertIn("  library-name-test      1  ", output)


class CorpusWordTest(TreeTest):
    def test_word_rule(self):
        pattern = hygiene.corpus_word_pattern()
        line = "cJSON_IsString lua_State evaluation sdsnew LUA"
        words = [word for _, _, word in hygiene.find_all(pattern, line)]
        self.assertEqual(words, ["cJSON", "lua"])
        self.write("lib/Analysis/Words.cpp", """
            // cJSON_IsString and lua_State are corpus names.
            int evaluation = sdsnew() + LUA + Zlib + lua2 + sds9 + x_sds;
            #include <zlib.h> // Lua's jansson, linenoise, jsmn, sds, minigzip
            """)
        self.assertEqual([(line, message) for _, line, message in self.found("corpus-word")], [
            (1, "word 'cJSON' at column 4"),
            (1, "word 'lua' at column 23"),
            (2, "word 'sds' at column 58"),  # x_sds: the underscore is a boundary
            (3, "word 'zlib' at column 11"),
            (3, "word 'Lua' at column 22"),
            (3, "word 'jansson' at column 28"),
            (3, "word 'linenoise' at column 37"),
            (3, "word 'jsmn' at column 48"),
            (3, "word 'sds' at column 54"),
            (3, "word 'minigzip' at column 59"),
        ])

    def test_every_file_under_lib_and_nothing_else(self):
        self.edit("lib/Core/LibrarySpec.txt", "a small table", "a small table (jansson.h is not listed)")
        self.write("lib/Analysis/README.txt", "Measured on zlib.\n")
        self.write("include/weavec/Analysis/Corpus.h", "// lua\n")
        self.write("tools/weavec/main.cpp", "// cJSON\n")
        self.write("docs/pages/corpus.md", "sds\n")
        self.assertEqual(self.found("corpus-word"), [
            ("lib/Analysis/README.txt", 1, "word 'zlib' at column 13"),
            ("lib/Core/LibrarySpec.txt", 1, "word 'jansson' at column 36"),
        ])


class DataflowIncludeTest(TreeTest):
    def test_direct_includes_resolved_or_not(self):
        self.write("lib/Analysis/SiteCollector.cpp", """
            #include "weavec/Analysis/SiteCollector.h"
            #include "Dataflow.h"
            """)
        self.write("include/weavec/Analysis/CheckPlanner.h", """
            #pragma once
            #  include <weavec/Analysis/Dataflow.h>
            """)
        self.assertEqual(self.found("dataflow-include"), [
            ("include/weavec/Analysis/CheckPlanner.h", 2, "includes Dataflow.h directly: "
             "include/weavec/Analysis/CheckPlanner.h:2 -> <weavec/Analysis/Dataflow.h>"),
            ("lib/Analysis/SiteCollector.cpp", 2, "includes Dataflow.h directly: "
             "lib/Analysis/SiteCollector.cpp:2 -> lib/Analysis/Dataflow.h"),
        ])

    def test_transitive_include_reports_the_chain(self):
        self.write("lib/Analysis/KindInferenceFields.cpp", """
            // Field invariants.

            #include "KindInferenceImpl.h"
            """)
        self.write("lib/Analysis/KindInferenceImpl.h", """
            #pragma once
            #include "weavec/Analysis/Engine.h"
            """)
        self.write("include/weavec/Analysis/Engine.h", """
            #pragma once
            #include "weavec/Analysis/LedgerAdapter.h"

            #include "Dataflow.h"
            """)
        self.assertEqual(self.found("dataflow-include"), [
            ("lib/Analysis/KindInferenceFields.cpp", 3, "includes Dataflow.h transitively: "
             "lib/Analysis/KindInferenceFields.cpp:3 -> lib/Analysis/KindInferenceImpl.h:2 -> "
             "include/weavec/Analysis/Engine.h:4 -> lib/Analysis/Dataflow.h"),
            ("lib/Analysis/KindInferenceImpl.h", 2, "includes Dataflow.h transitively: "
             "lib/Analysis/KindInferenceImpl.h:2 -> include/weavec/Analysis/Engine.h:4 -> "
             "lib/Analysis/Dataflow.h"),
        ])

    def test_other_files_comments_and_cycles_do_not_count(self):
        self.write("lib/Analysis/DataflowEngine.cpp", '#include "Dataflow.h"\n')
        self.write("lib/Analysis/SlotCollector.cpp", """
            // #include "Dataflow.h"
            /* #include "Dataflow.h" */
            #include "Cycle.h"
            #include "DataflowTypes.h"
            """)
        self.write("lib/Analysis/Cycle.h", '#include "SlotCollector.h"\n#include "Cycle.h"\n')
        self.write("lib/Analysis/SlotCollector.h", '#include "Cycle.h"\n')
        self.write("lib/Analysis/DataflowTypes.h", "#pragma once\nstruct Access {};\n")
        self.assertEqual(self.found("dataflow-include"), [])


class DataflowSinkTest(TreeTest):
    def test_a_sink_in_the_class_body(self):
        # Line 17 has a '}' in a comment and line 18 one in a string: neither
        # ends the class, so the members after them are still inside it.
        self.edit("lib/Analysis/Dataflow.h", "  // MEMBERS\n",
                  "  core::DiagnosticSink &sink;\n  struct Pending { ClangDiagnosticSink *clang; };\n")
        self.assertEqual(self.found("dataflow-sink"), [
            ("lib/Analysis/Dataflow.h", 20, "class FunctionDataflow names DiagnosticSink"),
            ("lib/Analysis/Dataflow.h", 21, "class FunctionDataflow names ClangDiagnosticSink"),
        ])

    def test_a_sink_outside_the_class_is_allowed(self):
        self.edit("lib/Analysis/Dataflow.h", "void describe(const FunctionDataflow &dataflow);",
                  "void describe(const FunctionDataflow &dataflow, core::DiagnosticSink &sink);\n"
                  "struct Report { core::DiagnosticSink *sink; };")
        self.write("lib/Analysis/DataflowReport.cpp", "void report(core::DiagnosticSink &sink) {}\n")
        self.assertEqual(self.found("dataflow-sink"), [])

    def test_a_constructor_definition_taking_a_sink(self):
        self.write("lib/Analysis/DataflowSetup.cpp", """
            #include "Dataflow.h"
            FunctionDataflow::FunctionDataflow(core::DiagnosticSink &sink,
                                               bool emitDiags) {}
            FunctionDataflow::~FunctionDataflow() { DiagnosticSink *unused = nullptr; }
            """)
        self.assertEqual(self.found("dataflow-sink"), [
            ("lib/Analysis/DataflowSetup.cpp", 2, "FunctionDataflow::FunctionDataflow takes DiagnosticSink"),
        ])

    def test_structs_that_a_constructor_takes(self):
        self.edit("include/weavec/Analysis/FunctionAnalysis.h", "  // OPTIONS\n",
                  "  core::DiagnosticSink *sink = nullptr;\n")
        self.edit("lib/Analysis/Dataflow.h", "void describe(const FunctionDataflow &dataflow);",
                  "struct DataflowInputs {\n  DiagnosticSink &sink;\n};\n"
                  "struct Unrelated { DiagnosticSink *sink; };")
        self.edit("lib/Analysis/Dataflow.h", "bool emitDiags);",
                  "bool emitDiags,\n                   const DataflowInputs &inputs);")
        self.write("include/weavec/Frontend/PrinterOptions.h", "struct PrinterOptions { DiagnosticSink *s; };\n")
        taken = "which a FunctionDataflow constructor takes, names DiagnosticSink"
        self.assertEqual(self.found("dataflow-sink"), [
            ("include/weavec/Analysis/FunctionAnalysis.h", 5, f"AnalysisOptions, {taken}"),
            ("lib/Analysis/Dataflow.h", 25, f"DataflowInputs, {taken}"),
        ])

    def test_a_missing_class_is_a_violation(self):
        self.write("lib/Analysis/Dataflow.h",
                   "#pragma once\nclass FunctionDataflow;\nfriend class FunctionDataflow;\n")
        self.assertEqual(self.found("dataflow-sink"),
                         [("lib/Analysis/Dataflow.h", None, "cannot find class FunctionDataflow")])
        (self.root / "lib/Analysis/Dataflow.h").unlink()
        self.assertEqual(self.found("dataflow-sink"), [
            ("lib/Analysis/Dataflow.h", None,
             "cannot find class FunctionDataflow (lib/Analysis/Dataflow.h is missing or unreadable)"),
        ])

    def test_class_heads(self):
        bare = hygiene.lex_c(textwrap.dedent("""
            struct [[nodiscard]] alignas(8) A { int x; };
            enum class B { One };
            template <class T> struct C : public Base<T> { T value; };
            struct D *make() { return nullptr; }
            class E;
            class LLVM_LIBRARY_VISIBILITY F final : public A { const char *s = "{"; };
            struct ns::G { };
            """)).bare
        definitions = hygiene.class_definitions(bare)
        self.assertEqual([name for name, _, _ in definitions], ["A", "C", "F", "G"])
        name, opening, closing = definitions[2]
        self.assertEqual(bare[opening:closing + 2], '{ const char *s = " "; };')

    def test_lexing_keeps_offsets_and_lines(self):
        text = 'a = "x{"; // }\nb = \'}\'; /* {\n */ c = R"(})";\n'
        lexed = hygiene.lex_c(text)
        self.assertEqual((len(lexed.code), len(lexed.bare)), (len(text), len(text)))
        self.assertEqual(lexed.bare.count("\n"), text.count("\n"))
        self.assertNotIn("{", lexed.bare)
        self.assertNotIn("}", lexed.bare)
        self.assertEqual([body for _, _, body in lexed.strings], ["x{"])


class LineLimitTest(TreeTest):
    def test_count_lines_is_wc_plus_a_last_line_without_newline(self):
        for data, lines in ((b"", 0), (b"\n", 1), (b"a", 1), (b"a\nb", 2), (b"a\nb\n", 2), (b"\n\n", 2)):
            self.assertEqual(hygiene.count_lines(data), lines, data)

    def test_dataflow_total(self):
        (self.root / "lib/Analysis/DataflowEngine.cpp").write_bytes(b"a\nb\nc")  # 3 lines
        (self.root / "include/weavec/Analysis/DataflowEngine.h").write_bytes(b"x\n")  # 1 line
        self.write("lib/Analysis/NotDataflow.cpp", "one\n")
        self.write("lib/Analysis/Dataflow.txt", "one\n")
        self.write("tools/weavec/DataflowTool.cpp", "one\n")
        total = 27 + 10 + 3 + 1  # Dataflow.h, Dataflow.cpp, DataflowEngine.cpp, DataflowEngine.h
        with mock.patch.object(hygiene, "DATAFLOW_LINE_LIMIT", total):
            result = hygiene.run_checks(self.root)
        self.assertEqual(result.dataflow_lines, {
            "include/weavec/Analysis/DataflowEngine.h": 1, "lib/Analysis/Dataflow.cpp": 10,
            "lib/Analysis/Dataflow.h": 27, "lib/Analysis/DataflowEngine.cpp": 3,
        })
        self.assertEqual(result.count("dataflow-lines"), 0)
        with mock.patch.object(hygiene, "DATAFLOW_LINE_LIMIT", total - 1):
            self.assertEqual(self.found("dataflow-lines"), [
                ("{lib,include}/**/Dataflow*.{h,cpp}", None, "41 lines in 4 files, 1 over the limit of 40"),
            ])

    def test_library_total_counts_every_text_file_under_lib_include_and_tools(self):
        for top in ("lib", "include", "tools"):
            shutil.rmtree(self.root / top)
        for path, data in (("lib/CMakeLists.txt", b"a\nb\n"), ("lib/Analysis/Partial.cpp", b"1\n2\n3"),
                           ("include/weavec/Empty.h", b"\n"), ("tools/weavec/CMakeLists.txt", b"x\ny\n"),
                           ("lib/Analysis/blob.bin", b"\0\1\2\n" * 10), ("docs/pages/long.md", b"line\n" * 100)):
            (self.root / path).parent.mkdir(parents=True, exist_ok=True)
            (self.root / path).write_bytes(data)
        with mock.patch.object(hygiene, "LIBRARY_LINE_LIMIT", 8):
            result = hygiene.run_checks(self.root)
        self.assertEqual(result.library_lines, {"include/weavec/Empty.h": 1, "lib/Analysis/Partial.cpp": 3,
                                                "lib/CMakeLists.txt": 2, "tools/weavec/CMakeLists.txt": 2})
        self.assertEqual(result.binary, ["lib/Analysis/blob.bin"])
        self.assertEqual(result.count("library-lines"), 0)
        with mock.patch.object(hygiene, "LIBRARY_LINE_LIMIT", 7):
            self.assertEqual(self.found("library-lines"), [
                ("{lib,include,tools}/**", None, "8 lines in 4 files, 1 over the limit of 7"),
            ])


class MainTest(TreeTest):
    def test_output_json_and_exit_code(self):
        self.write("docs/pages/reference/cli.md", "# CLI\n--checked\n")
        out = self.root / "reports" / "hygiene.json"
        with mock.patch.object(hygiene, "LIBRARY_LINE_LIMIT", 10):
            code, output = self.run_main("--json", str(out))
        self.assertEqual(code, 1)
        lines = output.splitlines()
        self.assertEqual(lines[0], "docs/pages/reference/cli.md:2: retired-name: '--checked' at column 1")
        self.assertRegex(lines[1], r"^\{lib,include,tools\}/\*\*: library-lines: "
                                   r"\d+ lines in 10 files, \d+ over the limit of 10$")
        self.assertEqual(lines[2:4], ["", "Summary (RFC 0030, gate H2)"])
        self.assertRegex(output, r"\n  files: +12 from a directory walk \(not a git work tree\); "
                                 r"skipped 0 binary, 0 unreadable\n")
        self.assertRegex(output, r"\n  LibrarySpec: +5 entries, 3 chk aliases, and their __builtin_ spellings\n")
        self.assertRegex(output, r"\n  Dataflow lines: 37 / 25,000 \(2 Dataflow\*\.\{h,cpp\} files\)\n")
        self.assertRegex(output, r"\n  library lines: +\d+ / 10 \(10 files under lib/, include/ and tools/\)\n")
        self.assertRegex(output, r"\n  retired-name +1  SafetyState")
        self.assertRegex(output, r"\n  library-name-test +0  ")
        self.assertRegex(output, r"\n  library-lines +1  ")
        self.assertEqual(lines[-1], "H2: FAIL (2 violations)")
        document = json.loads(out.read_text())
        self.assertFalse(document["passed"])
        self.assertEqual(document["files"]["source"], "walk")
        self.assertEqual(document["checks"]["retired-name"]["violations"], 1)
        self.assertEqual(document["violations"][0], {
            "check": "retired-name", "path": "docs/pages/reference/cli.md", "line": 2,
            "message": "'--checked' at column 1"})
        self.assertIsNone(document["violations"][1]["line"])
        self.assertEqual(document["lines"]["dataflow"], {
            "total": 37, "limit": 25000,
            "files": {"lib/Analysis/Dataflow.cpp": 10, "lib/Analysis/Dataflow.h": 27}})
        self.assertEqual(document["lines"]["library"]["limit"], 10)

    def test_usage_errors_exit_2(self):
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(hygiene.main(["--root", str(self.root / "missing")]), 2)
            shutil.rmtree(self.root / "lib")
            self.assertEqual(hygiene.main(["--root", str(self.root)]), 2)
            with self.assertRaises(SystemExit) as raised:
                hygiene.main(["--bogus"])
        self.assertEqual(raised.exception.code, 2)
        self.assertIn("is not a directory", errors.getvalue())
        self.assertIn("has no lib/ directory", errors.getvalue())


@unittest.skipIf(shutil.which("git") is None, "git is not installed")
class GitSelectionTest(TreeTest):
    def git(self, *args):
        subprocess.run(["git", *args], cwd=self.root, check=True, capture_output=True)

    def test_ignored_files_are_not_scanned_and_untracked_ones_are(self):
        self.write(".gitignore", "/docs/node_modules/\n/lib/Generated/\n")
        self.write("lib/Gone.cpp", "SafetyState gone;\n")
        self.git("init", "-q")
        self.git("add", "-A")
        (self.root / "lib/Gone.cpp").unlink()  # still in the index
        self.write("docs/node_modules/pkg/README.md", "--checked\n")
        self.write("lib/Generated/Table.cpp", "SafetyState generated;\n" * 100)
        self.write("lib/Analysis/New.cpp", "bool checkContracts;\n")  # untracked, not ignored
        result = hygiene.run_checks(self.root)
        self.assertEqual(result.file_source, "git")
        self.assertEqual([(v.path, v.line) for v in result.violations], [("lib/Analysis/New.cpp", 1)])
        self.assertIn("lib/Analysis/New.cpp", result.library_lines)
        self.assertNotIn("lib/Generated/Table.cpp", result.library_lines)
        self.assertNotIn("lib/Gone.cpp", result.library_lines)
        self.assertEqual(result.unreadable, [])


if __name__ == "__main__":
    unittest.main()
