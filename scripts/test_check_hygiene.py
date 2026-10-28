#!/usr/bin/env python3
"""Tests for scripts/check-hygiene.py (RFC 0036 gate H1).

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

# The real grammar: comments, continuation lines, directives, chk aliases,
# wrappers.
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
    arg(0):null-on-failure:str chk(__fgets_chk: 0,-1,1,2) wrapper(fgets);
builtins;
__builtin_expect (int, int) -> int;
"""

RUNTIME_HEADER = """\
/* weavec_rt.h: __weavec_rt_documented is only named in a comment. */
#include <stdint.h>
struct __weavec_rt_shadow_t { uintptr_t base; };
extern struct __weavec_rt_shadow_t __weavec_rt_shadow;
void __weavec_rt_guard(uintptr_t address, uint64_t width);
void __weavec_rt_unpoison_stack(void);
char *__weavec_rt_fgets(char *d, int n, void *stream);
"""

GUARD_PASS_CPP = """\
#include "GuardPassImpl.h"
#include "llvm/IR/Module.h"

llvm::FunctionCallee ModuleContext::guard() { return runtime("guard", guardType); }
llvm::Value *ModuleContext::shadow() { return module.getOrInsertGlobal("__weavec_rt_shadow", type); }
llvm::FunctionCallee ModuleContext::runtime(llvm::StringRef name, llvm::FunctionType *type) {
  return module.getOrInsertFunction(("__weavec_rt_" + name).str(), type);
}
"""

GUARD_FRAMES_CPP = """\
#include "GuardPassImpl.h"

void unpoison(ModuleContext &module) {
  llvm::FunctionCallee unpoison = module.runtime(
      "unpoison_stack",
      llvm::FunctionType::get(llvm::Type::getVoidTy(module.context), false));
}
"""

