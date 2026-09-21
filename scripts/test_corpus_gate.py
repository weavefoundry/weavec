#!/usr/bin/env python3
"""Tests for scripts/corpus-gate.py (RFC 0030, section 17.5).

Synthetic data only: fake tools written by the tests stand in for weavec and
weavec-cc, and a local git repository stands in for a corpus project, so the
tests need neither the network nor a WeaveC build. The checks that read the
repository's own test/corpus files (the manifest, the injections and the
legacy baseline) need no build either.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import subprocess
import sys
import tempfile
import textwrap
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


def diag(file="a.c", line=1, id_="use-after-free", severity="error", certainty="definite", facet="",
         fingerprint="fp", message="use of 'p' after it was freed"):
    return gate.Diagnostic(file=file, line=line, col=1, severity=severity, id=id_, message=message,
                           certainty=certainty, facet=facet, fingerprint=fingerprint)


def summary(proven=0, checked=0, violation=0, unresolved=0, trusted=0, errors=0, warnings=0, functions=1,
            spatial_unresolved=0, null_unresolved=0, spatial_total=0, null_total=0):
    facets = {f: {o: 0 for o in gate.OUTCOMES} for f in gate.FACETS}
    facets["spatial"]["unresolved"] = spatial_unresolved
    facets["spatial"]["proven"] = spatial_total - spatial_unresolved
    facets["null"]["unresolved"] = null_unresolved
    facets["null"]["proven"] = null_total - null_unresolved
    facets["temporal"]["proven"] = proven
    facets["temporal"]["checked"] = checked
    facets["temporal"]["violation"] = violation
    facets["temporal"]["unresolved"] = unresolved
    facets["temporal"]["trusted"] = trusted
    sites = sum(sum(v.values()) for v in facets.values())
    return {"sites": sites, "proven": 0, "checked": 0, "violation": 0, "unresolved": 0, "trusted": 0,
            "facets": facets, "errors": errors, "warnings": warnings, "functions": functions,
            "overBudget": [], "unresolvedReasons": {}}


def ledger(summary_, diagnostics=(), scope="unit"):
    return {"schema": "weavec-ledger", "version": 1, "scope": scope, "summary": summary_,
            "units": [], "diagnostics": list(diagnostics)}


class ParsingTest(unittest.TestCase):
    def test_diagnostics_are_relative_to_the_checkout(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            text = "\n".join([
                f"{root}/src/a.c:3:5: error: 'p' is freed twice [weavec::double-free]",
                "src/b.c:7:1: warning: call to 'f' is not checked [weavec::annotation-required]",
                f"{root}/src/a.c:9:1: error: unknown type name 'foo'",
                "In file included from x.h:1:",
            ])
            diagnostics, clang_errors = gate.parse_diagnostics(text, root)
        self.assertEqual([(d.file, d.line, d.id) for d in diagnostics],
                         [("src/a.c", 3, "double-free"), ("src/b.c", 7, "annotation-required")])
        self.assertEqual(clang_errors, 1)
        self.assertEqual(diagnostics[0].render(), "src/a.c:3:5: error: 'p' is freed twice [weavec::double-free]")

    def test_failures_never_look_clean(self):
        ok = gate.classify_failure
        self.assertEqual(ok(proc(), [], 0), "")
        self.assertEqual(ok(proc(1), [diag()], 0), "")  # an ownership error is a checked result
        self.assertIn("without a WeaveC error", ok(proc(1), [], 0))
        self.assertIn("status -11", ok(proc(-11), [], 0))
        self.assertIn("Clang parse error", ok(proc(0), [], 2))
        self.assertIn("timeout", ok(proc(timed_out=True), [], 0))
        self.assertEqual(ok(proc(error="No such file"), [], 0), "No such file")
        limit = diag(id_="analysis-incomplete", severity="warning",
                     message="analysis is incomplete: summary iteration limit reached")
        self.assertEqual(ok(proc(), [limit], 0), "analysis reached an iteration limit")
        self.assertIn("converge", ok(proc(stderr="program analysis did not converge"), [], 0))

    def test_report_lines_and_trap_signs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            text = "\n".join([
                f"weavec: runtime check failed: index at {root}/src/x.c:10:3",
                "weavec: runtime check failed: index at ../src/x.c:10:3",
                "weavec: runtime check failed: nonnull at y.c:2:1",
                "weavec: runtime check failed: nonnull at y.c:2:1",
            ])
            reports = gate.parse_reports(text, root)
        self.assertEqual([(r["template"], gate.normalise_report_file(r["file"]), r["line"]) for r in reports],
                         [("index", "src/x.c", 10), ("index", "src/x.c", 10), ("nonnull", "y.c", 2)])
        self.assertTrue(gate.trap_evidence(proc(-5)))
        self.assertTrue(gate.trap_evidence(proc(132)))
        self.assertTrue(gate.trap_evidence(proc(1, stderr="sh: line 1: 42 Illegal instruction ./example")))
        self.assertTrue(gate.trap_evidence(proc(2, stdout="make: *** [test] Error 133")))
        self.assertTrue(gate.trap_evidence(proc(8, stdout="  7/22 Test  #7: parse_number ***Exception: Illegal")))
        self.assertFalse(gate.trap_evidence(proc(1, stdout="1 test failed")))
        self.assertFalse(gate.trap_evidence(proc(-11)))

    def test_multiset_difference(self):
        self.assertEqual(gate.diff_sorted(["a", "b", "b"], ["b", "c"]), ["- a", "- b", "+ c"])
        self.assertEqual(gate.diff_sorted(["a"], ["a"]), [])


class ProcessTest(unittest.TestCase):
    def test_timeout_kills_the_session(self):
        start = time.perf_counter()
        result = gate.run_process(["/bin/sh", "-c", "sleep 30 & sleep 30"], cwd=".", timeout=0.5)
        self.assertTrue(result.timed_out)
        self.assertLess(time.perf_counter() - start, 10)

    def test_cpu_time_includes_descendants(self):
        busy = "x = 0\nfor i in range(3000000): x += i\n"
        direct = gate.run_process([sys.executable, "-c", busy], cwd=".", timeout=120)
        # `&& true` keeps the shell from exec'ing Python: it runs as a grandchild.
        nested = gate.run_process(["/bin/sh", "-c", f"{sys.executable} -c '{busy}' && true"], cwd=".",
                                  timeout=120)
        self.assertEqual(nested.returncode, 0, nested.output)
        self.assertGreater(direct.user, 0.05)
        self.assertGreaterEqual(nested.user, 0.7 * direct.user)

    def test_signals_are_negative_status(self):
        result = gate.run_process(["/bin/sh", "-c", "kill -TRAP $$"], cwd=".", timeout=10)
        self.assertEqual(result.signal, 5)
        self.assertEqual(gate.describe_status(result), "killed by SIGTRAP")


class ManifestTest(unittest.TestCase):
    CONFIGS = ["sds", "cJSON", "cJSON-program", "jsmn", "log.c", "printf", "linenoise", "linenoise-program",
               "zlib", "lua", "jansson"]

    def write(self, directory: Path, data: dict) -> Path:
        path = directory / "manifest.json"
        path.write_text(json.dumps(data))
        return path

    def minimal(self, **config):
        base = {"name": "p", "compile": {"files": ["a.c"], "args": []}}
        base.update(config)
        return {"schema": "weavec-corpus-manifest", "version": 1,
                "projects": [{"name": "p", "url": "u", "sha": "a" * 40, "support": [], "configs": [base]}]}

    def test_repository_manifest(self):
        manifest = gate.load_manifest(CORPUS / "manifest.json", CORPUS / "support")
        self.assertEqual(sorted(c.name for c in manifest.configs), sorted(self.CONFIGS))
        for project in manifest.projects:
            self.assertRegex(project.sha, r"^[0-9a-f]{40}$")
        whole = {c.name for c in manifest.configs if c.whole_program}
        self.assertEqual(whole, {"cJSON-program", "linenoise-program", "zlib", "lua", "jansson"})
        with_tests = {c.name for c in manifest.configs if c.test}
        self.assertTrue({"sds", "cJSON", "jsmn", "zlib", "lua", "jansson"} <= with_tests)
        benches = {c.name: c.bench.name for c in manifest.configs if c.bench}
        self.assertEqual(set(benches), {"lua", "zlib", "cJSON"})
        self.assertIn("make -j8", manifest.config("zlib").build)
        # Section 17.5: a build config may lower a definite error to reach a
        # successful build only next to a triage entry whose verdict is true.
        triaged_true = {e["fingerprint"] for e in gate.load_triage(CORPUS / "triage.json")
                        if e.get("verdict") == "true"}
        for config in manifest.configs:
            for lowered in config.lowered:
                self.assertRegex(lowered["flag"], r"^-Wno-error=weavec-[a-z-]+$")
                self.assertRegex(lowered["fingerprint"], r"^[0-9a-f]{32}$")
                self.assertTrue(lowered.get("note"), f"{config.name}: a lowering needs its reason")
                self.assertIn(lowered["fingerprint"], triaged_true,
                              f"{config.name} lowers {lowered['flag']} for a finding that is not "
                              f"triaged true; section 17.5 allows it only there")

    def test_invalid_manifests(self):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            bad_sha = self.minimal()
            bad_sha["projects"][0]["sha"] = "master"
            with self.assertRaisesRegex(gate.GateError, "40 lowercase hex"):
                gate.load_manifest(self.write(d, bad_sha), d)
            dup = self.minimal()
            dup["projects"][0]["configs"].append(dict(dup["projects"][0]["configs"][0]))
            with self.assertRaisesRegex(gate.GateError, "duplicate"):
                gate.load_manifest(self.write(d, dup), d)
            with self.assertRaisesRegex(gate.GateError, "lower errors only"):
                gate.load_manifest(self.write(d, self.minimal(build=["make CFLAGS=-Wno-error"])), d)
            with self.assertRaisesRegex(gate.GateError, "lower errors only"):
                gate.load_manifest(self.write(d, self.minimal(build=["make CC='cc -w'"])), d)
            with self.assertRaisesRegex(gate.GateError, "not -Wno-error=weavec-<id>"):
                gate.load_manifest(self.write(d, self.minimal(lowered=[{"flag": "-Wno-error", "fingerprint": "f"}])), d)
            ok = self.minimal(lowered=[{"flag": "-Wno-error=weavec-double-free", "fingerprint": "f1"}])
            manifest = gate.load_manifest(self.write(d, ok), d)
            config = manifest.configs[0]
            entry = {"fingerprint": "f1", "config": "p", "id": "double-free", "certainty": "definite",
                     "file": "a.c", "line": 1, "verdict": "true"}
            self.assertEqual(gate.check_lowered_against_triage([config], [entry]), [])
            self.assertTrue(gate.check_lowered_against_triage([config], [{**entry, "verdict": "false"}]))
            self.assertTrue(gate.check_lowered_against_triage([config], [{**entry, "id": "use-after-free"}]))

    def test_legacy_command_is_corpus_py_s(self):
        manifest = gate.load_manifest(CORPUS / "manifest.json", CORPUS / "support")
        jansson = manifest.config("jansson")
        root = Path("/work/jansson")
        files = [root / "src/dump.c", root / "src/value.c"]
        cmd = gate.legacy_command("weavec", jansson, files, CORPUS / "support")
        self.assertEqual(cmd[:4], ["weavec", "--whole-program", "/work/jansson/src/dump.c",
                                   "/work/jansson/src/value.c"])
        self.assertEqual(cmd[4:6], ["--", "-ferror-limit=0"])
        self.assertIn(f"-I{CORPUS / 'support' / 'jansson'}", cmd)
        sds = manifest.config("sds")
        self.assertEqual(gate.legacy_groups(sds, [Path("a.c"), Path("b.c")]), [[Path("a.c")], [Path("b.c")]])
        self.assertEqual(gate.legacy_command("weavec", sds, [Path("/w/sds.c")], CORPUS / "support"),
                         ["weavec", "/w/sds.c", "--", "-ferror-limit=0", "-std=c99", "-I."])


class LegacyTest(unittest.TestCase):
    def units(self):
        u = gate.UnitRun("c", ["a.c"], "cmd", 1.0, 1.0, 1, [
            diag(id_="double-free"), diag(id_="leak", severity="warning"),
            diag(id_="analysis-incomplete", severity="warning"), diag(id_="annotation-required", severity="warning"),
        ], 0)
        return [u]

    def test_bug_claims_exclude_coverage_ids(self):
        config = gate.Config(name="c", project=gate.Project("p", "u", "a" * 40, []), files=["a.c"], args=[])
        tally = gate.tally_legacy(config, self.units())
        self.assertEqual(tally["bugClaims"], 2)
        self.assertEqual(tally["byId"], {"analysis-incomplete": 1, "annotation-required": 1, "double-free": 1,
                                         "leak": 1})
        totals = gate.legacy_totals({"c": tally, "d": tally})
        self.assertEqual(totals["bugClaims"], 4)
        self.assertEqual(totals["units"], 2)

    def test_comparison_with_the_recorded_baseline(self):
        config = gate.Config(name="c", project=gate.Project("p", "u", "a" * 40, []), files=["a.c"], args=[])
        tally = gate.tally_legacy(config, self.units())
        recorded = {"platform": "plat", "configs": {"c": {k: tally[k] for k in ("units", "byId", "bugClaims",
                                                                                "digest")}},
                    "totals": gate.legacy_totals({"c": tally})}
        self.assertEqual(gate.compare_legacy({"c": tally}, recorded, "plat"), ([], []))
        failures, notes = gate.compare_legacy({"c": tally}, recorded, "other")
        self.assertEqual(failures, [])
        self.assertTrue(notes)
        changed = json.loads(json.dumps(tally))
        changed["byId"]["leak"] = 2
        changed["bugClaims"] = 3
        failures, _ = gate.compare_legacy({"c": changed}, recorded, "plat")
        self.assertTrue(any("byId" in f for f in failures))
        other_digest = dict(tally, digest="sha256:0")
        failures, _ = gate.compare_legacy({"c": other_digest}, recorded, "plat")
        self.assertTrue(any("sorted diagnostics differ" in f for f in failures))
        self.assertTrue(gate.compare_legacy({"c": tally}, None, "plat")[0])

    def test_recorded_s0_baseline(self):
        """The v0.10.0 numbers S0 must reproduce (RFC 0030, Implementation plan)."""
        legacy = json.loads((CORPUS / "expected.json").read_text()).get("legacy", {})
        quick = legacy.get("quick")
        if not quick:
            self.skipTest("no legacy baseline recorded yet")
        totals = quick["totals"]
        self.assertEqual(totals["bugClaims"], 301)
        by_id = totals["byId"]
        self.assertEqual({k: by_id.get(k) for k in ("double-free", "use-after-free", "null-dereference", "leak",
                                                   "invalid-release", "lifetime-too-short")},
                         {"double-free": 56, "use-after-free": 32, "null-dereference": 186, "leak": 22,
                          "invalid-release": 3, "lifetime-too-short": 2})
        self.assertEqual(by_id["annotation-required"], 40)
        self.assertEqual(by_id["analysis-incomplete"], 4239)
        self.assertEqual(len(quick["configs"]), 11)
        injections = legacy.get("injections")
        if injections:
            results = injections["results"]
            for caught in ("jansson-df-strbuffer-close", "jansson-df-array-remove", "jansson-uaf-delete-string",
                           "cjson-uaf-delete", "sds-uaf-sdsfree", "sds-df-freesplitres",
                           "linenoise-uaf-freehistory"):
                self.assertTrue(results[caught], caught)
            self.assertFalse(results["lua-uaf-luah-free"])
            self.assertFalse(results["lua-df-freeproto"])


class LedgerTest(unittest.TestCase):
    def test_aggregation_and_share(self):
        analysis = gate.Analysis(kind="units")
        root = Path("/p")
        diagnostics = [{"id": "double-free", "severity": "error", "certainty": "definite", "facet": "temporal",
                        "message": "m", "file": "a.c", "line": 3, "column": 1, "fingerprint": "f1"}]
        gate.add_ledger(analysis, ledger(summary(errors=1, spatial_unresolved=2, spatial_total=10,
                                                 null_unresolved=1, null_total=10, proven=5), diagnostics), root)
        gate.add_ledger(analysis, ledger(summary(warnings=2, spatial_unresolved=1, spatial_total=10,
                                                 null_total=10)), root)
        self.assertEqual(analysis.errors, 1)
        self.assertEqual(analysis.warnings, 2)
        self.assertEqual(analysis.spatial_null_share, round(4 / 40, 4))
        measured = analysis.measured()
        self.assertEqual(measured["ledger"]["unresolved"], 4)
        self.assertEqual(measured["ledger"]["proven"], 36 + 5)
        self.assertEqual(measured["workCounters"]["functions"], 2)
        self.assertEqual([d.fingerprint for d in analysis.diagnostics], ["f1"])
        with self.assertRaises(ValueError):
            gate.validate_ledger({"schema": "x"}, Path("l.json"))
        with self.assertRaises(ValueError):
            gate.validate_ledger({"schema": "weavec-ledger", "version": 2, "summary": {}}, Path("l.json"))

    def test_program_ledger_diagnostics_are_deduplicated(self):
        item = {"id": "use-after-free", "severity": "warning", "certainty": "possible", "file": "/p/a.c",
                "line": 2, "column": 3, "message": "m", "fingerprint": "f"}
        data = ledger(summary(), [item], scope="program")
        data["units"] = [{"source": "a.c", "diagnostics": [item]}]
        found = gate.ledger_diagnostics(data, Path("/p"))
        self.assertEqual([(d.file, d.line) for d in found], [("a.c", 2)])


class RatchetTest(unittest.TestCase):
    def analysis(self, **kw):
        base = {"errors": 0, "warnings": 2, "ledger": {"sites": 100, "proven": 60, "checked": 30, "violation": 0,
                                                        "unresolved": 10, "trusted": 0},
                "unresolvedShare": {"spatialNull": 0.1}, "cpuSeconds": 10.0,
                "workCounters": {"blockTransfers": 1000, "functions": 10, "sites": 100}}
        for key, value in kw.items():
            gate.set_path(base, tuple(key.split(".")), value)
        return base

    def expected(self, machine="m", **configs):
        return {"schema": "weavec-corpus-expected", "version": 1,
                "platforms": {"plat": {"machine": machine, "configs": configs}}}

    def compare(self, now, before, machine="m"):
        return gate.compare_ratchet({"c": now}, self.expected(c=before), "plat", machine)

    def test_equal_passes(self):
        result = self.compare({"units": self.analysis()}, {"units": self.analysis()})
        self.assertFalse(result.failed, result)

    def test_worse_counts_fail(self):
        result = self.compare({"units": self.analysis(errors=1)}, {"units": self.analysis()})
        self.assertTrue(result.regressions)
        result = self.compare({"units": self.analysis(**{"ledger.proven": 59})}, {"units": self.analysis()})
        self.assertTrue(result.regressions)
        result = self.compare({"units": self.analysis(**{"unresolvedShare.spatialNull": 0.2})},
                              {"units": self.analysis()})
        self.assertTrue(result.regressions)

    def test_better_counts_must_be_recorded(self):
        result = self.compare({"units": self.analysis(**{"ledger.unresolved": 5})}, {"units": self.analysis()})
        self.assertFalse(result.regressions)
        self.assertTrue(result.improvements)
        self.assertTrue(result.failed)
        result = self.compare({"units": self.analysis(**{"ledger.checked": 31})}, {"units": self.analysis()})
        self.assertTrue(result.changes)

    def test_budgets(self):
        self.assertFalse(self.compare({"units": self.analysis(cpuSeconds=10.9)}, {"units": self.analysis()}).failed)
        self.assertTrue(self.compare({"units": self.analysis(cpuSeconds=11.5)}, {"units": self.analysis()}).over_budget)
        # Sub-second times: within the one-second slack.
        self.assertFalse(self.compare({"units": self.analysis(cpuSeconds=0.09)},
                                      {"units": self.analysis(cpuSeconds=0.06)}).failed)
        other = self.compare({"units": self.analysis(cpuSeconds=50.0)}, {"units": self.analysis()}, machine="x")
        self.assertFalse(other.failed)
        self.assertTrue(other.notes)
        self.assertFalse(self.compare({"units": self.analysis(**{"workCounters.blockTransfers": 1019})},
                                      {"units": self.analysis()}).failed)
        self.assertTrue(self.compare({"units": self.analysis(**{"workCounters.blockTransfers": 1021})},
                                     {"units": self.analysis()}).over_budget)
        self.assertFalse(self.compare({"units": self.analysis(cpuSeconds=5.0)}, {"units": self.analysis()}).failed)

    def test_traps_overhead_and_missing(self):
        self.assertTrue(self.compare({"traps": 1}, {"traps": 0}).regressions)
        self.assertTrue(self.compare({"traps": 0}, {"traps": 1}).improvements)
        self.assertFalse(self.compare({"overhead": 1.05}, {"overhead": 1.0}).failed)
        self.assertTrue(self.compare({"overhead": 1.2}, {"overhead": 1.0}).over_budget)
        self.assertTrue(gate.compare_ratchet({"c": {}}, {"platforms": {}}, "plat", "m").missing)
        self.assertTrue(self.compare({"program": self.analysis()}, {"units": self.analysis()}).missing)
        # A section this run did not measure is not compared.
        self.assertFalse(self.compare({"overhead": 1.0}, {"units": self.analysis(), "overhead": 1.0}).failed)

    def test_update_merges_one_platform(self):
        expected = {"schema": "weavec-corpus-expected", "version": 1, "legacy": {"quick": {"x": 1}},
                    "platforms": {"other": {"configs": {"c": {"traps": 0}}}}}
        merged = gate.merge_expected(expected, {"c": {"units": self.analysis()}, "d": {"traps": 0}}, "plat", "m",
                                     "weavec 1")
        self.assertEqual(merged["legacy"], expected["legacy"])
        self.assertEqual(merged["platforms"]["other"], expected["platforms"]["other"])
        self.assertEqual(list(merged["platforms"]["plat"]["configs"]), ["c", "d"])
        again = gate.merge_expected(merged, {"c": {"traps": 0}}, "plat", "m", "weavec 1")
        self.assertIn("units", again["platforms"]["plat"]["configs"]["c"])
        self.assertFalse(gate.compare_ratchet({"c": {"units": self.analysis()}}, merged, "plat", "m").failed)


class TriageTest(unittest.TestCase):
    def entry(self, fingerprint="f1", verdict="true", certainty="definite", **kw):
        base = {"fingerprint": fingerprint, "config": "c", "id": "double-free", "certainty": certainty,
                "file": "a.c", "line": 3, "verdict": verdict, "note": "n"}
        base.update(kw)
        return base

    def test_findings(self):
        diagnostics = [
            diag(fingerprint="f1"),
            diag(fingerprint="f1"),  # the same finding from the program analysis
            diag(fingerprint="f2", severity="warning", certainty="possible", facet="temporal"),
            diag(fingerprint="f3", id_="null-dereference", severity="warning", certainty="possible"),
            diag(fingerprint="f4", id_="leak", severity="warning", certainty="possible"),
        ]
        findings = gate.findings_of("c", diagnostics)
        self.assertEqual([(f["fingerprint"], f["certainty"]) for f in findings],
                         [("f1", "definite"), ("f2", "possible")])

    def test_untriaged_false_and_stale(self):
        findings = gate.findings_of("c", [diag(fingerprint="f1"), diag(fingerprint="f2")])
        findings += gate.findings_of("c", [diag(fingerprint="f1")])  # seen again by --full
        result = gate.check_triage(findings, [self.entry("f1"), self.entry("f9")], {"c"})
        self.assertEqual([f["fingerprint"] for f in result.untriaged], ["f2"])
        self.assertEqual(len(result.definite_errors), 2)
        self.assertEqual([e["fingerprint"] for e in result.stale], ["f9"])
        self.assertTrue(result.failed)
        result = gate.check_triage(findings[:1], [self.entry("f1", verdict="false")], {"c"})
        self.assertEqual(len(result.false_errors), 1)
        result = gate.check_triage(findings[:1], [self.entry("f1")], {"c"})
        self.assertFalse(result.failed)
        # Stale entries of configs that did not run are not reported.
        self.assertFalse(gate.check_triage([], [self.entry("f9", config="d")], {"c"}).stale)

    def test_invalid_entries(self):
        result = gate.check_triage([], [self.entry(verdict="maybe"), {"fingerprint": "x"}], {"c"})
        self.assertEqual(len(result.invalid), 2)
        self.assertEqual(gate.true_error_sites([self.entry(), self.entry("f2", verdict="false")]),
                         {("c", "a.c", 3)})

    def test_repository_triage_file(self):
        entries = gate.load_triage(CORPUS / "triage.json")
        result = gate.check_triage([], entries, set(ManifestTest.CONFIGS))
        self.assertEqual(result.invalid, [], "every entry needs a config, a site and a verdict")
        seen = set()
        for entry in entries:
            self.assertRegex(entry["fingerprint"], r"^[0-9a-f]{32}$")
            self.assertIn(entry["verdict"], ("true", "false"))
            self.assertTrue(str(entry.get("note", "")).strip(),
                            f"{entry['fingerprint']}: a verdict needs the evidence behind it")
            # One site can be reported by more than one config, so the key is
            # the pair, not the fingerprint alone.
            key = (entry["fingerprint"], entry["config"])
            self.assertNotIn(key, seen, "one entry per finding per config")
            seen.add(key)


class InjectionTest(unittest.TestCase):
    def injection(self, **expect):
        return gate.Injection(id="i", config="c", patch="p.patch", file="a.c", line=10,
                              expect=expect or {"ids": ["use-after-free"], "severity": "any"}, mode="unit")

    def test_diagnostic_expectations(self):
        inj = self.injection(ids=["use-after-free"], severity="error")
        self.assertTrue(gate.diagnostic_matches(diag(line=10), inj, False))
        self.assertFalse(gate.diagnostic_matches(diag(line=11), inj, False))
        self.assertFalse(gate.diagnostic_matches(diag(line=10, id_="double-free"), inj, False))
        self.assertFalse(gate.diagnostic_matches(diag(line=10, severity="warning"), inj, False))
        self.assertTrue(gate.diagnostic_matches(diag(line=10, severity="warning"), inj, True))  # legacy
        trap_only = self.injection(trap="index", run="./t")
        self.assertTrue(gate.diagnostic_matches(diag(line=10, id_="out-of-bounds"), trap_only, False))
        self.assertFalse(gate.diagnostic_matches(diag(line=10, id_="null-dereference"), trap_only, False))

    def test_trap_expectations(self):
        report = {"template": "index", "file": "a.c", "line": 10, "col": 3}
        self.assertTrue(gate.report_matches(report, self.injection(trap="index", run="./t")))
        self.assertFalse(gate.report_matches(report, self.injection(trap="nonnull", run="./t")))
        self.assertFalse(gate.report_matches({**report, "line": 9}, self.injection(trap="index", run="./t")))
        self.assertTrue(gate.report_matches(report, self.injection(ids=["out-of-bounds"])))
        self.assertFalse(gate.report_matches(report, self.injection(ids=["use-after-free"])))
        self.assertTrue(gate.report_matches({**report, "template": "violation"}, self.injection(ids=["x"])))

    def test_repository_injections(self):
        manifest = gate.load_manifest(CORPUS / "manifest.json", CORPUS / "support")
        injections = gate.load_injections(CORPUS / "injections" / "injections.json", manifest)
        self.assertGreaterEqual(len(injections), 28)
        projects = {manifest.config(i.config).project.name for i in injections}
        self.assertEqual(projects, {p.name for p in manifest.projects})
        required = {i.id: i for i in injections if i.required}
        self.assertEqual(set(required), {"lua-uaf-luah-free", "lua-df-freeproto"})
        for inj in required.values():
            self.assertEqual(inj.mode, "whole-program")
            self.assertEqual(inj.config, "lua")
        for inj in injections:
            patch = (CORPUS / "injections" / inj.patch).read_text()
            self.assertIn("INJECTED", patch)
            self.assertIn(f"+++ b/{inj.file}", patch)

    def test_patches_apply_to_the_checkouts(self):
        workdir = ROOT / "build" / "corpus"
        manifest = gate.load_manifest(CORPUS / "manifest.json", CORPUS / "support")
        injections = gate.load_injections(CORPUS / "injections" / "injections.json", manifest)
        checked = 0
        for inj in injections:
            checkout = workdir / manifest.config(inj.config).project.name
            if not (checkout / inj.file).is_file():
                continue
            with tempfile.TemporaryDirectory() as directory:
                copy = Path(directory) / "src"
                copy.mkdir()
                target = copy / inj.file
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((checkout / inj.file).read_bytes())
                self.assertEqual(gate.apply_patch(CORPUS / "injections" / inj.patch, copy), "", inj.id)
                self.assertEqual(gate.check_injected_line(copy, inj), "", inj.id)
                checked += 1
        if not checked:
            self.skipTest("no corpus checkouts under build/corpus")


FAKE_WEAVEC = r'''#!PYTHON
"""A stand-in for weavec and weavec-cc: diagnostics from the sources, ledgers from FAKE_*."""
import json, os, sys
args = sys.argv[1:]
files = [a for a in args if a.endswith(".c") and not a.startswith("-")]
if "--" in args:
    files = [a for a in args[:args.index("--")] if a.endswith(".c")]
ledger = next((a.split("=", 1)[1] for a in args if a.startswith(("-fweavec-ledger=", "--ledger="))), None)
stats = next((a.split("=", 1)[1] for a in args if a.startswith(("-fweavec-analysis-stats=", "--analysis-stats="))), None)
status = 0
diagnostics = []
for f in files:
    for number, line in enumerate(open(f), 1):
        if "BUG" in line:
            severity = "error" if "BUG!" in line else "warning"
            print(f"{os.path.abspath(f)}:{number}:1: {severity}: bad thing [weavec::double-free]", file=sys.stderr)
            diagnostics.append({"id": "double-free", "severity": severity,
                                "certainty": "definite" if severity == "error" else "possible",
                                "facet": "temporal", "message": "bad thing", "file": f, "line": number,
                                "column": 1, "function": "f", "fingerprint": f"fp-{os.path.basename(f)}-{number}"})
            if severity == "error":
                status = 1
if ledger:
    extra = int(os.environ.get("FAKE_UNRESOLVED", "0"))
    facets = {k: {o: 0 for o in ("proven", "checked", "violation", "unresolved", "trusted")}
              for k in ("spatial", "null", "temporal", "assertion")}
    facets["null"]["proven"] = 8
    facets["null"]["unresolved"] = 2 + extra
    summary = {"sites": 10 + extra, "facets": facets, "errors": sum(d["severity"] == "error" for d in diagnostics),
               "warnings": sum(d["severity"] == "warning" for d in diagnostics), "functions": 3, "overBudget": []}
    with open(ledger, "w") as out:
        json.dump({"schema": "weavec-ledger", "version": 1, "scope": "program" if "--whole-program" in args else "unit",
                   "summary": summary, "units": [], "diagnostics": diagnostics}, out)
if stats:
    with open(stats, "w") as out:
        json.dump({"version": 1, "counters": {"block_transfers": 100}, "final": True}, out)
sys.exit(status)
'''


class EndToEndTest(unittest.TestCase):
    """The gate against a local git repository and fake tools."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="weavec-corpus-gate-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.upstream = self.root / "upstream"
        self.upstream.mkdir()
        (self.upstream / "a.c").write_text("int a(void) { return 0; } /* BUG! */\n")
        (self.upstream / "b.c").write_text("int b(void) { return 1; }\n/* BUG */\n")
        env = {**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.com",
               "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.com"}
        for cmd in (["git", "init", "--quiet"], ["git", "add", "."], ["git", "commit", "--quiet", "-m", "x"]):
            subprocess.run(cmd, cwd=self.upstream, check=True, env=env)
        self.sha = subprocess.run(["git", "rev-parse", "HEAD"], cwd=self.upstream, check=True,
                                  capture_output=True, text=True).stdout.strip()
        self.tool = self.root / "fake-weavec"
        self.tool.write_text(FAKE_WEAVEC.replace("#!PYTHON", f"#!{sys.executable}"))
        self.tool.chmod(0o755)
        manifest = {"schema": "weavec-corpus-manifest", "version": 1, "gates": {},
                    "projects": [{"name": "proj", "url": str(self.upstream), "sha": self.sha, "support": [],
                                  "configs": [
                                      {"name": "one", "compile": {"files": ["*.c"], "args": ["-I."]}},
                                      {"name": "whole", "compile": {"files": ["*.c"], "args": []},
                                       "wholeProgram": True}]}]}
        self.manifest = self.root / "manifest.json"
        self.manifest.write_text(json.dumps(manifest))
        self.expected = self.root / "expected.json"
        self.triage = self.root / "triage.json"
        self.workdir = self.root / "work"

    def run_gate(self, *extra):
        argv = ["--manifest", str(self.manifest), "--expected", str(self.expected), "--triage", str(self.triage),
                "--workdir", str(self.workdir), "--support-dir", str(self.root), "--jobs", "2",
                "--weavec", str(self.tool), "--weavec-cc", str(self.tool), *extra]
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = gate.main(argv)
        return status, out.getvalue() + err.getvalue()

    def test_legacy_baseline_round_trip(self):
        status, output = self.run_gate("--quick", "--legacy")
        self.assertEqual(status, 1, output)  # clones, then finds no baseline
        self.assertIn("no legacy baseline", output)
        self.assertTrue((self.workdir / "proj" / "a.c").exists())
        status, output = self.run_gate("--quick", "--legacy", "--update")
        self.assertEqual(status, 0, output)
        recorded = json.loads(self.expected.read_text())["legacy"]["quick"]
        self.assertEqual(recorded["configs"]["one"]["units"], 2)
        self.assertEqual(recorded["configs"]["whole"]["units"], 1)
        self.assertEqual(recorded["totals"]["bugClaims"], 4)
        status, output = self.run_gate("--quick", "--legacy", "--json", str(self.root / "r.json"))
        self.assertEqual(status, 0, output)
        results = json.loads((self.root / "r.json").read_text())
        self.assertEqual(results["legacyTotals"]["bugClaims"], 4)
        # A changed checkout is refused rather than analysed.
        (self.workdir / "proj" / "b.c").write_text("/* BUG */\n/* BUG */\n")
        status, output = self.run_gate("--quick", "--legacy")
        self.assertEqual(status, 2, output)
        self.assertIn("modified", output)

    def test_compare_golden(self):
        golden = self.root / "golden"
        golden.mkdir()
        (golden / "weavec").symlink_to(self.tool)
        status, output = self.run_gate("--compare-golden", "--golden-dir", str(golden))
        self.assertEqual(status, 0, output)
        other = self.root / "other-weavec"
        other.write_text(FAKE_WEAVEC.replace("#!PYTHON", f"#!{sys.executable}").replace("bad thing", "worse thing"))
        other.chmod(0o755)
        argv_status, output = self.run_gate("--compare-golden", "--golden-dir", str(golden), "--weavec", str(other))
        self.assertEqual(argv_status, 1, output)
        self.assertIn("differ from the golden run", output)

    def test_ratchet_and_triage(self):
        status, output = self.run_gate("--quick", "--update")
        # Recorded, but the findings are untriaged.
        self.assertEqual(status, 1, output)
        self.assertIn("untriaged definite double-free", output)
        self.assertIn("untriaged possible double-free", output)
        platform = gate.platform_key()
        configs = json.loads(self.expected.read_text())["platforms"][platform]["configs"]
        self.assertEqual(configs["one"]["units"]["errors"], 1)
        self.assertEqual(configs["whole"]["program"]["warnings"], 1)
        entries = []
        for name in ("one", "whole"):
            entries.append({"fingerprint": "fp-a.c-1", "config": name, "id": "double-free",
                            "certainty": "definite", "file": "a.c", "line": 1, "verdict": "true", "note": "n"})
            entries.append({"fingerprint": "fp-b.c-2", "config": name, "id": "double-free",
                            "certainty": "possible", "file": "b.c", "line": 2, "verdict": "true", "note": "n"})
        self.triage.write_text(json.dumps({"schema": "weavec-corpus-triage", "version": 1, "entries": entries}))
        status, output = self.run_gate("--quick")
        self.assertEqual(status, 0, output)
        os.environ["FAKE_UNRESOLVED"] = "3"
        try:
            status, output = self.run_gate("--quick", "--json", str(self.root / "r.json"))
        finally:
            del os.environ["FAKE_UNRESOLVED"]
        self.assertEqual(status, 1, output)
        self.assertIn("ratchet regressions", output)
        # Measurements from elsewhere can be recorded without rerunning.
        status, output = self.run_gate("--update-from", str(self.root / "r.json"))
        self.assertEqual(status, 0, output)
        configs = json.loads(self.expected.read_text())["platforms"][platform]["configs"]
        self.assertEqual(configs["one"]["units"]["ledger"]["unresolved"], 10)


