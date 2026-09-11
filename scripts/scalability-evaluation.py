#!/usr/bin/env python3
"""RFC 0020: sequential, reproducible ordinary and checked cost observations.

Run after builds, tests and profiling have finished. This script deliberately
runs one checker process at a time. It preserves every report and raw corpus
result; its summary never substitutes a timeout for a completed report.
"""
import argparse
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import statistics
import shutil
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location('checked_report', ROOT / 'scripts/checked-report.py')
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)
MANIFEST = ROOT / 'scripts/corpus/rfc0015.json'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def canonical_digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def report_summary(path):
    if not path.is_file():
        return dict(present=False)
    metadata, units, functions = {}, {}, {}
    selected = complete = 0
    complete_functions, limited_functions = [], []
    iteration_origins, complete_with_iteration_limit = set(), []

    def consume(index, function):
        nonlocal selected, complete
        functions.setdefault(index, []).append(canonical_digest(function))
        selected += bool(function['selected'])
        complete += bool(function['selected'] and function['complete'])
        if function['selected'] and function['complete']:
            complete_functions.append([index, function['name']])
        if function['limited']:
            limited_functions.append([index, function['name'], function['complete']])
        for obligation in function['obligations']:
            if 'iteration limit reached' not in obligation['reason']:
                continue
            location = obligation['location']
            iteration_origins.add((location['file'], location['line'], location['column'],
                                   obligation['reason']))
            if function['complete']:
                complete_with_iteration_limit.append([index, function['name']])

    # Version 3 may put shared tables after references. Retain those tables
    # and re-read function references, never all expanded functions together.
    with path.open(encoding='utf-8') as stream:
        for kind, index, key, value in REPORT.report_events(stream):
            if kind == 'root':
                metadata[key] = value
            elif kind == 'unit':
                units.setdefault(index, {})[key] = value
            elif value['obligations'] and type(value['obligations'][0]) is int:
                continue
            else:
                consume(index, value)
    version = metadata.get('version')
    if version not in (2, 3):
        raise ValueError('unsupported checked report version')
    if version == 3:
        tables = REPORT.CompactTables(metadata)
        # Preserve function ordering, including empty-obligation records.
        # All version-3 records are re-read below, one at a time.
        functions.clear()
        selected = complete = 0
        complete_functions.clear()
        limited_functions.clear()
        iteration_origins.clear()
        complete_with_iteration_limit.clear()
        with path.open(encoding='utf-8') as stream:
            for kind, index, _, value in REPORT.report_events(stream):
                if kind == 'function':
                    consume(index, tables.expand_function(value))
        for key in ('strings', 'locations', 'call_paths', 'obligation_records', 'obligation_fields'):
            metadata.pop(key)
    metadata['version'] = 2
    metadata['units'] = [dict(units[index], functions=functions.get(index, [])) for index in sorted(units)]
    # A versioned Merkle digest covers every field and ordered function digest.
    # It is equal iff expanded canonical contents are equal (modulo SHA-256).
    for entry in complete_functions + limited_functions + complete_with_iteration_limit:
        entry[0] = units[entry[0]]['source']
    return dict(present=True, bytes=path.stat().st_size, version=version,
                sha256=digest(path), semantic_hash_format='ordered-function-sha256-v1',
                semantic_sha256=canonical_digest(metadata), selected=selected,
                complete_selected=complete, complete_functions=sorted(complete_functions),
                limited_functions=sorted(limited_functions),
                iteration_origins=sorted(iteration_origins),
                complete_with_iteration_limit=sorted(complete_with_iteration_limit),
                totals=metadata['totals'])


def checked_coverage_valid(units, report):
    """RFC 0020 accepts explicit incomplete coverage, never missing results.

    Preserve corpus.py's stricter ordinary classification in the raw artifact.
    Only its exact function-limit case can represent valid checked coverage.
    Whole-program nonconvergence, crashes and timeouts remain invalid.
    """
    if not report.get('present') or not units:
        return False
    for unit in units:
        if unit['clang_errors'] or unit['exit_code'] not in (0, 1):
            return False
        failure = unit['failure']
        if not failure:
            continue
        if failure != 'analysis reached an iteration limit':
            return False
        messages = [d['message'] for d in unit['diagnostics']
                    if d['id'] == 'analysis-incomplete' and 'iteration limit reached' in d['message']]
        if not messages or any(message != 'analysis is incomplete: function dataflow iteration limit reached'
                               for message in messages):
            return False
        if (not report.get('iteration_origins') or report.get('complete_with_iteration_limit') or
                any(entry[2] for entry in report.get('limited_functions', []))):
            return False
    return True