# A tree that passes every check.
CLEAN_TREE = {
    "lib/CMakeLists.txt": "add_subdirectory(Core)\nadd_subdirectory(Frontend)\n",
    "lib/Core/LibrarySpec.txt": LIBRARY_SPEC,
    "lib/Core/LibrarySpec.cpp": ('#include "weavec/Core/LibrarySpec.h"\n#include <string_view>\n'
                                 'bool isFree(std::string_view n) { return n == "free"; }\n'),
    "include/weavec/Core/LibrarySpec.h": "#pragma once\n#include <string>\n",
    "lib/Frontend/GuardPass.cpp": GUARD_PASS_CPP,
    "lib/Frontend/GuardFrames.cpp": GUARD_FRAMES_CPP,
    "include/weavec/Frontend/GuardPass.h": "#pragma once\n#include <llvm/IR/PassManager.h>\n",
    "tools/weavec-cc/main.cpp": "int main() { return 0; }\n",
    "runtime/weavec_rt.h": RUNTIME_HEADER,
    "runtime/weavec_guard.c": '#include "weavec_rt.h"\n#include <string.h>\nvoid f(void) {}\n',
    "runtime/test/rt_test.c": '#include "../weavec_rt.h"\nint main(void) { return 0; }\n',
    "docs/pages/reference/cli.md": "# CLI\n\nweavec-cc [options] files\n",
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
        self.assertEqual(result.entry_points, {"__weavec_rt_fgets": True, "__weavec_rt_guard": True,
                                               "__weavec_rt_shadow": True, "__weavec_rt_unpoison_stack": True})
        code, output = self.run_main()
        self.assertEqual(code, 0)
        self.assertTrue(output.startswith("Summary (RFC 0036, gate H1)\n"), output)
        self.assertEqual(output.splitlines()[-1], "H1: PASS (0 violations)")

    def test_walk_skips_build_and_tool_output(self):
        for skipped in ("node_modules/pkg", "dist", ".generated", ".astro", "test-results",
                        "playwright-report", "__pycache__", "build-release"):
            self.write(f"lib/{skipped}/Notes.cpp", "// cJSON\n")
        self.assertEqual(self.found("corpus-word"), [])

    def test_docs_are_not_scanned(self):
        self.write("docs/pages/corpus.md", 'zlib: name == "free"\n#include <clang/AST/Decl.h>\n')
        self.assertEqual(hygiene.run_checks(self.root).violations, [])


class LibrarySpecNamesTest(unittest.TestCase):
    def test_logical_lines_drop_comments_and_join_continuations(self):
        lines = {first: " ".join(text.split()) for first, text in hygiene.library_spec_lines(LIBRARY_SPEC)}
        self.assertEqual(lines[3], "")  # a comment line
        self.assertEqual(lines[4], "header stdlib.h;")
        self.assertEqual(lines[8], "memcpy (w:bytes(a2), r:bytes(a2), int) -> arg(0) disjoint(0,1,a2) "
                                   "copies(0,1,a2) chk(__builtin___memcpy_chk: 0,1,2,-1) "
                                   "chk(__memcpy_chk: 0,1,2,-1);")
        self.assertEqual(lines[11], "fgets (w:bytes(a1), int, rw) -> arg(0):null-on-failure:str "
                                    "chk(__fgets_chk: 0,-1,1,2) wrapper(fgets);")
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
        for word in ("header", "builtins", "stdlib", "string", "fake", "arg", "chk", "copies", "wrapper"):
            self.assertNotIn(word, names)
        self.assertEqual(len(names), 15)  # 5 entries, 3 aliases, 7 new __builtin_ spellings


class LibraryNameTestTest(TreeTest):
    def test_comparisons_in_either_order_outside_comments(self):
        self.write("lib/Frontend/Calls.cpp", r'''
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
            ("lib/Frontend/Calls.cpp", 1, f'== "free" {entry}'),
            ("lib/Frontend/Calls.cpp", 2, f'"free" == {entry}'),
            ("lib/Frontend/Calls.cpp", 4, f'== "malloc" {entry}'),
            ("lib/Frontend/Calls.cpp", 8, f'== "memcpy" {entry}'),
            ("lib/Frontend/Calls.cpp", 10,
             '== "__memcpy_chk" compares against a LibrarySpec chk alias (of memcpy)'),
            ("lib/Frontend/Calls.cpp", 10,
             '== "__builtin_memcpy" compares against a LibrarySpec __builtin_ spelling (of memcpy)'),
            ("lib/Frontend/Calls.cpp", 11, f'"fgets" == {entry}'),
            ("lib/Frontend/Calls.cpp", 11, f'== "free" {entry}'),
        ])

    def test_only_c_sources_outside_library_spec_count(self):
        self.write("lib/Core/LibrarySpecTable.inc", 'if (name == "free") return;\n')
        self.write("lib/Frontend/notes.md", 'name == "free"\n')
        self.write("include/weavec/Frontend/Names.def", 'CASE(name == "malloc")\n')
        self.write("tools/weavec-cc/main.cc", 'bool x = arg == "fgets";\n')
        self.write("runtime/weavec_libc.c", 'int y = arg == "free";\n')
        self.assertEqual([(path, line) for path, line, _ in self.found("library-name-test")],
                         [("include/weavec/Frontend/Names.def", 1), ("tools/weavec-cc/main.cc", 1)])

    def test_lexing_keeps_literals_comments_and_numbers_apart(self):
        self.write("lib/Frontend/Lexing.cpp", r'''
            auto raw = R"x(a)" == "free")x";
            char quote = '"'; bool a = n == "malloc";
            long big = 1'000; bool b = n == "free";
            bool c = n == "fr" "ee";
            const char *url = "http://example.org"; bool d = n == "fgets";
            /* don't */ bool e = n == "memcpy";
            ''')
        self.assertEqual([(line, message.split()[1]) for _, line, message in self.found("library-name-test")],
                         [(2, '"malloc"'), (3, '"free"'), (5, '"fgets"'), (6, '"memcpy"')])

    def test_lexing_keeps_offsets_and_lines(self):
        text = 'a = "x{"; // }\nb = \'}\'; /* {\n */ c = R"(})";\n'
        lexed = hygiene.lex_c(text)
        self.assertEqual((len(lexed.code), len(lexed.bare)), (len(text), len(text)))
        self.assertEqual(lexed.bare.count("\n"), text.count("\n"))
        self.assertNotIn("{", lexed.bare)
        self.assertNotIn("}", lexed.bare)
        self.assertEqual([body for _, _, body in lexed.strings], ["x{"])

    def test_missing_library_spec_is_a_violation(self):
        (self.root / "lib/Core/LibrarySpec.txt").unlink()
        self.write("lib/Frontend/Calls.cpp", 'bool a = n == "free";\n')
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
        self.write("lib/Frontend/Words.cpp", """
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

    def test_fresh_and_sealed_projects(self):
        # RFC 0033 section 11: the fresh and sealed corpus projects.
        fresh_and_sealed = ("zstd", "libuv", "oniguruma", "redis", "expat", "pcre2", "libevent", "libsodium",
                            "libxml2", "libpng", "mbedtls", "msgpack", "yyjson")
        self.assertTrue(set(fresh_and_sealed) <= set(hygiene.CORPUS_WORDS))
        self.write("lib/Frontend/Fresh.cpp", """
            // ZSTD_compress, redisCommand and expatriate are not words; msgpack_pack is.
            #include <zstd.h> // libuv, oniguruma, pcre2, libevent, libsodium
            // libxml2 libpng mbedtls yyjson redis expat
            """)
        self.assertEqual([(line, message) for _, line, message in self.found("corpus-word")], [
            (1, "word 'msgpack' at column 62"),
            (2, "word 'zstd' at column 11"),
            (2, "word 'libuv' at column 22"),
            (2, "word 'oniguruma' at column 29"),
            (2, "word 'pcre2' at column 40"),
            (2, "word 'libevent' at column 47"),
            (2, "word 'libsodium' at column 57"),
            (3, "word 'libxml2' at column 4"),
            (3, "word 'libpng' at column 12"),
            (3, "word 'mbedtls' at column 19"),
            (3, "word 'yyjson' at column 27"),
            (3, "word 'redis' at column 34"),
            (3, "word 'expat' at column 40"),
        ])

    def test_every_file_under_lib_and_nothing_else(self):
        self.edit("lib/Core/LibrarySpec.txt", "a small table", "a small table (jansson.h is not listed)")
        self.write("lib/Frontend/README.txt", "Measured on zlib.\n")
        self.write("include/weavec/Frontend/Corpus.h", "// lua\n")
        self.write("tools/weavec-cc/main.cpp", "// cJSON\n")
        self.write("runtime/weavec_libc.c", "/* sds */\n")
        self.write("docs/pages/corpus.md", "sds\n")
        self.assertEqual(self.found("corpus-word"), [
            ("lib/Core/LibrarySpec.txt", 1, "word 'jansson' at column 36"),
            ("lib/Frontend/README.txt", 1, "word 'zlib' at column 13"),
        ])


class CoreIncludeTest(TreeTest):
    def test_clang_and_llvm_headers_in_core(self):
        self.write("lib/Core/LibrarySpec.cpp", """
            #include "weavec/Core/LibrarySpec.h"
            #include "llvm/ADT/StringRef.h"
            // #include <clang/AST/Decl.h>
            /* #include <llvm/Support/Error.h> */
            #  include <clang-c/Index.h>
            #include <llvmish/Thing.h>
            #include "Helpers/clang/Thing.h"
            """)
        self.write("include/weavec/Core/Table.h", "#pragma once\n#include <llvm-c/Core.h>\n")
        self.write("lib/Core/Notes.txt", "#include <llvm/ADT/StringRef.h>\n")
        self.write("lib/Frontend/Driver.cpp", '#include "clang/Driver/Driver.h"\n')
        self.assertEqual(self.found("core-include"), [
            ("include/weavec/Core/Table.h", 2, "includes <llvm-c/Core.h>"),
            ("lib/Core/LibrarySpec.cpp", 2, 'includes "llvm/ADT/StringRef.h"'),
            ("lib/Core/LibrarySpec.cpp", 5, "includes <clang-c/Index.h>"),
        ])


class RuntimeIncludeTest(TreeTest):
    def test_only_weavec_rt_h(self):
        self.write("runtime/weavec_alloc.c", """
            #include "weavec_rt.h"
            #include "weavec_alloc.h"
            #include <weavec.h>
            #include <weavec/Core/LibrarySpec.h>
            // #include "other.h"
            #include <sys/mman.h>
            """)
        self.write("runtime/test/rt_test.c", '#include "../weavec_rt.h"\n#include "helpers.h"\n')
        self.write("runtime/CMakeLists.txt", '# #include "notes.h"\n')
        self.assertEqual(self.found("runtime-include"), [
            ("runtime/test/rt_test.c", 2, 'includes "helpers.h", not weavec_rt.h'),
            ("runtime/weavec_alloc.c", 2, 'includes "weavec_alloc.h", not weavec_rt.h'),
            ("runtime/weavec_alloc.c", 3, "includes the WeaveC header <weavec.h>"),
            ("runtime/weavec_alloc.c", 4, "includes the WeaveC header <weavec/Core/LibrarySpec.h>"),
        ])


class RuntimeEntryTest(TreeTest):
    def test_names_the_passes_use_must_be_declared(self):
        self.write("lib/Frontend/GuardLoops.cpp", """
            llvm::FunctionCallee test = module.runtime(
                "range_ok", type);
            // module.runtime("commented", type);
            const char *name = "__weavec_rt_documented";
            const char *prefix = "__weavec_rt_";
            bool runtimes = myruntime("not_a_pass_call");
            """)
        self.edit("lib/Core/LibrarySpec.txt", "free (none:null-ok:release(free)) -> void;",
                  "free (none:null-ok:release(free)) -> void wrapper(free);")
        self.write("lib/Core/Notes.cpp", 'auto f = runtime("core_is_not_a_pass");\n')
        self.assertEqual(self.found("runtime-entry"), [
            # By name.
            ("lib/Frontend/GuardLoops.cpp", 4,
             "__weavec_rt_documented is not declared in runtime/weavec_rt.h"),
            ("lib/Core/LibrarySpec.txt", 5, "__weavec_rt_free is not declared in runtime/weavec_rt.h"),
            ("lib/Frontend/GuardLoops.cpp", 1, "__weavec_rt_range_ok is not declared in runtime/weavec_rt.h"),
        ])
        self.edit("runtime/weavec_rt.h", "void __weavec_rt_unpoison_stack(void);",
                  "void __weavec_rt_unpoison_stack(void);\nint __weavec_rt_range_ok(uint64_t a, uint64_t n);\n"
                  "void __weavec_rt_free(void *p);\nextern int __weavec_rt_documented;")
        self.assertEqual(self.found("runtime-entry"), [])

    def test_a_missing_runtime_header_is_a_violation(self):
        (self.root / "runtime/weavec_rt.h").unlink()
        self.assertEqual(self.found("runtime-entry"), [
            ("runtime/weavec_rt.h", None, "missing or unreadable, so the runtime's entry points are unknown"),
        ])


class LineLimitTest(TreeTest):
    def test_count_lines_is_wc_plus_a_last_line_without_newline(self):
        for data, lines in ((b"", 0), (b"\n", 1), (b"a", 1), (b"a\nb", 2), (b"a\nb\n", 2), (b"\n\n", 2)):
            self.assertEqual(hygiene.count_lines(data), lines, data)

    def test_the_real_limits(self):
        """RFC 0036 gate H1 and RFC 0032 gate H1 (the module docstring)."""
        self.assertEqual((hygiene.LIBRARY_LINE_LIMIT, hygiene.RUNTIME_LINE_LIMIT), (9_000, 6_000))

    def test_runtime_total_leaves_out_its_tests(self):
        """RFC 0032 gate H1: the runtime is counted apart, without runtime/test/."""
        shutil.rmtree(self.root / "runtime")
        for path, data in (("runtime/weavec_alloc.c", b"a\nb\nc\n"), ("runtime/weavec_rt.h", b"h\n"),
                           ("runtime/CMakeLists.txt", b"x\n"), ("runtime/test/rt_test.c", b"t\n" * 50)):
            (self.root / path).parent.mkdir(parents=True, exist_ok=True)
            (self.root / path).write_bytes(data)
        result = hygiene.run_checks(self.root)
        self.assertEqual(result.runtime_lines, {"runtime/CMakeLists.txt": 1, "runtime/weavec_alloc.c": 3,
                                                "runtime/weavec_rt.h": 1})
        self.assertEqual(result.count("runtime-lines"), 0)
        self.assertNotIn("runtime/weavec_alloc.c", result.library_lines)
        with mock.patch.object(hygiene, "RUNTIME_LINE_LIMIT", 4):
            self.assertEqual(self.found("runtime-lines"), [
                ("runtime/**", None, "5 lines in 3 files, 1 over the limit of 4"),
            ])

    def test_library_total_counts_every_text_file_under_lib_include_and_tools(self):
        for top in ("lib", "include", "tools"):
            shutil.rmtree(self.root / top)
        for path, data in (("lib/CMakeLists.txt", b"a\nb\n"), ("lib/Frontend/Partial.cpp", b"1\n2\n3"),
                           ("include/weavec/Empty.h", b"\n"), ("tools/weavec-cc/CMakeLists.txt", b"x\ny\n"),
                           ("lib/Frontend/blob.bin", b"\0\1\2\n" * 10), ("docs/pages/long.md", b"line\n" * 100)):
            (self.root / path).parent.mkdir(parents=True, exist_ok=True)
            (self.root / path).write_bytes(data)
        with mock.patch.object(hygiene, "LIBRARY_LINE_LIMIT", 8):
            result = hygiene.run_checks(self.root)
        self.assertEqual(result.library_lines, {"include/weavec/Empty.h": 1, "lib/CMakeLists.txt": 2,
                                                "lib/Frontend/Partial.cpp": 3, "tools/weavec-cc/CMakeLists.txt": 2})
        self.assertEqual(result.binary, ["lib/Frontend/blob.bin"])
        self.assertEqual(result.count("library-lines"), 0)
        with mock.patch.object(hygiene, "LIBRARY_LINE_LIMIT", 7):
            self.assertEqual(self.found("library-lines"), [
                ("{lib,include,tools}/**", None, "8 lines in 4 files, 1 over the limit of 7"),
            ])

    def test_library_total_excludes_the_declarative_table(self):
        """The table is data, and its growth is coverage."""
        result = hygiene.run_checks(self.root)
        self.assertNotIn(hygiene.LIBRARY_SPEC, result.library_lines)
        self.assertIn("lib/Core/LibrarySpec.cpp", result.library_lines)
        table = (self.root / hygiene.LIBRARY_SPEC)
        table.write_text(table.read_text() + "header wchar.h;\n" * 5_000)
        with mock.patch.object(hygiene, "LIBRARY_LINE_LIMIT", 100):
            self.assertEqual(self.found("library-lines"), [])


class MainTest(TreeTest):
    def test_output_json_and_exit_code(self):
        self.write("lib/Frontend/Notes.txt", "Corpus notes:\nzlib\n")
        out = self.root / "reports" / "hygiene.json"
        with mock.patch.object(hygiene, "LIBRARY_LINE_LIMIT", 10):
            code, output = self.run_main("--json", str(out))
        self.assertEqual(code, 1)
        lines = output.splitlines()
        self.assertEqual(lines[0], "lib/Frontend/Notes.txt:2: corpus-word: word 'zlib' at column 1")
        self.assertRegex(lines[1], r"^\{lib,include,tools\}/\*\*: library-lines: "
                                   r"\d+ lines in 8 files, \d+ over the limit of 10$")
        self.assertEqual(lines[2:4], ["", "Summary (RFC 0036, gate H1)"])
        self.assertRegex(output, r"\n  files: +12 from a directory walk \(not a git work tree\); "
                                 r"skipped 0 binary, 0 unreadable\n")
        self.assertRegex(output, r"\n  LibrarySpec: +5 entries, 3 chk aliases, and their __builtin_ spellings\n")
        self.assertRegex(output, r"\n  entry points: +4 of 4 declared in runtime/weavec_rt\.h\n")
        self.assertRegex(output, r"\n  library lines: +\d+ / 10 \(8 code files under lib/, include/ and "
                                 r"tools/, lib/Core/LibrarySpec\.txt excluded\)\n")
        self.assertRegex(output, r"\n  runtime lines: +\d+ / 6,000 \(2 files under runtime/, runtime/test/ "
                                 r"excluded\)\n")
        for check, count in (("library-name-test", 0), ("corpus-word", 1), ("core-include", 0),
                             ("runtime-include", 0), ("runtime-entry", 0), ("library-lines", 1),
                             ("runtime-lines", 0)):
            self.assertRegex(output, rf"\n  {check} +{count}  ")
        self.assertEqual(lines[-1], "H1: FAIL (2 violations)")
        document = json.loads(out.read_text())
        self.assertFalse(document["passed"])
        self.assertEqual(document["version"], 3)
        self.assertEqual(document["files"]["source"], "walk")
        self.assertEqual(document["checks"]["corpus-word"]["violations"], 1)
        self.assertEqual(document["violations"][0], {
            "check": "corpus-word", "path": "lib/Frontend/Notes.txt", "line": 2,
            "message": "word 'zlib' at column 1"})
        self.assertIsNone(document["violations"][1]["line"])
        self.assertEqual(document["entryPoints"]["__weavec_rt_guard"], True)
        self.assertEqual(document["lines"]["library"]["limit"], 10)
        self.assertEqual(document["lines"]["library"]["files"], 8)
        self.assertEqual(sorted(document["lines"]["runtime"]["files"]),
                         ["runtime/weavec_guard.c", "runtime/weavec_rt.h"])

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
        self.write(".gitignore", "/lib/Generated/\n")
        self.write("lib/Gone.cpp", "// zlib\n")
        self.git("init", "-q")
        self.git("add", "-A")
        (self.root / "lib/Gone.cpp").unlink()  # still in the index
        self.write("lib/Generated/Table.cpp", "// zlib\n" * 100)
        self.write("lib/Frontend/New.cpp", "// cJSON\n")  # untracked, not ignored
        result = hygiene.run_checks(self.root)
        self.assertEqual(result.file_source, "git")
        self.assertEqual([(v.path, v.line) for v in result.violations], [("lib/Frontend/New.cpp", 1)])
        self.assertIn("lib/Frontend/New.cpp", result.library_lines)
        self.assertNotIn("lib/Generated/Table.cpp", result.library_lines)
        self.assertNotIn("lib/Gone.cpp", result.library_lines)
        self.assertEqual(result.unreadable, [])


if __name__ == "__main__":
    unittest.main()