FAKE_CC = r"""#!PYTHON
# A stand-in for weavec-cc in builds: ledgers and diagnostics, then the system cc.
# With FAKE_TRAP set, trap and verify builds get -DWEAVEC_FAKE_TRAP and report
# builds -DWEAVEC_FAKE_REPORT, which the test program turns into a trap or a
# report-mode line.
#
# The real weavec-cc is a Clang driver, so corpus-gate.py hands it Clang's own
# options (-ferror-limit=) alongside the -fweavec ones. The system cc here is
# whatever /usr/bin/cc is: Clang on macOS but GCC on Linux, which errors out on
# an unknown -f option. This stand-in therefore consumes the Clang-only options
# as the real driver would, and forwards only what any C compiler accepts.
import json, os, sys
args = sys.argv[1:]
driver_only = ("-fweavec", "-fno-weavec", "-Wno-error=weavec", "-ferror-limit=")
checks, ledger, rest = "trap", None, []
for a in args:
    if a.startswith("-fweavec-checks="):
        checks = a.split("=", 1)[1]
    elif a.startswith("-fweavec-ledger="):
        ledger = a.split("=", 1)[1]
    elif not a.startswith(driver_only):
        rest.append(a)
sources = [a for a in rest if a.endswith(".c")]
out = rest[rest.index("-o") + 1] if "-o" in rest else "a.out"
diagnostics = []
for f in sources:
    for number, line in enumerate(open(f), 1):
        if "BUG" in line:
            print(f"{f}:{number}:1: warning: bad thing [weavec::double-free]", file=sys.stderr)
            diagnostics.append({"id": "double-free", "severity": "warning", "certainty": "possible",
                                "facet": "temporal", "message": "bad thing", "file": f, "line": number,
                                "column": 1, "function": "f", "fingerprint": f"fp-{os.path.basename(f)}-{number}"})
if ledger:
    target = ledger
    if ledger.endswith("/"):
        target = os.path.join(ledger, os.path.basename(out) + ".ledger.json")
    facets = {k: {o: 0 for o in ("proven", "checked", "violation", "unresolved", "trusted")}
              for k in ("spatial", "null", "temporal", "assertion")}
    facets["null"]["checked"] = 1
    summary = {"sites": 1, "facets": facets, "errors": 0, "warnings": len(diagnostics), "functions": 1,
               "overBudget": []}
    with open(target, "w") as handle:
        json.dump({"schema": "weavec-ledger", "version": 1, "scope": "unit" if "-c" in rest else "program",
                   "summary": summary, "units": [{"source": s} for s in sources],
                   "diagnostics": diagnostics}, handle)
if os.environ.get("FAKE_TRAP"):
    rest.append("-DWEAVEC_FAKE_REPORT" if checks == "report" else "-DWEAVEC_FAKE_TRAP")
os.execvp("cc", ["cc", *rest])
"""