def archive_report(path, expected):
    """Preserve large reports after measurement with verified decoded bytes."""
    archive = Path(str(path) + '.gz')
    temporary = Path(str(archive) + '.tmp')
    with path.open('rb') as source, temporary.open('wb') as destination:
        with gzip.GzipFile(filename='', mode='wb', fileobj=destination,
                           compresslevel=1, mtime=0) as compressed:
            shutil.copyfileobj(source, compressed, 1024 * 1024)
    with gzip.open(temporary, 'rb') as decoded:
        actual = hashlib.file_digest(decoded, 'sha256').hexdigest()
    if actual != expected:
        raise ValueError('compressed report failed verification: ' + str(path))
    temporary.replace(archive)
    result = dict(path=str(archive), bytes=archive.stat().st_size,
                  sha256=digest(archive), decoded_sha256=actual)
    path.unlink()
    return result


def run_corpus(command, *, cwd, stdout, timeout):
    """Let corpus.py reap its detached checker before stopping the runner."""
    with subprocess.Popen(command, cwd=cwd, stdout=stdout,
                          stderr=subprocess.STDOUT) as process:
        try:
            process.wait(timeout=timeout)
        except (KeyboardInterrupt, subprocess.TimeoutExpired):
            # subprocess.run kills its immediate child on interruption. That
            # prevents corpus.py from cleaning up the checker's own session.
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            raise
    return process


def observation(binary, output, name, projects, *, checked=False, cache=False,
                compact=False, timeout=600, archive_reports=False):
    destination = output / name
    destination.mkdir(parents=True, exist_ok=True)
    result = dict(name=name, binary_sha256=digest(binary), projects=[])
    for project in projects:
        stem = destination / project['name']
        corpus = Path(str(stem) + '.corpus.json')
        report = Path(str(stem) + '.report.json')
        stats = Path(str(stem) + '.stats.json')
        # Never mistake a surviving report from an earlier attempt for output.
        for artifact in (corpus, report, stats):
            artifact.unlink(missing_ok=True)
        command = ['python3', str(ROOT / 'scripts/corpus.py'), '--weavec', str(binary),
                   '--manifest', str(MANIFEST), '--only', project['name'],
                   '--timeout', str(timeout), '--measure-memory', '--json', str(corpus)]
        if checked:
            command += ['--weavec-arg=--checked', f'--weavec-arg=--checked-report={report}',
                        f'--weavec-arg=--analysis-stats={stats}']
        if compact:
            command.append('--weavec-arg=--checked-report-format=compact')
        if cache:
            command.append(f'--weavec-arg=--analysis-cache={output / "cache" / project["name"]}')
        started = time.time()
        with Path(str(stem) + '.log').open('w') as log:
            process = run_corpus(command, cwd=ROOT, stdout=log, timeout=timeout + 60)
        entry = dict(project=project['name'], command=command, started=started,
                     elapsed=time.time() - started, returncode=process.returncode)
        if corpus.exists():
            data = json.loads(corpus.read_text())
            entry['measurement'] = data['summary']['projects'][project['name']]
            entry['commit'] = data['commits'][project['name']]
        if checked:
            entry['report'] = report_summary(report)
            entry['valid_checked_coverage'] = corpus.exists() and checked_coverage_valid(data['units'], entry['report'])
            entry['cold_time_gate'] = entry.get('measurement', {}).get('seconds', float('inf')) <= 600
            if stats.exists():
                entry['stats'] = json.loads(stats.read_text())
            if archive_reports and entry['report']['present']:
                entry['report']['archive'] = archive_report(report, entry['report']['sha256'])
        result['projects'].append(entry)
        (destination / 'observation.json').write_text(json.dumps(result, indent=2) + '\n')
        measurement = entry.get('measurement', {})
        print(name, project['name'], f'{measurement.get("seconds", "missing")}s',
              f'failures={measurement.get("failures", "missing")}', flush=True)
    return result


