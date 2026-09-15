#!/usr/bin/env python3
"""RFC 0027 recursive ownership source, object, cache and concrete-heap checks."""
import argparse
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / 'test/evaluation/rfc0027'
SPEC = importlib.util.spec_from_file_location('checked_evaluation', ROOT / 'scripts/checked-evaluation.py')
EVALUATION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EVALUATION)
REPORT_SPEC = importlib.util.spec_from_file_location('checked_report', ROOT / 'scripts/checked-report.py')
REPORT = importlib.util.module_from_spec(REPORT_SPEC)
REPORT_SPEC.loader.exec_module(REPORT)

def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify(directory):
    for name, expected in json.loads((directory / 'frozen-sha256.json').read_text()).items():
        if digest(directory / name) != expected:
            raise ValueError('frozen input differs: ' + str(directory / name))


def invoke(args, label, command, cwd=None):
    # A repeated output directory must never supply a previous run's proof
    # when this invocation fails before writing its requested artifacts.
    executable = str(Path(command[0]).resolve())
    if executable in args.binary_sha256 and digest(Path(executable)) != args.binary_sha256[executable]:
        raise ValueError('executable changed during evaluation: ' + executable)
    for argument in command:
        if argument.startswith(('--checked-report=', '-fweavec-checked-report=',
                                '--analysis-stats=')):
            Path(argument.split('=', 1)[1]).unlink(missing_ok=True)
    start = time.monotonic()
    run = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=args.timeout)
    (args.output / (label + '.log')).write_text(run.stdout + run.stderr)
    return run, time.monotonic() - start


def document(path):
    return json.loads(path.read_text()) if path.exists() else {}


def archive(path):
    if not path.is_file():
        return None
    expected = digest(path)
    compressed = Path(str(path) + '.gz')
    with path.open('rb') as raw, gzip.open(compressed, 'wb', compresslevel=1) as output:
        shutil.copyfileobj(raw, output)
    with gzip.open(compressed, 'rb') as decoded:
        assert hashlib.file_digest(decoded, 'sha256').hexdigest() == expected
    path.unlink()
    return dict(path=str(compressed), decoded_sha256=expected, sha256=digest(compressed))


def verify_upstream():
    identity = document(FIXTURES / 'upstream/upstream-identity.json')
    upstream = ROOT / 'build/corpus/cJSON-program'
    revision = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse', 'HEAD'], text=True).strip()
    assert revision == identity['commit'], 'upstream commit differs'
    for file, expected in identity['files'].items():
        assert digest(upstream / file) == expected, 'upstream source changed'
        original = subprocess.check_output(['git', '-C', str(upstream), 'show', revision + ':' + file])
        assert hashlib.sha256(original).hexdigest() == expected, 'inventory differs from pinned source'


