#!/usr/bin/env python3
"""Tests for scripts/corpus-gate.py (RFC 0035, section 11).

Synthetic data only: fake tools written by the tests stand in for weavec,
weavec-cc and the reference compiler (the "programs" they build are shell
scripts), and a local git repository stands in for a corpus project, so the
tests need neither the network, a compiler nor a WeaveC build. The checks
that read the repository's own test/corpus files need no build either; the
one that applies the injection patches to the corpus checkouts skips when
there are none.
"""

from __future__ import annotations

import contextlib
import difflib
import fnmatch
import importlib.util
import io
import json
import os
import shlex
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SPEC = importlib.util.spec_from_file_location("corpus_gate", HERE / "corpus-gate.py")
gate = importlib.util.module_from_spec(SPEC)
sys.modules["corpus_gate"] = gate
SPEC.loader.exec_module(gate)

CORPUS = ROOT / "test" / "corpus"


def proc(returncode=0, stdout="", stderr="", timed_out=False, error=""):
    return gate.ProcResult("cmd", returncode, stdout, stderr, 0.1, timed_out=timed_out, error=error)


def diag(file="a.c", line=1, id_="use-after-free", severity="error", message="use of 'p' after it was freed"):
    return gate.Diagnostic(file=file, line=line, col=1, severity=severity, id=id_, message=message)


def ledger_document(*summaries, version=3, rows=()):
    return {"schema": "weavec-ledger", "version": version, "producer": {"name": "weavec-cc"},
            "units": [{"source": "a.c", "object": "a.o", "summary": s, "rows": list(rows)} for s in summaries]}


def config(name="c", corpus_set="original", build=("make",), bench=False):
    project = gate.Project(name="p", url="u", sha="0" * 40, support=[])
    return gate.Config(name=name, project=project, files=["a.c"], args=[], build=list(build), test=["./t"],
                       bench=gate.Bench(name=name, build=["b"], command="c", repeat=1) if bench else None,
                       held_out=corpus_set != "original", set=corpus_set)


def report(kind="heap-use-after-free", file="a.c", line=3, col=5, proven=False):
    return {"kind": kind, "file": file, "line": line, "col": col, "proven": proven}


# -- parsing ------------------------------------------------------------------------