PROGRAM = """#include <stdio.h>
int helper(int i);
int main(void) {
#ifdef WEAVEC_FAKE_TRAP
    __builtin_trap();
#endif
#ifdef WEAVEC_FAKE_REPORT
    fprintf(stderr, "weavec: runtime check failed: index at prog.c:9:5\\n");
#endif
    return helper(-1);
}
"""


class FullEndToEndTest(unittest.TestCase):
    """--full and --inject with a fake weavec-cc that compiles with the system cc."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="weavec-corpus-full-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        upstream = self.root / "upstream"
        upstream.mkdir()
        (upstream / "a.c").write_text("int helper(int i) {\n    return i + 1; /* BUG */\n}\n")
        (upstream / "prog.c").write_text(PROGRAM)
        env = {**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.com",
               "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.com"}
        for cmd in (["git", "init", "--quiet"], ["git", "add", "."], ["git", "commit", "--quiet", "-m", "x"]):
            subprocess.run(cmd, cwd=upstream, check=True, env=env)
        sha = subprocess.run(["git", "rev-parse", "HEAD"], cwd=upstream, check=True, capture_output=True,
                             text=True).stdout.strip()
        self.tool = self.root / "fake-weavec"
        self.tool.write_text(FAKE_WEAVEC.replace("#!PYTHON", f"#!{sys.executable}"))
        self.cc = self.root / "fake-weavec-cc"
        self.cc.write_text(FAKE_CC.replace("#!PYTHON", f"#!{sys.executable}"))
        for tool in (self.tool, self.cc):
            tool.chmod(0o755)
        manifest = {"schema": "weavec-corpus-manifest", "version": 1, "gates": {"G12": {"minReportedShare": 0.9}},
                    "projects": [{"name": "proj", "url": str(upstream), "sha": sha, "support": [], "configs": [
                        {"name": "built", "compile": {"files": ["a.c"], "args": []},
                         "build": ['"$CC" -c a.c -o a.o', '"$CC" prog.c a.o -o prog'], "test": ["./prog"]}]}]}
        self.manifest = self.root / "manifest.json"
        self.manifest.write_text(json.dumps(manifest))
        injections = self.root / "injections"
        (injections / "proj").mkdir(parents=True)
        self.write_patch(injections / "proj" / "static.patch", upstream / "a.c", "a.c",
                         "    return i + 1; /* BUG */\n", "    return i + 1; /* BUG */\n    i++; /* BUG INJECTED */\n")
        report = '    fprintf(stderr, "weavec: runtime check failed: index at prog.c:10:5\\n"); /* INJECTED */\n'
        self.write_patch(injections / "proj" / "trap.patch", upstream / "prog.c", "prog.c",
                         "#endif\n    return helper(-1);\n", "#endif\n" + report + "    return helper(-1);\n")
        (injections / "injections.json").write_text(json.dumps({
            "schema": "weavec-corpus-injections", "version": 1, "injections": [
                {"id": "static", "config": "built", "patch": "proj/static.patch", "file": "a.c", "line": 3,
                 "expect": {"ids": ["double-free"], "severity": "any"}, "mode": "unit"},
                {"id": "trap", "config": "built", "patch": "proj/trap.patch", "file": "prog.c", "line": 10,
                 "expect": {"trap": "index", "run": "./prog"}, "mode": "unit"}]}))
        self.injections = injections / "injections.json"
        self.expected = self.root / "expected.json"
        self.triage = self.root / "triage.json"

    @staticmethod
    def write_patch(path: Path, source: Path, name: str, old: str, new: str) -> None:
        import difflib
        text = source.read_text()
        assert text.count(old) == 1
        patched = text.replace(old, new)
        path.write_text("".join(difflib.unified_diff(text.splitlines(keepends=True), patched.splitlines(keepends=True),
                                                     fromfile=f"a/{name}", tofile=f"b/{name}")))

    def triage_entries(self, *extra):
        entries = [{"fingerprint": "fp-a.c-2", "config": "built", "id": "double-free", "certainty": "possible",
                    "file": "a.c", "line": 2, "verdict": "true", "note": "n"}, *extra]
        self.triage.write_text(json.dumps({"schema": "weavec-corpus-triage", "version": 1, "entries": entries}))

    def run_gate(self, *extra, trap=False):
        argv = ["--manifest", str(self.manifest), "--expected", str(self.expected), "--triage", str(self.triage),
                "--injections", str(self.injections), "--workdir", str(self.root / "work"),
                "--support-dir", str(self.root), "--jobs", "2", "--weavec", str(self.tool),
                "--weavec-cc", str(self.cc), "--cc", "cc", *extra]
        out, err = io.StringIO(), io.StringIO()
        if trap:
            os.environ["FAKE_TRAP"] = "1"
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                status = gate.main(argv)
        finally:
            os.environ.pop("FAKE_TRAP", None)
        return status, out.getvalue() + err.getvalue()

    def test_full_mode(self):
        status, output = self.run_gate("--full", "--update")
        self.assertEqual(status, 1, output)
        self.assertIn("untriaged possible double-free", output)
        # --update writes nothing once an analysis, build or run has failed, and
        # the status is 1 either way. Assert the write here, with the gate's log:
        # without it the read below dies on a FileNotFoundError naming no reason.
        self.assertTrue(self.expected.exists(),
                        f"--update wrote no {self.expected.name}; gate output:\n{output}")
        configs = json.loads(self.expected.read_text())["platforms"][gate.platform_key()]["configs"]
        self.assertEqual(configs["built"]["traps"], 0)
        self.assertEqual(configs["built"]["units"]["warnings"], 1)
        self.triage_entries()
        status, output = self.run_gate("--full", "--json", str(self.root / "r.json"))
        self.assertEqual(status, 0, output)
        results = json.loads((self.root / "r.json").read_text())
        self.assertEqual(results["gates"]["G11"]["status"], "pass")
        self.assertEqual(results["gates"]["G12"]["detail"]["reported"], 2)
        self.assertEqual({i["id"]: i["via"] for i in results["injections"]},
                         {"static": ["diagnostic"], "trap": ["trap"]})
        # A check that fails in the test suite is a trap (G11) and a ratchet regression.
        status, output = self.run_gate("--full", trap=True)
        self.assertEqual(status, 1, output)
        self.assertIn("check failed in the test suite: index at prog.c:9:5", output)
        self.assertIn("ratchet regressions: built.traps: 0 -> 1", output)
        # At the site of a triaged-true definite error, it is a true positive.
        self.triage_entries({"fingerprint": "fp-x", "config": "built", "id": "out-of-bounds",
                             "certainty": "definite", "file": "prog.c", "line": 9, "verdict": "true", "note": "n"})
        status, output = self.run_gate("--full", trap=True)
        self.assertEqual(status, 0, output)
        self.assertIn("trapped only at triaged-true definite errors", output)

    def test_reference_only(self):
        # The synthetic trap injection only prints a report line; ASan has nothing to find in it.
        data = json.loads(self.injections.read_text())
        data["injections"] = [i for i in data["injections"] if i["id"] == "static"]
        self.injections.write_text(json.dumps(data))
        status, output = self.run_gate("--full", "--reference-only", "--json", str(self.root / "r.json"))
        self.assertEqual(status, 0, output)
        results = json.loads((self.root / "r.json").read_text())
        build = results["configs"]["built"]["builds"]["reference"]
        self.assertTrue(build["built"])
        self.assertTrue(build["testsPassed"])
        self.assertNotIn("measured", results["configs"]["built"])


if __name__ == "__main__":
    unittest.main()