def objects(args):
    upstream = args.population == 'upstream-objects'
    directory = FIXTURES / ('upstream' if upstream else 'transport')
    verify(directory)
    if upstream:
        verify_upstream()
    results = []
    for case in document(directory / 'manifest.json')['cases']:
        result = dict(name=case['name'], passed=False, stages=[])
        try:
            with tempfile.TemporaryDirectory(prefix='weavec-recursive-object-') as temporary:
                work = Path(temporary)
                names = case.get('sources', [case.get('source')])
                if not upstream:
                    for name in [*names, 'tree-api.h']:
                        shutil.copy2(directory / name, work / name)
                for index, name in enumerate(names):
                    source_path = directory / name if upstream else work / name
                    label = case['name'] + '-compile-' + str(index)
                    report = args.output / (label + '.json')
                    command = [str(args.cc), '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                               '-fweavec-checked-report=' + str(report)]
                    if 'main(' in source_path.read_text() or 'main (' in source_path.read_text():
                        command.append('-fweavec-checked-function=main')
                    command += ['-c', str(source_path), '-o', str(index) + '.o']
                    run, seconds = invoke(args, label, command, work)
                    result['stages'].append(dict(label=label, returncode=run.returncode, seconds=seconds))
                    if run.returncode:
                        passed, reason = EVALUATION.assess(case, run.returncode, document(report))
                        result.update(passed=passed, reason=reason, rejected_at='compile')
                        result['stages'][-1]['report'] = archive(report)
                        break
                    result['stages'][-1]['report'] = archive(report)
                    metadata = work / (str(index) + '.o.weavec')
                    if not metadata.is_file():
                        raise ValueError('object has no sidecar')
                    result['stages'][-1]['sidecar_sha256'] = digest(metadata)
                else:
                    label = case['name'] + '-link'
                    report = args.output / (label + '.json')
                    command = [str(args.cc), '-fweavec-checked-function=main',
                               '-fweavec-checked-report=' + str(report),
                               *[str(i) + '.o' for i in range(len(names))], '-o', 'client']
                    run, seconds = invoke(args, label, command, work)
                    passed, reason = EVALUATION.assess(case, run.returncode, document(report))
                    if passed and case['expect'] == 'accepted' and not (work / 'client').is_file():
                        passed, reason = False, 'checked link produced no executable'
                    result.update(passed=passed, reason=reason, seconds=seconds, returncode=run.returncode)
                    result['report'] = archive(report)
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            result['reason'] = str(error)
        results.append(result)
        print(('PASS ' if result['passed'] else 'FAIL ') + case['name'] + ': ' + result.get('reason', ''), flush=True)
        save(args, results)
    return results

def cache(args):
    directory = FIXTURES / 'transport'
    verify(directory)
    results = []
    with tempfile.TemporaryDirectory(prefix='weavec-recursive-cache-') as temporary:
        work = Path(temporary)
        sources = ['runtime-good.c', 'tree.c']
        for name in [*sources, 'tree-api.h']:
            shutil.copy2(directory / name, work / name)
        original = (work / 'tree.c').read_text()

        def analyze(label, cached=True, compact=False):
            report = args.output / (label + '.json')
            stats = args.output / (label + '.stats.json')
            command = [str(args.weavec), '--whole-program', '--checked-function=main',
                       '--checked-report=' + str(report), '--analysis-stats=' + str(stats)]
            if cached:
                command.append('--analysis-cache=' + str(work / 'cache'))
            if compact:
                command.append('--checked-report-format=compact')
            command += [str(work / name) for name in sources] + ['--', '-std=c11']
            run, seconds = invoke(args, label, command)
            if run.returncode not in (0, 1) or not document(report):
                raise ValueError('analysis failed: ' + label)
            return run.returncode, REPORT.expand_report(document(report)), run.stderr, document(stats)['counters'], seconds

        cold, warm, uncached = analyze('cold'), analyze('warm'), analyze('uncached', False)
        assert cold[:3] == warm[:3] == uncached[:3], 'cached reports differ'
        assert cold[0] == 0, 'valid caller was rejected'
        assert warm[3].get('cache_hits') == 2 and warm[3].get('function_analyses', 0) == 0
        results.append(dict(name='equivalent-unchanged', passed=True, counters=warm[3]))
        compact = analyze('compact', compact=True)
        assert cold[:3] == compact[:3], 'compact report differs'
        assert compact[3].get('cache_hits') == 2 and compact[3].get('function_analyses', 0) == 0
        results.append(dict(name='equivalent-compact', passed=True, counters=compact[3]))
        changed_source = original.replace('destroy(p->right);', '(void)p->right;')
        assert changed_source != original, 'cache mutation did not change the source'
        (work / 'tree.c').write_text(changed_source)
        changed, fresh = analyze('skipped-child'), analyze('skipped-child-uncached', False)
        assert changed[:3] == fresh[:3] and changed[0] == 1, 'stale complete cleanup reused'
        assert changed[3].get('function_analyses', 0) > 0
        results.append(dict(name='skipped-child-invalidates-output', passed=True, counters=changed[3]))
        (work / 'tree.c').write_text(original)
        for path in (work / 'cache').glob('*.wcache'):
            path.write_bytes(path.read_bytes()[:91])
        changed, fresh = analyze('corrupt'), analyze('corrupt-uncached', False)
        assert changed[:3] == fresh[:3] and changed[0] == 0
        assert changed[3].get('cache_hits', 0) == 0
        results.append(dict(name='corrupt-cache-recomputed', passed=True))
        for index, name in enumerate(sources):
            run, _ = invoke(args, 'object-' + str(index),
                            [str(args.cc), '-std=c11', '-c', name, '-o', str(index) + '.o'], work)
            assert run.returncode == 0, run.stderr

        def link(label):
            report = args.output / (label + '.json')
            return invoke(args, label, [str(args.cc), '-fweavec-checked-function=main',
                          '-fweavec-checked-report=' + str(report), '0.o', '1.o', '-o', 'client'], work)[0]

        assert link('sidecar-good').returncode == 0
        (work / 'tree.c').write_text(original + '\n/* changed binding */\n')
        run = link('sidecar-stale')
        assert run.returncode == 1 and 'source input changed' in run.stderr
        results.append(dict(name='stale-sidecar-rejected', passed=True))
        (work / 'tree.c').write_text(original)
        metadata = work / '1.o.weavec'
        text = metadata.read_text()
        match = re.match(r'weavec-summaries (\d+)\n', text)
        assert match
        old = int(match[1]) - 1
        metadata.write_text(re.sub(r'^weavec-summaries \d+\n', 'weavec-summaries ' + str(old) + '\n', text, count=1))
        run = link('sidecar-old-format')
        assert run.returncode == 1 and 'unsupported format ' + str(old) in run.stderr
        results.append(dict(name='old-sidecar-rejected', passed=True))
    save(args, results)
    return results

def concrete_failure(edges, roots):
    """Interpret allocation identities independently of the abstract domains."""
    active, released = set(), set()
    def destroy(node):
        if node is None:
            return None
        if node in active:
            return 'owning cycle'
        if node in released:
            return 'shared owned allocation'
        active.add(node)
        for child in edges[node]:
            failure = destroy(child)
            if failure:
                return failure
        active.remove(node)
        released.add(node)
        return None
    for root in roots:
        failure = destroy(root)
        if failure:
            return failure
    return None if released == set(range(len(edges))) else 'unreleased allocation'


def source(args):
    directory = FIXTURES if args.population == 'source' else FIXTURES / args.population
    verify(directory)
    cases = document(directory / 'manifest.json')['cases']
    if args.population == 'oracle':
        truth = document(directory / 'concrete-truth.json')['cases']
        assert [c['name'] for c in cases] == [c['name'] for c in truth]
        for case, heap in zip(cases, truth):
            failure = concrete_failure(heap['edges'], heap['roots'])
            assert failure == heap['failure']
            assert case['expect'] == ('rejected' if failure else 'accepted')
            assert digest(directory / case['source']) == heap['sha256']
    if args.population == 'upstream':
        verify_upstream()
    results = []
    for case in cases:
        label = case['name']
        report = args.output / (label + '.json')
        result = dict(name=label, passed=False, expected=case['expect'])
        try:
            sources = [str(directory / name) for name in case.get('sources', [case.get('source')])]
            syntax = subprocess.run([args.clang, '-fsyntax-only', *sources], capture_output=True,
                                    text=True, timeout=args.timeout)
            if syntax.returncode:
                raise ValueError('C syntax failure: ' + syntax.stderr)
            command = [str(args.weavec), '--checked-report=' + str(report)]
            command += ['--checked-function=' + name for name in case.get('functions', [case.get('function')])]
            if len(sources) > 1:
                command.append('--whole-program')
            command += case.get('flags', []) + sources + ['--', '-std=c11']
            run, seconds = invoke(args, label, command)
            actual = document(report)
            passed, reason = EVALUATION.assess(case, run.returncode, actual)
            selected = [f for u in actual.get('units', []) for f in u['functions'] if f['selected']]
            result.update(passed=passed, reason=reason, seconds=seconds,
                          returncode=run.returncode, selected=selected)
            # Keep the full evidence without retaining a gigabyte of repeated
            # upstream reports in memory or in an uncompressed results file.
            result['report'] = archive(report)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            result['reason'] = str(error)
        results.append(result)
        print(('PASS ' if result['passed'] else 'FAIL ') + label + ': ' + result.get('reason', ''), flush=True)
        save(args, results)
    return results


def scaling(args):
    """Vary runtime cardinality while retaining one inductive helper body."""
    original = (FIXTURES / 'runtime-good.c').read_text()
    assert 'make(n)' in original
    case = next(case for case in document(FIXTURES / 'manifest.json')['cases']
                if case['name'] == 'runtime-good')
    results = []
    with tempfile.TemporaryDirectory(prefix='weavec-recursive-scale-') as temporary:
        work = Path(temporary)
        shutil.copy2(FIXTURES / 'tree.h', work / 'tree.h')
        for length in (0, 1, 8, 64, 1024, 1048576):
            source = work / 'client.c'
            source.write_text(original.replace('make(n)', 'make(' + str(length) + 'U)'))
            label = str(length)
            report = args.output / (label + '.json')
            stats = args.output / (label + '.stats.json')
            syntax = subprocess.run([args.clang, '-std=c11', '-fsyntax-only', str(source)],
                                    capture_output=True, text=True, timeout=args.timeout)
            if syntax.returncode:
                raise ValueError('scaling C syntax failure: ' + syntax.stderr)
            command = [str(args.weavec), '--checked-function=main',
                       '--checked-report=' + str(report), '--analysis-stats=' + str(stats),
                       str(source), '--', '-std=c11']
            run, seconds = invoke(args, label, command)
            passed, reason = EVALUATION.assess(case, run.returncode, document(report))
            results.append(dict(name=label, length=length, passed=passed, reason=reason,
                                seconds=seconds, counters=document(stats)['counters'],
                                source_sha256=digest(source)))
            print(('PASS ' if passed else 'FAIL ') + 'runtime length ' + label, flush=True)
            save(args, results)
    # A million-node argument must not produce work proportional to its value.
    large = [row['counters'].get('block_transfers', 0) for row in results if row['length'] >= 8]
    assert min(large) > 0 and max(large) <= 2 * min(large), 'analysis work scales with heap cardinality'
    return results


def save(args, cases):
    (args.output / 'results.json').write_text(json.dumps(dict(
        version=1, rfc='0027', population=args.population, cases=cases,
        executable_sha256=args.binary_sha256.get(str(
            args.cc if args.population.endswith('objects') else args.weavec))), indent=2) + '\n')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--population', choices=['source', 'transport', 'objects', 'cache', 'oracle', 'upstream', 'upstream-objects', 'scaling'], required=True)
    parser.add_argument('--weavec', type=Path, required=True)
    parser.add_argument('--cc', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--clang', default=shutil.which('clang'))
    parser.add_argument('--timeout', type=float, default=600)
    args = parser.parse_args()
    if args.population in ('objects', 'upstream-objects', 'cache') and not args.cc:
        parser.error('this population requires --cc')
    args.weavec = args.weavec.resolve()
    args.cc = args.cc.resolve() if args.cc else None
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    args.binary_sha256 = {str(binary): digest(binary) for binary in (args.weavec, args.cc)
                          if binary and binary.is_file()}
    try:
        verify(FIXTURES)
        results = {'objects': objects, 'upstream-objects': objects,
                   'cache': cache, 'scaling': scaling}.get(args.population, source)(args)
    except (OSError, ValueError, AssertionError, subprocess.SubprocessError) as error:
        save(args, [dict(name='population', passed=False, reason=str(error))])
        parser.error(str(error))
    return 0 if results and all(row['passed'] for row in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