class ParsingTest(unittest.TestCase):
    def test_diagnostics_are_relative_to_the_checkout(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            text = "\n".join([
                f"{root}/src/a.c:3:5: error: 'p' is freed twice [weavec::double-free]",
                "src/b.c:7:1: warning: 'q' is leaked [weavec::leak]",
                f"{root}/src/a.c:9:1: error: unknown type name 'foo'",
                "    3 |   free(p);",
                "In file included from x.h:1:",
            ])
            diagnostics, clang_errors = gate.parse_diagnostics(text, root)
        self.assertEqual([(d.file, d.line, d.id, d.severity) for d in diagnostics],
                         [("src/a.c", 3, "double-free", "error"), ("src/b.c", 7, "leak", "warning")])
        self.assertEqual(clang_errors, 1)
        self.assertEqual(diagnostics[0].render(), "src/a.c:3:5: error: 'p' is freed twice [weavec::double-free]")

    def test_summary_lines(self):
        text = "\n".join([
            "weavec: cJSON.c: 1,221 sites: 858 proven, 361 not proven, 0 violations, 2 trusted; 0 errors, 4 warnings",
            "weavec: b.c: 1 site: 0 proven, 0 not proven, 1 violation, 0 trusted; 1 error, 1 warning; "
            "2 functions over budget (f, g)",
            "weavec: program program: 1,745 sites in 2 units: 1,109 proven, 634 not proven, 0 violations, "
            "2 trusted; 0 errors, 10 warnings",
            "weavec: a.c: 812 accesses: 431 proven, 381 guarded, 0 unguarded",
        ])
        units, program = gate.parse_summaries(text)
        self.assertEqual([u["source"] for u in units], ["cJSON.c", "b.c"])
        self.assertEqual(units[0]["sites"], 1221)
        self.assertEqual((units[1]["violations"], units[1]["errors"], units[1]["overBudget"]), (1, 1, 2))
        self.assertEqual((program["sites"], program["proven"], program["warnings"]), (1745, 1109, 10))

    def test_failures_never_look_clean(self):
        ok = gate.classify_failure
        self.assertEqual(ok(proc(), [], 0, 1), "")
        self.assertEqual(ok(proc(1), [diag()], 0, 1), "")  # a definite error is a result
        self.assertIn("without a WeaveC error", ok(proc(1), [], 0, 1))
        self.assertIn("SIGSEGV", ok(proc(-11), [], 0, 1))
        self.assertIn("Clang error", ok(proc(1), [], 2, 1))
        self.assertIn("timeout", ok(proc(timed_out=True), [], 0, 1))
        self.assertEqual(ok(proc(error="No such file"), [], 0, 1), "No such file")
        self.assertIn("no summary line", ok(proc(), [], 0, 0))

    def test_reports(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            text = "\n".join([
                f"weavec: heap-buffer-overflow at {root}/src/x.c:10:3: read of 4 bytes at 0x10",
                "weavec: 0x10 is 0 bytes after the 4-byte heap object at 0xc",
                "12: weavec: heap-buffer-overflow at ../src/x.c:10:3: read of 4 bytes at 0x10",
                "weavec: weavec.proven: stack-buffer-overflow at ./y.c:2:1: write of 8 bytes at 0x20",
                "weavec: null-dereference at <unknown>: access at 0x0",
                "weavec: invalid release of 0x1234: the block was already released",
                "weavec: runtime: 12 allocations",
                "weavec: frobnicated at z.c:1:1: nothing",
            ])
            reports = gate.parse_reports(text, root)
        self.assertEqual([gate.describe_report(r) for r in reports], [
            "heap-buffer-overflow at src/x.c:10:3",
            "weavec.proven: stack-buffer-overflow at y.c:2:1",
            "null-dereference at no location",
            "invalid-release at no location",
        ])

    def test_trap_and_fault_evidence(self):
        self.assertTrue(gate.trap_evidence(proc(-5)))
        self.assertTrue(gate.trap_evidence(proc(133)))
        self.assertTrue(gate.trap_evidence(proc(1, stderr="sh: line 1: 42 Illegal instruction ./example")))
        self.assertTrue(gate.trap_evidence(proc(2, stdout="make: *** [test] Error 133")))
        self.assertTrue(gate.trap_evidence(proc(8, stdout="  7/22 Test  #7: parse ***Exception: Illegal")))
        self.assertFalse(gate.trap_evidence(proc(1, stdout="1 test failed")))
        self.assertFalse(gate.trap_evidence(proc(-11)))
        self.assertTrue(gate.fault_evidence(proc(-11)))
        self.assertTrue(gate.fault_evidence(proc(139)))
        self.assertTrue(gate.fault_evidence(proc(1, stderr="sh: line 1: 7 Segmentation fault ./x")))
        self.assertFalse(gate.fault_evidence(proc(-5)))

    def test_paths(self):
        self.assertEqual(gate.normalise_path("../../src/./x.c"), "src/x.c")
        self.assertTrue(gate.same_file("x.c", "src/x.c"))
        self.assertTrue(gate.same_file("/tmp/w/src/x.c", "src/x.c"))
        self.assertFalse(gate.same_file("src/y.c", "src/x.c"))
        self.assertFalse(gate.same_file("xx.c", "x.c"))

    def test_geometric_mean(self):
        self.assertEqual(gate.geometric_mean([1.0, 4.0]), 2.0)
        self.assertIsNone(gate.geometric_mean([]))
        self.assertIsNone(gate.geometric_mean([1.0, 0.0]))


class ProcessTest(unittest.TestCase):
    def test_timeout_kills_the_session(self):
        start = time.perf_counter()
        result = gate.run_process(["/bin/sh", "-c", "sleep 30 & sleep 30"], cwd=".", timeout=0.5)
        self.assertTrue(result.timed_out)
        self.assertLess(time.perf_counter() - start, 10)

    def test_cpu_time_includes_descendants(self):
        burn = f"{sys.executable} -c 'import time\nt=time.process_time()\nwhile time.process_time()-t<0.3: pass'"
        result = gate.run_shell(f"{burn} && {burn}", cwd=Path("."), env=dict(os.environ), timeout=60)
        self.assertEqual(result.returncode, 0, result.output)
        self.assertGreater(result.cpu, 0.5)

    def test_signals_are_negative_status(self):
        result = gate.run_process(["/bin/sh", "-c", "kill -TRAP $$"], cwd=".", timeout=10)
        self.assertEqual(result.signal, 5)
        self.assertIn("SIGTRAP", gate.describe_status(result))


# -- the enforcement ledger and --quick's aggregation -------------------------------------


class LedgerTest(unittest.TestCase):
    def write(self, directory, data):
        path = Path(directory) / "x.ledger.json"
        path.write_text(json.dumps(data))
        return path

    def test_units_are_summed(self):
        rows = [{"outcome": "guarded", "reason": "access"}, {"outcome": "proven", "reason": "in-bounds"},
                {"outcome": "guarded", "reason": "access"}]
        with tempfile.TemporaryDirectory() as directory:
            totals, reasons = gate.read_ledger(self.write(directory, ledger_document(
                {"accesses": 3, "proven": 1, "guarded": 2, "unguarded": 0},
                {"accesses": 4, "proven": 4, "guarded": 0, "unguarded": 0}, rows=rows)))
        self.assertEqual(totals, {"accesses": 7, "proven": 5, "guarded": 2, "unguarded": 0})
        self.assertEqual(reasons["guarded:access"], 4)

    def test_invalid_ledgers(self):
        with tempfile.TemporaryDirectory() as directory:
            for data, message in ((ledger_document({"accesses": 1}, version=2), "version 2"),
                                  ({"schema": "other"}, "not a weavec-ledger"),
                                  (ledger_document(), "no units"),
                                  ({"schema": "weavec-ledger", "version": 3, "units": [{}]}, "no summary")):
                with self.assertRaisesRegex(ValueError, message):
                    gate.read_ledger(self.write(directory, data))
            with self.assertRaisesRegex(ValueError, "x.ledger.json"):
                gate.read_ledger(Path(directory) / "missing" / "x.ledger.json")

    def test_quick_measurement(self):
        quick = gate.Quick(config="c")
        quick.add_compile(gate.CompileRun("a.c", {"accesses": 8, "proven": 2, "guarded": 6, "unguarded": 0},
                                          gate.collections.Counter({"proven:in-bounds": 2}), 0.5, 2 ** 20, 0.4))
        quick.add_compile(gate.CompileRun("b.c", {}, gate.collections.Counter(), 0.1, None, failure="exit status 1"))
        quick.add_analysis(gate.AnalysisRun(["a.c"], {"sites": 5, "errors": 1, "warnings": 2}, [diag()], 0.2))
        quick.add_analysis(gate.AnalysisRun(["b.c"], {"sites": 9}, [], 0.2, failure="timeout"))
        measured = quick.measured()
        self.assertEqual(measured["ledger"], {"accesses": 8, "proven": 2, "guarded": 6, "unguarded": 0,
                                              "provenShare": 0.25})
        self.assertEqual((measured["analysis"]["sites"], measured["analysis"]["errors"]), (5, 1))
        self.assertEqual(len(quick.failures), 2)
        self.assertEqual(quick.units["a.c"]["referenceCpuSeconds"], 0.4)
        self.assertEqual(len(quick.to_json()["diagnostics"]), 1)
        self.assertIsNone(gate.Quick(config="empty").proven_share)


# -- the manifest ----------------------------------------------------------------------


def manifest_data(*configs, gates=None):
    return {"schema": "weavec-corpus-manifest", "version": 2, "gates": gates or {},
            "projects": [{"name": "proj", "url": "u", "sha": "a" * 40, "support": [], "configs": list(configs)}]}


class ManifestTest(unittest.TestCase):
    def load(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            path.write_text(json.dumps(data))
            return gate.load_manifest(path, Path(directory))

    def test_repository_manifest(self):
        manifest = gate.load_manifest(CORPUS / "manifest.json", CORPUS / "support")
        names = {c.name for c in manifest.configs}
        for name in ("sds", "cJSON", "cJSON-program", "jsmn", "log.c", "printf", "linenoise",
                     "linenoise-program", "zlib", "lua", "jansson"):
            self.assertIn(name, names)
            self.assertEqual(manifest.config(name).set, "original")
        self.assertTrue(set(c.set for c in manifest.configs) <= set(gate.SETS))
        default = gate.select_configs(manifest, [], held_out=False)
        self.assertEqual({c.set for c in default}, {"original"})
        full = gate.select_configs(manifest, [], held_out=True)
        self.assertFalse(any(c.sealed for c in full))
        self.assertTrue(any(c.held_out for c in full))
        for key in ("buildTime", "runTime"):
            self.assertIn(key, manifest.gates)
        self.assertIn("lua", manifest.gates["runTime"]["maxOverheadPerConfig"])
        for name in manifest.gates["runTime"]["maxOverheadPerConfig"]:
            self.assertIsNotNone(manifest.config(name).bench, name)

    def test_selection(self):
        manifest = self.load(manifest_data(
            {"name": "o", "compile": {"files": ["a.c"]}},
            {"name": "h", "heldOut": True, "compile": {"files": ["a.c"]}},
            {"name": "f", "heldOut": True, "set": "fresh35", "compile": {"files": ["a.c"]}},
            {"name": "s", "heldOut": True, "set": "sealed35", "compile": {"files": ["a.c"]}}))
        names = lambda configs: [c.name for c in configs]  # noqa: E731
        self.assertEqual(names(gate.select_configs(manifest, [])), ["o"])
        self.assertEqual(names(gate.select_configs(manifest, [], held_out=True)), ["o", "h", "f"])
        self.assertEqual(names(gate.select_configs(manifest, [], sets=["sealed35"])), ["s"])
        self.assertEqual(names(gate.select_configs(manifest, [], sets=["fresh35", "heldOut"])), ["h", "f"])
        self.assertEqual(names(gate.select_configs(manifest, ["s", "o"], sets=["fresh35"])), ["o", "s"])
        with self.assertRaisesRegex(gate.GateError, "unknown config"):
            gate.select_configs(manifest, ["nope"])

    def test_held_out_default(self):
        args = gate.parse_args(["--quick"])
        self.assertFalse(gate.with_held_out(args))
        self.assertTrue(gate.with_held_out(gate.parse_args(["--full"])))
        self.assertFalse(gate.with_held_out(gate.parse_args(["--full", "--no-held-out"])))
        self.assertTrue(gate.with_held_out(gate.parse_args(["--quick", "--held-out"])))

    def test_invalid_manifests(self):
        cases = [
            ({"name": "a", "compile": {"files": ["a.c"]}, "lowered": []}, "unknown field"),
            ({"name": "a", "compile": {"files": ["a.c"]}, "link": {"args": []}}, "unknown field"),
            ({"name": "a", "compile": {}}, "compile.files is missing"),
            ({"name": "a", "compile": {"files": ["a.c"]}, "set": "fresh35"}, "needs heldOut true"),
            ({"name": "a", "compile": {"files": ["a.c"]}, "heldOut": True, "set": "fresh36"}, "set must be one of"),
            ({"name": "a", "compile": {"files": ["a.c"]}, "test": ["t"]}, "test needs build"),
            ({"name": "a", "compile": {"files": ["a.c"]}, "build": ["b"], "testTimeout": 0}, "testTimeout"),
            ({"name": "a", "compile": {"files": ["a.c"]}, "bench": {"name": "x", "build": ["b"]}},
             "bench needs build and command"),
        ]
        for c, message in cases:
            with self.assertRaisesRegex(gate.GateError, message):
                self.load(manifest_data(c))
        with self.assertRaisesRegex(gate.GateError, "version 2"):
            self.load({**manifest_data({"name": "a", "compile": {"files": ["a.c"]}}), "version": 1})
        bad_sha = manifest_data({"name": "a", "compile": {"files": ["a.c"]}})
        bad_sha["projects"][0]["sha"] = "HEAD"
        with self.assertRaisesRegex(gate.GateError, "40 lowercase hex"):
            self.load(bad_sha)

    def test_arguments(self):
        for argv, message in ((["--weavec", "x"], "choose a mode"),
                              (["--quick", "--reference-only"], "no --quick"),
                              (["--full", "--reference-only", "--update"], "not recorded"),
                              (["--bench", "--update"], "--update records"),
                              (["--quick", "--jobs", "0"], "at least 1"),
                              (["--quick", "--legacy"], "unrecognized"),
                              (["--quick", "--set", "nope"], "invalid choice")):
            with contextlib.redirect_stderr(io.StringIO()) as err, self.assertRaises(SystemExit):
                gate.parse_args(argv)
            self.assertIn(message, err.getvalue(), argv)
        args = gate.parse_args(["--quick", "--only", "a", "b", "--only", "c"])
        self.assertEqual(args.only, ["a", "b", "c"])


# -- the ratchet -----------------------------------------------------------------------


def measured(proven=50, accesses=100, unguarded=0, errors=0, warnings=2, sites=40):
    return {"ledger": {"accesses": accesses, "proven": proven, "guarded": accesses - proven - unguarded,
                       "unguarded": unguarded, "provenShare": gate.share(proven, accesses)},
            "analysis": {"sites": sites, "proven": 30, "notProven": 10, "violations": 0, "trusted": 0,
                         "errors": errors, "warnings": warnings, "overBudget": 0}}


class RatchetTest(unittest.TestCase):
    platform = "darwin-arm64"

    def expected(self, **configs):
        return gate.merge_expected({}, configs, self.platform, "machine", "weavec-cc version test")

    def test_equal_passes(self):
        result = gate.compare_ratchet({"a": measured()}, self.expected(a=measured()), self.platform)
        self.assertFalse(result.failed)
        self.assertEqual(result.to_json(), {"regressions": [], "improvements": [], "changes": [], "missing": []})

    def test_worse_fails(self):
        expected = self.expected(a=measured())
        for now, field in ((measured(proven=49), "provenShare"), (measured(unguarded=1), "unguarded"),
                           (measured(errors=1), "errors"), (measured(warnings=3), "warnings")):
            result = gate.compare_ratchet({"a": now}, expected, self.platform)
            self.assertTrue(result.failed, field)
            self.assertIn(f"a.{'analysis' if field in ('errors', 'warnings') else 'ledger'}.{field}",
                          result.regressions[0])

    def test_better_and_changed_counts_are_notes(self):
        expected = self.expected(a=measured())
        result = gate.compare_ratchet({"a": measured(proven=60, warnings=1)}, expected, self.platform)
        self.assertFalse(result.failed)
        self.assertEqual(len(result.improvements), 2)
        self.assertIn("a.ledger.proven: 50 -> 60", result.changes)
        result = gate.compare_ratchet({"a": measured(proven=100, accesses=200)}, expected, self.platform)
        self.assertFalse(result.failed)  # the same share of more accesses
        self.assertIn("a.ledger.accesses: 100 -> 200", result.changes)

    def test_missing_records_are_notes(self):
        result = gate.compare_ratchet({"b": measured()}, self.expected(a=measured()), self.platform)
        self.assertFalse(result.failed)
        self.assertIn("b: not recorded", result.missing[0])
        result = gate.compare_ratchet({"a": measured()}, self.expected(a=measured()), "linux-x86_64")
        self.assertIn("nothing recorded for linux-x86_64", result.missing[0])

    def test_merge_keeps_other_platforms_and_configs(self):
        expected = self.expected(a=measured(), b=measured())
        merged = gate.merge_expected(expected, {"a": measured(proven=70)}, "linux-x86_64", "m2", "p2")
        merged = gate.merge_expected(merged, {"a": measured(proven=60)}, self.platform, "m", "p")
        self.assertEqual(sorted(merged["platforms"]), ["darwin-arm64", "linux-x86_64"])
        darwin = merged["platforms"][self.platform]
        self.assertEqual(darwin["configs"]["a"]["ledger"]["proven"], 60)
        self.assertEqual(darwin["configs"]["b"]["ledger"]["proven"], 50)
        self.assertEqual((merged["schema"], merged["version"]), (gate.EXPECTED_SCHEMA, 2))

    def test_old_expected_files(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "expected.json"
            self.assertEqual(gate.load_expected(path, update=False), {})
            path.write_text(json.dumps({"schema": gate.EXPECTED_SCHEMA, "version": 1, "legacy": {}}))
            with self.assertRaisesRegex(gate.GateError, "version 2"):
                gate.load_expected(path, update=False)
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(gate.load_expected(path, update=True), {})

    def test_repository_expected(self):
        expected = gate.load_expected(CORPUS / "expected.json", update=False)
        manifest = gate.load_manifest(CORPUS / "manifest.json", CORPUS / "support")
        names = {c.name for c in manifest.configs}
        for platform, section in expected["platforms"].items():
            for name, record in section["configs"].items():
                self.assertIn(name, names, f"{platform}: {name}")
                self.assertEqual(set(record), {"ledger", "analysis"})
                self.assertEqual(set(record["ledger"]), set(gate.LEDGER_COUNTS) | {"provenShare"})
                self.assertEqual(set(record["analysis"]), set(gate.ANALYSIS_COUNTS))


# -- triage ----------------------------------------------------------------------------


def entry(config="c", file="a.c", line=1, id_="use-after-free", certainty="definite", verdict="true"):
    return {"config": config, "id": id_, "certainty": certainty, "file": file, "line": line,
            "verdict": verdict, "note": "n"}


class TriageTest(unittest.TestCase):
    def test_findings(self):
        findings = gate.findings_of("c", [diag(), diag(), diag(line=2, severity="warning", id_="leak")])
        self.assertEqual([(f["certainty"], f["line"]) for f in findings], [("definite", 1), ("possible", 2)])

    def test_definite_errors_need_a_true_verdict(self):
        findings = gate.findings_of("c", [diag(line=1), diag(line=2), diag(line=3), diag(line=4, severity="warning")])
        result = gate.check_triage(findings, [entry(line=1), entry(line=2, verdict="false"), entry(line=9),
                                              entry(config="other", line=9)], {"c"})
        self.assertEqual([f["line"] for f in result.untriaged], [3])
        self.assertEqual([f["line"] for f in result.false_errors], [2])
        self.assertEqual((len(result.definite), result.possible), (3, 1))
        self.assertEqual([e["line"] for e in result.stale], [9])  # the other config did not run
        self.assertTrue(result.failed)
        ok = gate.check_triage(gate.findings_of("c", [diag(file="./a.c")]), [entry()], {"c"})
        self.assertFalse(ok.failed)

    def test_load_triage(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "triage.json"
            self.assertEqual(gate.load_triage(path).entries, [])

            def write(entries=(), failures=(), version=2):
                path.write_text(json.dumps({"schema": gate.TRIAGE_SCHEMA, "version": version,
                                            "entries": list(entries), "guardFailures": list(failures)}))

            write([entry()], [{"config": "c", "file": "a.c", "line": 1, "verdict": "true", "note": "n",
                               "kind": "heap-buffer-overflow"}])
            self.assertEqual(len(gate.load_triage(path).guard_failures), 1)
            for entries, failures, message in (
                    ([{**entry(), "verdict": "maybe"}], [], "verdict must be"),
                    ([{**entry(), "certainty": "likely"}], [], "certainty must be"),
                    ([{k: v for k, v in entry().items() if k != "note"}], [], "missing note"),
                    ([], [{"config": "c", "file": "a.c", "line": 1, "verdict": "false", "note": "n"}],
                     "must be \"true\""),
                    ([], [{"config": "c", "file": "a.c", "line": 1, "verdict": "true", "note": "n", "kind": "object"}],
                     "unknown kind")):
                write(entries, failures)
                with self.assertRaisesRegex(gate.GateError, message):
                    gate.load_triage(path)
            write(version=1)
            with self.assertRaisesRegex(gate.GateError, "version 2"):
                gate.load_triage(path)

    def test_triaged_guard_failures(self):
        failures = [{"config": "c", "file": "src/lookup3.h", "line": 259, "verdict": "true", "note": "n"},
                    {"config": "c", "file": "x.c", "line": 3, "verdict": "true", "note": "n",
                     "kind": "heap-buffer-overflow"}]
        self.assertTrue(gate.triaged_failure(failures, "c", report(file="../src/lookup3.h", line=259)))
        self.assertFalse(gate.triaged_failure(failures, "d", report(file="src/lookup3.h", line=259)))
        self.assertFalse(gate.triaged_failure(failures, "c", report(file="src/lookup3.h", line=260)))
        self.assertTrue(gate.triaged_failure(failures, "c", report(kind="heap-buffer-overflow", file="x.c")))
        self.assertFalse(gate.triaged_failure(failures, "c", report(kind="heap-use-after-free", file="x.c")))
        self.assertFalse(gate.triaged_failure(failures, "c", report(kind="invalid-release", file=None, line=0)))

    def test_repository_triage(self):
        triage = gate.load_triage(CORPUS / "triage.json")
        manifest = gate.load_manifest(CORPUS / "manifest.json", CORPUS / "support")
        names = {c.name for c in manifest.configs}
        keys = set()
        for e in triage.entries + triage.guard_failures:
            self.assertIn(e["config"], names)
        for e in triage.entries:
            key = (e["config"], e["file"], e["line"], e["id"])
            self.assertNotIn(key, keys, key)
            keys.add(key)
            self.assertTrue(any(fnmatch.fnmatch(e["file"], p) for p in manifest.config(e["config"]).files),
                            f"{key}: not a file the config analyses")


# -- injections ------------------------------------------------------------------------


def injection(**overrides):
    item = {"id": "i", "config": "c", "patch": "p.patch", "file": "a.c", "line": 3}
    item.update(overrides)
    return item


class InjectionTest(unittest.TestCase):
    def load(self, items):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "p.patch").write_text("+ /* INJECTED */\n")
            manifest_path = root / "manifest.json"
            manifest_path.write_text(json.dumps(manifest_data(
                {"name": "c", "compile": {"files": ["a.c"]}, "build": ["b"], "test": ["./t"]},
                {"name": "notest", "compile": {"files": ["a.c"]}})))
            manifest = gate.load_manifest(manifest_path, root)
            path = root / "injections.json"
            path.write_text(json.dumps({"schema": gate.INJECTIONS_SCHEMA, "version": 2, "injections": items}))
            return gate.load_injections(path, manifest)

    def test_fields(self):
        loaded = self.load([injection(), injection(id="j", run="./x", build=["make x"], kinds=["heap-use-after-free"],
                                                   unlocated="invalid-release", stop={"file": "b.c", "line": 7})])
        self.assertEqual(loaded[0].commands(gate.Config("c", None, [], [], test=["./t"])), ["./t"])
        self.assertEqual((loaded[0].stop_file, loaded[0].stop_line), ("a.c", 3))
        j = loaded[1]
        self.assertEqual((j.run, j.build, j.kinds, j.unlocated), (["./x"], ["make x"], ("heap-use-after-free",),
                                                                  "invalid-release"))
        self.assertEqual((j.stop_file, j.stop_line), ("b.c", 7))

    def test_invalid(self):
        for items, message in (([injection(), injection()], "duplicate id"),
                               ([injection(expect={"ids": ["x"]})], "unknown field"),
                               ([injection(mode="unit")], "unknown field"),
                               ([injection(config="nope")], "unknown config"),
                               ([injection(patch="missing.patch")], "not found"),
                               ([injection(kinds=["object"])], "unknown kind"),
                               ([injection(unlocated="anywhere")], "unlocated must be"),
                               ([injection(config="notest")], "no run commands"),
                               ([injection(run=3)], "commands must be"),
                               ([{"id": "x"}], "'config'")):
            with self.assertRaisesRegex(gate.GateError, message):
                self.load(items)

    def stops(self, build_reports=(), faults=(), **overrides):
        inj = gate.Injection(**{"id": "i", "config": "c", "patch": "p", "file": "src/a.c", "line": 3, **overrides})
        build = gate.BuildRun(config="c", mode="trap", compiler="cc", reports=list(build_reports),
                              fault_deaths=list(faults))
        return gate.stops_at(inj, build)

    def test_stops(self):
        self.assertEqual(self.stops([report(file="a.c", line=3)]), ["heap-use-after-free at a.c:3:5"])
        self.assertEqual(self.stops([report(file="src/a.c", line=4)]), [])
        self.assertEqual(self.stops([report(file="src/a.c")], kinds=("heap-buffer-overflow",)), [])
        self.assertTrue(self.stops([report(file="src/b.c", line=9)], stop=("src/b.c", 9)))
        self.assertFalse(self.stops([report(file="src/a.c")], stop=("src/b.c", 9)))
        unlocated = report(kind="invalid-release", file=None, line=0)
        self.assertEqual(self.stops([unlocated]), [])
        self.assertTrue(self.stops([unlocated], unlocated="invalid-release"))
        self.assertEqual(self.stops(faults=["./x: killed by SIGSEGV"]), [])
        self.assertTrue(self.stops(faults=["./x: killed by SIGSEGV"], unlocated="fault"))

    def test_repository_injections(self):
        manifest = gate.load_manifest(CORPUS / "manifest.json", CORPUS / "support")
        injections = gate.load_injections(CORPUS / "injections" / "injections.json", manifest)
        self.assertGreaterEqual(len(injections), 39)
        for inj in injections:
            patch = (CORPUS / "injections" / inj.patch).read_text()
            self.assertIn("INJECTED", patch, inj.id)
            self.assertIn(f"+++ b/{inj.file}", patch, inj.id)
            config = manifest.config(inj.config)
            self.assertTrue(config.build or inj.build or inj.run, inj.id)

    def test_patches_apply_to_the_checkouts(self):
        # The checkouts the gate uses (another tree's with WEAVEC_CORPUS_WORKDIR).
        workdir = Path(os.environ.get("WEAVEC_CORPUS_WORKDIR", ROOT / "build" / "corpus"))
        manifest = gate.load_manifest(CORPUS / "manifest.json", CORPUS / "support")
        injections = gate.load_injections(CORPUS / "injections" / "injections.json", manifest)
        checked = 0
        for inj in injections:
            checkout = workdir / manifest.config(inj.config).project.name
            if not (checkout / inj.file).is_file():
                continue
            with tempfile.TemporaryDirectory() as directory:
                copy = Path(directory) / "src"
                target = copy / inj.file
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((checkout / inj.file).read_bytes())
                self.assertEqual(gate.apply_patch(CORPUS / "injections" / inj.patch, copy), "", inj.id)
                self.assertEqual(gate.check_injected_line(copy, inj), "", inj.id)
                checked += 1
        if not checked:
            self.skipTest("no corpus checkouts under build/corpus")


# -- the gates ---------------------------------------------------------------------------


class GateFunctionTest(unittest.TestCase):
    def test_drop_in(self):
        configs = [config("a"), config("b"), config("nobuild", build=())]
        good = {"builds": {"trap": {"built": True, "testsPassed": True},
                           "report": {"built": True, "testsPassed": True}}, "traps": 0}
        ok, detail = gate.drop_in_gate(configs, {"a": good, "b": good}, "trap")
        self.assertTrue(ok)
        self.assertEqual(sorted(detail), ["a", "b"])
        for bad in ({**good, "traps": 1},
                    {**good, "builds": {**good["builds"], "trap": {"built": False}}},
                    {**good, "builds": {**good["builds"], "report": {"built": True, "testsPassed": False}}},
                    {**good, "builds": {**good["builds"], "trap": {"built": True, "testsPassed": False}}}):
            self.assertFalse(gate.drop_in_gate(configs, {"a": good, "b": bad}, "trap")[0], bad)
        triaged = {**good, "trueTrapsOnly": True, "builds": {**good["builds"],
                                                             "trap": {"built": True, "testsPassed": False}}}
        self.assertTrue(gate.drop_in_gate(configs, {"a": good, "b": triaged}, "trap")[0])
        self.assertIsNone(gate.drop_in_gate([config("nobuild", build=())], {}, "trap")[0])

    def test_build_time(self):
        spec = {"maxBuildRatio": 1.5, "maxUnitRatio": 3.0, "minUnitCpuSeconds": 0.25}
        units = {"a.c": {"cpuSeconds": 1.0, "referenceCpuSeconds": 0.5},
                 "tiny.c": {"cpuSeconds": 1.0, "referenceCpuSeconds": 0.01}}
        entries = {"a": {"buildCpuRatio": 1.2, "quick": {"units": units}}}
        ok, detail = gate.build_time_gate([config("a")], entries, spec)
        self.assertTrue(ok)
        self.assertEqual(detail["a"]["worstUnit"], {"file": "a.c", "value": 2.0, "limit": 3.0})
        entries["a"]["buildCpuRatio"] = 1.6
        self.assertFalse(gate.build_time_gate([config("a")], entries, spec)[0])
        entries["a"]["buildCpuRatio"] = 1.0
        units["a.c"]["cpuSeconds"] = 2.0
        self.assertFalse(gate.build_time_gate([config("a")], entries, spec)[0])
        self.assertIsNone(gate.build_time_gate([config("a")], {}, spec)[0])

    def test_run_time(self):
        spec = {"sets": ["fresh35"], "maxOverhead": 2.0, "maxGeometricMean": 1.8,
                "maxOverheadPerConfig": {"lua": 2.0, "zlib": 1.4}}
        fresh = [config("f1", "fresh35", bench=True), config("f2", "fresh35", bench=True)]
        lua, zlib = config("lua", bench=True), config("zlib", bench=True)
        all_configs = fresh + [lua, zlib]

        def bench(ratio, asan=None):
            return {"bench": {"ratio": ratio, "asanRatio": asan}}

        entries = {"f1": bench(1.5, 3.0), "f2": bench(1.6), "lua": bench(1.9), "zlib": bench(1.3)}
        ok, detail = gate.run_time_gate(all_configs, all_configs, entries, spec)
        self.assertTrue(ok, detail)
        self.assertTrue(detail["geometricMean"]["complete"])
        # Over 2.0, but within ASan's ratio.
        entries["f1"] = bench(2.8, 3.0)
        self.assertFalse(gate.run_time_gate(all_configs, all_configs, entries, spec)[0])  # the mean is 2.12
        spec["maxGeometricMean"] = 2.5
        self.assertTrue(gate.run_time_gate(all_configs, all_configs, entries, spec)[0])
        entries["f2"] = bench(2.1, 1.5)
        self.assertFalse(gate.run_time_gate(all_configs, all_configs, entries, spec)[0])
        entries["f2"] = bench(1.6)
        entries["zlib"] = bench(1.5)
        self.assertFalse(gate.run_time_gate(all_configs, all_configs, entries, spec)[0])
        # The mean is gated only once every workload of the sets ran.
        spec["maxGeometricMean"] = 1.0
        ok, detail = gate.run_time_gate([fresh[0]], all_configs, {"f1": bench(1.5)}, spec)
        self.assertTrue(ok)
        self.assertFalse(detail["geometricMean"]["complete"])
        self.assertIsNone(gate.run_time_gate([lua], all_configs, {}, spec)[0])

    def test_verify(self):
        entries = {"a": {"builds": {"verify": {"reports": [report()]}}},
                   "b": {"bench": {"reports": []}}}
        self.assertTrue(gate.verify_gate([config("a"), config("b")], entries)[0])
        entries["b"]["bench"]["reports"] = [report(proven=True)]
        ok, detail = gate.verify_gate([config("a"), config("b")], entries)
        self.assertFalse(ok)
        self.assertEqual(detail["b"], ["weavec.proven: heap-use-after-free at a.c:3:5"])


# -- end to end, with fake tools ------------------------------------------------------------

FAKE_WEAVEC = r'''#!PYTHON
"""A stand-in for weavec: a diagnostic per BUG line, a summary line per file."""
import os, sys
args = sys.argv[1:]
if args == ["--version"]:
    print("weavec version fake"); sys.exit(0)
files = [a for a in args[:args.index("--")] if a.endswith(".c")]
status = 0
totals = [0, 0, 0]
for f in files:
    errors = warnings = 0
    lines = open(f).read().splitlines()
    for number, line in enumerate(lines, 1):
        if "BUG!" in line:
            print(f"{os.path.abspath(f)}:{number}:1: error: 'p' is freed twice [weavec::double-free]", file=sys.stderr)
            errors += 1
        elif "BUG" in line:
            print(f"{os.path.abspath(f)}:{number}:1: warning: 'p' may be freed twice [weavec::double-free]",
                  file=sys.stderr)
            warnings += 1
    sites = len(lines) + int(os.environ.get("FAKE_SITES", "0"))
    print(f"weavec: {os.path.basename(f)}: {sites:,} sites: {sites} proven, 0 not proven, 0 violations, "
          f"0 trusted; {errors} errors, {warnings} warnings", file=sys.stderr)
    totals = [totals[0] + sites, totals[1] + errors, totals[2] + warnings]
    status = status or (1 if errors else 0)
if "--whole-program" in args:
    print(f"weavec: program program: {totals[0]} sites in {len(files)} units: {totals[0]} proven, 0 not proven, "
          f"0 violations, 0 trusted; {totals[1]} errors, {totals[2]} warnings", file=sys.stderr)
sys.exit(status)
'''

FAKE_CC = r'''#!PYTHON
"""A stand-in for weavec-cc (and, named fake-clang, for the reference compiler).

-c writes an "object" that lists its source and, for weavec-cc with
-fweavec-ledger=, a version 3 ledger: one access per line, FAKE_PROVEN of
them proven. Linking writes a shell script: the program. A source line
`REPORT kind` makes it report that guard at that line when FAKE_TRAP is set
at run time, `REPORT! kind` always; `PROVEN kind` reports a weavec.proven
monitor in verify mode when FAKE_MONITOR is set. In trap and verify mode a
report kills the program with SIGTRAP. The reference compiler's programs
only print the result.
"""
import json, os, shlex, sys
args = sys.argv[1:]
reference = os.path.basename(sys.argv[0]) == "fake-clang"
if args == ["--version"]:
    print("clang version fake" if reference else "weavec-cc version fake"); sys.exit(0)
out = args[args.index("-o") + 1] if "-o" in args else "a.out"
checks = next((a.split("=", 1)[1] for a in args if a.startswith("-fweavec-checks=")), "trap")
ledger = next((a.split("=", 1)[1] for a in args if a.startswith("-fweavec-ledger=")), None)
sources = [a for a in args if a.endswith(".c")]
for obj in (a for a in args if a.endswith(".o") and a != out):
    sources += open(obj).read().split()
sources = [os.path.abspath(s) if os.path.exists(s) else s for s in sources]
if "-c" in args:
    with open(out, "w") as f:
        f.write("\n".join(sources))
    if ledger and not reference:
        if ledger.endswith("/"):
            ledger = os.path.join(ledger, os.path.basename(out) + ".ledger.json")
        lines = sum(len(open(s).read().splitlines()) for s in sources)
        proven = min(lines, int(os.environ.get("FAKE_PROVEN", "1")))
        summary = {"accesses": lines, "proven": proven, "guarded": lines - proven, "unguarded": 0}
        rows = [{"file": sources[0], "line": 1, "column": 1, "function": "f", "operation": "load", "bytes": 4,
                 "outcome": "proven", "reason": "in-bounds"}] * proven
        json.dump({"schema": "weavec-ledger", "version": 3, "units": [
            {"source": sources[0], "object": out, "config": {"checks": checks}, "summary": summary,
             "rows": rows}]}, open(ledger, "w"))
    sys.exit(0)
body = ["#!/bin/sh", 'emit() { if [ -n "$WEAVEC_RT_REPORT_LOG" ]; then echo "$1" >> "$WEAVEC_RT_REPORT_LOG"; '
        'else echo "$1" >&2; fi; }']
if not reference:
    for source in sources:
        for number, line in enumerate(open(source).read().splitlines(), 1):
            for marker, condition, prefix in (("REPORT!", "true", ""), ("REPORT", '[ -n "$FAKE_TRAP" ]', ""),
                                              ("PROVEN", '[ -n "$FAKE_MONITOR" ]' if checks == "verify" else "false",
                                               "weavec.proven: ")):
                if marker + " " in line:
                    kind = line.split(marker + " ", 1)[1].split()[0]
                    text = f"weavec: {prefix}{kind} at {os.path.basename(source)}:{number}:5: read of 4 bytes at 0x10"
                    stop = "kill -TRAP $$" if checks in ("trap", "verify") else ":"
                    body.append(f"if {condition}; then emit {shlex.quote(text)}; {stop}; fi")
                    break
body.append("echo result 42")
with open(out, "w") as f:
    f.write("\n".join(body) + "\n")
os.chmod(out, 0o755)
'''

PROGRAM = """int helper(int);
int main(void) {
    /* REPORT heap-use-after-free */
    /* PROVEN heap-buffer-overflow */
    return helper(1);
}
"""


class EndToEndTest(unittest.TestCase):
    """The gate against a local git repository and fake tools."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="weavec-corpus-gate-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.upstream = self.root / "upstream"
        self.upstream.mkdir()
        (self.upstream / "a.c").write_text("int helper(int i) {\n    return i; /* BUG! */\n}\n")
        (self.upstream / "b.c").write_text("int other(void) { return 1; }\n/* BUG */\n")
        (self.upstream / "prog.c").write_text(PROGRAM)
        self.sha = self.commit()
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        self.weavec = bin_dir / "fake-weavec"
        self.weavec.write_text(FAKE_WEAVEC.replace("#!PYTHON", f"#!{sys.executable}"))
        self.weavec_cc = bin_dir / "fake-weavec-cc"
        self.weavec_cc.write_text(FAKE_CC.replace("#!PYTHON", f"#!{sys.executable}"))
        self.clang = bin_dir / "fake-clang"
        self.clang.write_text(FAKE_CC.replace("#!PYTHON", f"#!{sys.executable}"))
        for tool in (self.weavec, self.weavec_cc, self.clang):
            tool.chmod(0o755)
        self.manifest_data = manifest_data(
            {"name": "units", "compile": {"files": ["a.c", "b.c"], "args": ["-I."]},
             "build": ['"$CC" -c a.c -o a.o', '"$CC" prog.c a.o -o prog'], "test": ["./prog"],
             "bench": {"name": "b", "build": ['"$CC" b.c -o benchprog'], "command": "./benchprog", "repeat": 2}},
            {"name": "whole", "compile": {"files": ["*.c"]}, "wholeProgram": True},
            {"name": "held", "heldOut": True, "set": "fresh35", "compile": {"files": ["b.c"]}},
            gates={"buildTime": {"maxBuildRatio": 1000}})
        self.manifest_data["projects"][0].update(url=str(self.upstream), sha=self.sha)
        self.manifest = self.root / "manifest.json"
        self.manifest.write_text(json.dumps(self.manifest_data))
        self.expected = self.root / "expected.json"
        self.triage = self.root / "triage.json"
        self.injections = self.root / "injections" / "injections.json"
        self.injections.parent.mkdir()
        self.write_injections([])
        self.write_triage([entry(config=name, file="a.c", line=2, id_="double-free") for name in ("units", "whole")])

    def commit(self) -> str:
        env = {**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.com",
               "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.com"}
        for cmd in (["git", "init", "--quiet"], ["git", "add", "."], ["git", "commit", "--quiet", "-m", "x"]):
            subprocess.run(cmd, cwd=self.upstream, check=True, env=env)
        return subprocess.run(["git", "rev-parse", "HEAD"], cwd=self.upstream, check=True, capture_output=True,
                              text=True).stdout.strip()

    def write_triage(self, entries, failures=()):
        self.triage.write_text(json.dumps({"schema": gate.TRIAGE_SCHEMA, "version": 2, "entries": entries,
                                           "guardFailures": list(failures)}))

    def write_injections(self, items):
        self.injections.write_text(json.dumps({"schema": gate.INJECTIONS_SCHEMA, "version": 2,
                                               "injections": items}))

    def write_patch(self, name, file, old, new):
        source = (self.upstream / file).read_text()
        self.assertEqual(source.count(old), 1)
        patched = source.replace(old, new)
        path = self.injections.parent / name
        path.write_text("".join(difflib.unified_diff(source.splitlines(keepends=True),
                                                     patched.splitlines(keepends=True),
                                                     fromfile=f"a/{file}", tofile=f"b/{file}")))

    def run_gate(self, *extra, env=None):
        argv = ["--manifest", str(self.manifest), "--expected", str(self.expected), "--triage", str(self.triage),
                "--injections", str(self.injections), "--workdir", str(self.root / "work"),
                "--support-dir", str(self.root), "--bench-dir", str(self.root), "--jobs", "2",
                "--weavec", str(self.weavec), "--weavec-cc", str(self.weavec_cc), "--cc", str(self.clang),
                "--json", str(self.root / "results.json"), *extra]
        saved = {k: os.environ.get(k) for k in (env or {})}
        os.environ.update(env or {})
        out, err = io.StringIO(), io.StringIO()
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                status = gate.main(argv)
        finally:
            for key, value in saved.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value
        return status, out.getvalue() + err.getvalue()

    def results(self):
        return json.loads((self.root / "results.json").read_text())

    def recorded(self):
        return json.loads(self.expected.read_text())["platforms"][gate.platform_key()]["configs"]

    def test_quick_ratchet_and_triage(self):
        status, output = self.run_gate("--quick", "--update")
        self.assertEqual(status, 0, output)
        self.assertNotIn("held", self.recorded())  # --quick leaves the held-out configs out
        self.assertEqual(self.recorded()["units"]["ledger"],
                         {"accesses": 5, "proven": 2, "guarded": 3, "unguarded": 0, "provenShare": 0.4})
        self.assertEqual(self.recorded()["units"]["analysis"]["errors"], 1)
        self.assertEqual(self.recorded()["whole"]["analysis"]["warnings"], 1)
        status, output = self.run_gate("--quick")
        self.assertEqual(status, 0, output)
        self.assertEqual(self.results()["gates"]["triage"]["status"], "pass")
        self.assertEqual(self.results()["configs"]["whole"]["quick"]["analysis"]["sites"], 11)
        # More proven accesses: a note, not a failure.
        status, output = self.run_gate("--quick", env={"FAKE_PROVEN": "3"})
        self.assertEqual(status, 0, output)
        self.assertIn("(better); --update records it", output)
        # Fewer: a regression.
        status, output = self.run_gate("--quick", env={"FAKE_PROVEN": "0"})
        self.assertEqual(status, 1, output)
        self.assertIn("ratchet regression: units.ledger.provenShare: 0.4 -> 0.0 (worse)", output)
        # Measurements from elsewhere are recorded without rerunning.
        status, output = self.run_gate("--update-from", str(self.root / "results.json"))
        self.assertEqual(status, 0, output)
        self.assertEqual(self.recorded()["units"]["ledger"]["proven"], 0)
        # A definite error needs a verdict, and not a false one.
        self.write_triage([entry(config="units", file="a.c", line=2, id_="double-free")])
        status, output = self.run_gate("--quick", env={"FAKE_PROVEN": "0"})
        self.assertEqual(status, 1, output)
        self.assertIn("untriaged definite double-free in whole at a.c:2", output)
        self.write_triage([entry(config=name, file="a.c", line=2, id_="double-free", verdict="false")
                           for name in ("units", "whole")])
        status, output = self.run_gate("--quick", env={"FAKE_PROVEN": "0"})
        self.assertEqual(status, 1, output)
        self.assertIn("definite double-free triaged false in units at a.c:2", output)
        # The held-out configs run when asked for.
        self.write_triage([entry(config=name, file="a.c", line=2, id_="double-free") for name in ("units", "whole")])
        status, output = self.run_gate("--quick", "--held-out", env={"FAKE_PROVEN": "0"})
        self.assertEqual(status, 0, output)
        self.assertIn("held: not recorded", output)
        self.assertEqual(self.results()["sets"], {"original": ["units", "whole"], "fresh35": ["held"]})

    def test_a_tool_failure_is_not_recorded(self):
        (self.root / "bin" / "fake-weavec").write_text(f"#!{sys.executable}\nimport sys; sys.exit(3)\n")
        status, output = self.run_gate("--quick", "--update")
        self.assertEqual(status, 2, output)  # the probe sees it first
        self.assertIn("prints no summary line", output)
        self.assertFalse(self.expected.exists())

    def test_a_modified_checkout_is_refused(self):
        status, output = self.run_gate("--quick", "--only", "units")
        self.assertEqual(status, 0, output)
        (self.root / "work" / "proj" / "a.c").write_text("changed\n")
        status, output = self.run_gate("--quick", "--only", "units")
        self.assertEqual(status, 2, output)
        self.assertIn("modified", output)

    def test_full(self):
        status, output = self.run_gate("--full", "--only", "units", "--no-asan", "--update")
        self.assertEqual(status, 0, output)
        results = self.results()
        entry_ = results["configs"]["units"]
        self.assertEqual(sorted(entry_["builds"]), ["reference", "report", "trap"])
        self.assertTrue(entry_["builds"]["trap"]["testsPassed"])
        self.assertIsNone(entry_["builds"]["reference"]["testsPassed"])  # timed, not tested
        self.assertEqual(entry_["traps"], 0)
        self.assertIsNotNone(entry_["buildCpuRatio"])
        self.assertEqual(results["gates"]["drop-in"]["status"], "pass")
        self.assertEqual(results["gates"]["build-time"]["status"], "pass")
        self.assertEqual(entry_["bench"]["outputs"], {"reference": "result 42", "weavec-cc": "result 42"})
        self.assertEqual(len(entry_["bench"]["times"]["weavec-cc"]), 2)
        # A guard failing in the test suite: the trap-mode run dies and the
        # report-mode rerun names the site.
        status, output = self.run_gate("--full", "--only", "units", "--no-asan", env={"FAKE_TRAP": "1"})
        self.assertEqual(status, 1, output)
        self.assertIn("a guard failed in the test suite: heap-use-after-free at prog.c:3:5", output)
        self.assertIn("units (trap): test suite failed", output)
        self.assertEqual(self.results()["gates"]["drop-in"]["status"], "fail")
        self.assertEqual(self.results()["configs"]["units"]["traps"], 1)
        # A true bug of the project, triaged: not a trap.
        self.write_triage([entry(config=name, file="a.c", line=2, id_="double-free") for name in ("units", "whole")],
                          [{"config": "units", "file": "prog.c", "line": 3, "verdict": "true", "note": "n"}])
        status, output = self.run_gate("--full", "--only", "units", "--no-asan", env={"FAKE_TRAP": "1"})
        self.assertEqual(status, 0, output)
        self.assertIn("trapped only at guard failures triaged as true bugs", output)

    def test_verify(self):
        status, output = self.run_gate("--full", "--only", "units", "--checks", "verify", "--no-asan")
        self.assertEqual(status, 0, output)
        self.assertEqual(self.results()["gates"]["verify"]["status"], "pass")
        self.assertNotIn("run-time", self.results()["gates"])
        status, output = self.run_gate("--full", "--only", "units", "--checks", "verify", "--no-asan",
                                       env={"FAKE_MONITOR": "1"})
        self.assertEqual(status, 1, output)
        self.assertIn("weavec.proven: heap-buffer-overflow at prog.c:4:5", output)
        self.assertEqual(self.results()["gates"]["verify"]["status"], "fail")

    def test_injections(self):
        self.write_patch("stops.patch", "a.c", "    return i; /* BUG! */\n",
                         "    return i; /* BUG! */\n    /* REPORT! heap-buffer-overflow INJECTED */\n")
        self.write_patch("elsewhere.patch", "a.c", "    return i; /* BUG! */\n",
                         "    return i; /* BUG! */\n    /* INJECTED */\n")
        self.write_injections([
            {"id": "stops", "config": "units", "patch": "stops.patch", "file": "a.c", "line": 3},
            {"id": "wrong-kind", "config": "units", "patch": "stops.patch", "file": "a.c", "line": 3,
             "kinds": ["heap-use-after-free"]},
            {"id": "silent", "config": "units", "patch": "elsewhere.patch", "file": "a.c", "line": 3,
             "run": "./prog && echo ran"},
            {"id": "unmarked", "config": "units", "patch": "elsewhere.patch", "file": "a.c", "line": 2}])
        status, output = self.run_gate("--inject")
        self.assertEqual(status, 1, output)
        runs = {r["id"]: r for r in self.results()["injections"]}
        self.assertEqual(runs["stops"]["via"], ["heap-buffer-overflow at a.c:3:5"])
        self.assertFalse(runs["wrong-kind"]["stopped"])
        self.assertFalse(runs["silent"]["stopped"])
        self.assertIn("does not carry the INJECTED marker", runs["unmarked"]["failures"][0])
        self.assertEqual(self.results()["gates"]["injections"]["detail"]["missed"],
                         ["wrong-kind", "silent", "unmarked"])
        status, output = self.run_gate("--inject", "--injection", "stops")
        self.assertEqual(status, 0, output)
        status, output = self.run_gate("--inject", "--injection", "nope")
        self.assertEqual(status, 2, output)

    def test_reference_only(self):
        status, output = self.run_gate("--full", "--reference-only", "--only", "units")
        self.assertEqual(status, 0, output)
        results = self.results()
        self.assertEqual(sorted(results["configs"]["units"]["builds"]), ["reference"])
        self.assertTrue(results["configs"]["units"]["builds"]["reference"]["testsPassed"])
        self.assertNotIn("weavec-cc", results["binaries"])
        self.assertEqual(results["configs"]["units"]["bench"]["outputs"], {"reference": "result 42"})


class RepositoryFilesTest(unittest.TestCase):
    """The workflows run the gate with flags it has."""

    def test_workflow_invocations_parse(self):
        for workflow in ("ci.yml", "corpus.yml"):
            text = (ROOT / ".github" / "workflows" / workflow).read_text()
            start = text.index("python3 scripts/corpus-gate.py")
            lines = []
            for line in text[start:].splitlines():
                lines.append(line.rstrip(" \\"))
                if not line.endswith("\\"):
                    break
            words = shlex.split(" ".join(lines).replace("${{ matrix.preset }}", "preset"))
            args = gate.parse_args(words[2:])
            self.assertTrue(args.quick or args.full, workflow)


if __name__ == "__main__":
    unittest.main()