def ordinary_totals(observations):
    totals = []
    peaks = []
    valid = True
    for run in observations:
        measurements = [entry.get('measurement', {}) for entry in run['projects']]
        valid &= all(m.get('failures') == 0 and m.get('clang_errors') == 0 for m in measurements)
        totals.append(sum(m.get('seconds', 0) for m in measurements))
        peaks.append(max((m.get('peak_rss_bytes', 0) for m in measurements), default=0))
    return dict(valid=valid, seconds=totals, peak_rss_bytes=peaks,
                median_seconds=statistics.median(totals), median_peak_rss_bytes=statistics.median(peaks))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--weavec', type=Path, required=True)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--phase', choices=['ordinary', 'checked', 'cache', 'all'], default='all')
    parser.add_argument('--only', action='append', default=[])
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--timeout', type=float, default=600)
    parser.add_argument('--archive-reports', action='store_true',
                        help='checksum-verify and compress reports after measurement')
    args = parser.parse_args()
    if args.repetitions < 1:
        parser.error('--repetitions must be positive')
    binary = args.weavec.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    projects = json.loads(MANIFEST.read_text())['projects']
    if args.only:
        projects = [project for project in projects if project['name'] in args.only]
    if not projects:
        parser.error('no projects selected')
    summary = dict(version=1, manifest_sha256=digest(MANIFEST), binary_sha256=digest(binary),
                   phase=args.phase, observations=[])

    def record(name, **options):
        result = observation(binary, output, name, projects, timeout=args.timeout,
                             archive_reports=args.archive_reports, **options)
        summary['observations'].append(result)
        save()
        return result

    def save():
        (output / f'{args.phase}-results.json').write_text(json.dumps(summary, indent=2) + '\n')

    passed = True
    if args.phase in ('ordinary', 'all'):
        if not args.baseline:
            parser.error('--baseline is required for ordinary comparisons')
        baseline = args.baseline.resolve()
        before = [observation(baseline, output, f'baseline-{i + 1}', projects, timeout=args.timeout)
                  for i in range(args.repetitions)]
        summary['baseline'] = ordinary_totals(before)
        summary['observations'] += before
        after = [record(f'final-{i + 1}') for i in range(args.repetitions)]
        summary['final'] = ordinary_totals(after)
        summary['time_ratio'] = summary['final']['median_seconds'] / summary['baseline']['median_seconds']
        summary['memory_ratio'] = summary['final']['median_peak_rss_bytes'] / summary['baseline']['median_peak_rss_bytes']
        passed &= summary['baseline']['valid'] and summary['final']['valid']
        passed &= summary['time_ratio'] <= 1.1 and summary['memory_ratio'] <= 1.1
    if args.phase in ('checked', 'all'):
        run = record('checked-cold', checked=True)
        passed &= all(entry['valid_checked_coverage'] and entry['cold_time_gate'] for entry in run['projects'])
    if args.phase in ('cache', 'all'):
        # A fresh directory is required for an actual cold-cache observation.
        cache = output / 'cache'
        if cache.exists() and any(cache.rglob('*.wcache')):
            parser.error('cache already has records; choose a new --output directory for the cache phase')
        cold = record('cache-cold', checked=True, cache=True)
        warm = record('cache-warm', checked=True, cache=True, compact=True)
        checks = []
        for before, after in zip(cold['projects'], warm['projects']):
            a, b = before['report'], after['report']
            counters = after.get('stats', {}).get('counters', {})
            check = dict(project=before['project'], equivalent=a.get('semantic_sha256') == b.get('semantic_sha256'),
                         report_reduction=a.get('bytes', 0) / max(1, b.get('bytes', 0)),
                         function_analyses=counters.get('function_analyses', 0),
                         cache_hits=counters.get('cache_hits', 0))
            check['passed'] = bool(a['present'] and b['present'] and check['equivalent']
                                   and check['function_analyses'] == 0 and check['cache_hits'] > 0)
            check['passed'] &= all(entry['valid_checked_coverage'] for entry in (before, after))
            check['passed'] &= before['cold_time_gate']
            if check['project'] in ('cJSON-program', 'linenoise-program'):
                check['passed'] &= check['report_reduction'] >= 5
            passed &= check['passed']
            checks.append(check)
        summary['cache_checks'] = checks
    summary['passed'] = passed
    save()
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
