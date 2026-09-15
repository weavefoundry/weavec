#!/usr/bin/env python3
"""RFC 0028 opaque interfaces: frozen source/object proofs and cache invalidation."""
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
FIXTURES = ROOT / 'test/evaluation/rfc0028'


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), ROOT / 'scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


EVALUATION = load('checked-evaluation')
REPORT = load('checked-report')


def document(path):
    return json.loads(path.read_text()) if path.is_file() else {}


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify(directory):
    for name, expected in document(directory / 'frozen-sha256.json').items():
        if digest(directory / name) != expected:
            raise ValueError('frozen input differs: ' + str(directory / name))


def verify_upstream():
    identity = document(FIXTURES / 'upstream/upstream-identity.json')
    upstream = ROOT / 'build/corpus/cJSON-program'
    revision = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse', 'HEAD'], text=True).strip()
    assert revision == identity['commit'], 'upstream commit differs'
    for name, expected in identity['files'].items():
        original = subprocess.check_output(['git', '-C', str(upstream), 'show', revision + ':' + name])
        assert hashlib.sha256(original).hexdigest() == expected, 'upstream inventory differs'
        assert digest(upstream / name) == expected, 'upstream source changed'


def invoke(args, label, command, cwd=None):
    binary = str(Path(command[0]).resolve())
    if binary in args.hashes:
        assert digest(Path(binary)) == args.hashes[binary], 'executable changed during evaluation'
    for option in command:
        if option.startswith(('--checked-report=', '-fweavec-checked-report=', '--analysis-stats=')):
            Path(option.split('=', 1)[1]).unlink(missing_ok=True)
    start = time.monotonic()
    run = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=args.timeout)
    (args.output / (label + '.log')).write_text(run.stdout + run.stderr)
    return run, time.monotonic() - start


def retain(path):
    if not path.is_file():
        return None
    expected = digest(path)
    target = Path(str(path) + '.gz')
    with path.open('rb') as source, gzip.open(target, 'wb', compresslevel=1) as output:
        shutil.copyfileobj(source, output)
    with gzip.open(target, 'rb') as decoded:
        assert hashlib.file_digest(decoded, 'sha256').hexdigest() == expected
    path.unlink()
    return dict(path=str(target), decoded_sha256=expected, sha256=digest(target))


def save(args, results):
    (args.output / 'results.json').write_text(json.dumps(dict(
        version=1, rfc='0028', population=args.population, binary_sha256=args.hashes,
        cases=results), indent=2) + '\n')


def cases(args):
    upstream = args.population.startswith('upstream')
    regression = args.population.startswith('regression')
    directory = FIXTURES / 'upstream' if upstream else FIXTURES / 'regressions' if regression else FIXTURES
    directories = [directory]
    if regression:
        directories.append(FIXTURES / 'followups')
    if upstream:
        verify_upstream()
    result = []
    for directory in directories:
        verify(directory)
        properties = document(directory / 'rejection-properties.json')
        for entry in document(directory / 'manifest.json')['cases']:
            case = dict(entry)
            if case['expect'] == 'rejected':
                case['reason'] = properties.get(case['name']) or case['reason']
            case['sources'] = [str((directory / name).resolve()) for name in case['sources']]
            result.append(case)
    return result


def assess(case, run, path):
    actual = REPORT.expand_report(document(path))
    passed, reason = EVALUATION.assess(case, run.returncode, actual)
    selected = [fn for unit in actual.get('units', []) for fn in unit.get('functions', []) if fn['selected']]
    return dict(passed=passed, reason=reason, returncode=run.returncode, selected=selected,
                report=retain(path))


def source(args):
    results = []
    args.progress = results
    for case in cases(args):
        row = dict(name=case['name'], expected=case['expect'], passed=False)
        try:
            syntax, _ = invoke(args, case['name'] + '-syntax',
                               [args.clang, '-std=c11', '-fsyntax-only', *case['sources']])
            if syntax.returncode:
                raise ValueError('C syntax failure')
            report = args.output / (case['name'] + '.json')
            command = [str(args.weavec), '--whole-program', '--checked-function=main',
                       '--checked-report=' + str(report), *case['sources'], '--', '-std=c11', '-ferror-limit=0']
            run, seconds = invoke(args, case['name'], command)
            row.update(assess(case, run, report), seconds=seconds)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            row['reason'] = str(error)
        results.append(row)
        print(('PASS ' if row['passed'] else 'FAIL ') + row['name'] + ': ' + row.get('reason', ''), flush=True)
        save(args, results)
    return results


def objects(args):
    results = []
    args.progress = results
    # Compile each unchanged library once. Every client is linked independently.
    with tempfile.TemporaryDirectory(prefix='weavec-interface-objects-') as temporary:
        work = Path(temporary)
        compiled = {}
        for case in cases(args):
            row = dict(name=case['name'], expected=case['expect'], passed=False, stages=[])
            try:
                inputs = []
                for source in case['sources']:
                    if source not in compiled:
                        output = work / (str(len(compiled)) + '.o')
                        label = 'compile-' + str(len(compiled))
                        command = [str(args.cc), '-std=c11', '-c', source, '-o', str(output)]
                        # The primary population starts with ordinary sidecars.
                        # The other object populations also transport inferred,
                        # unselected contracts, as in the existing checked-build
                        # workflow. Only the subsequent link selects main.
                        report = None
                        if args.population != 'objects':
                            report = args.output / (label + '.json')
                            command.append('-fweavec-checked-report=' + str(report))
                        run, seconds = invoke(args, label, command)
                        if run.returncode or not Path(str(output) + '.weavec').is_file():
                            raise ValueError('object compilation failed: ' + run.stderr)
                        if report:
                            actual = REPORT.expand_report(document(report))
                            if not actual.get('invocation_ok') or actual['totals']['selected'] != 0:
                                raise ValueError('invalid unselected compile report')
                        compiled[source] = output
                        row['stages'].append(dict(source=source, seconds=seconds,
                            sidecar_sha256=digest(Path(str(output) + '.weavec')),
                            compile_report=retain(report) if report else None))
                    inputs.append(str(compiled[source]))
                report = args.output / (case['name'] + '.json')
                executable = work / 'client'
                executable.unlink(missing_ok=True)
                run, seconds = invoke(args, case['name'],
                    [str(args.cc), '-fweavec-checked-function=main',
                     '-fweavec-checked-report=' + str(report), *inputs, '-o', str(executable)])
                row.update(assess(case, run, report), seconds=seconds)
                if row['passed'] and case['expect'] == 'accepted' and not executable.is_file():
                    row.update(passed=False, reason='checked link produced no executable')
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                row['reason'] = str(error)
            results.append(row)
            print(('PASS ' if row['passed'] else 'FAIL ') + row['name'] + ': ' + row.get('reason', ''), flush=True)
            save(args, results)
    return results


def cache(args):
    results = []
    args.progress = results
    with tempfile.TemporaryDirectory(prefix='weavec-interface-cache-') as temporary:
        work = Path(temporary)
        for name in ('library.h', 'library.c', 'hooks-good.c'):
            shutil.copy2(FIXTURES / name, work / name)
        implementation = work / 'library.c'
        original = implementation.read_text()

        def analyze(label, cached=True, compact=False):
            report, stats = args.output / (label + '.json'), args.output / (label + '.stats.json')
            command = [str(args.weavec), '--whole-program', '--checked-function=main',
                       '--checked-report=' + str(report), '--analysis-stats=' + str(stats)]
            if cached:
                command += ['--analysis-cache=' + str(work / 'cache')]
            if compact:
                command += ['--checked-report-format=compact']
            command += [str(work / 'hooks-good.c'), str(implementation), '--', '-std=c11']
            run, seconds = invoke(args, label, command)
            assert run.returncode in (0, 1) and document(report), 'analysis failed: ' + label
            actual = REPORT.expand_report(document(report))
            retain(report)
            return run.returncode, actual, run.stderr, document(stats)['counters'], seconds

        cold, warm, off = analyze('cold'), analyze('warm'), analyze('off', False)
        assert cold[:3] == warm[:3] == off[:3] and cold[0] == 0, 'cached contracts differ'
        assert warm[3].get('cache_hits') == 2 and warm[3].get('function_analyses', 0) == 0
        results.append(dict(name='cold-warm-off-equivalent', passed=True, counters=warm[3]))
        compact = analyze('compact', compact=True)
        assert cold[:3] == compact[:3], 'compact report differs'
        assert compact[3].get('cache_hits') == 2 and compact[3].get('function_analyses', 0) == 0
        results.append(dict(name='compact-equivalent', passed=True, counters=compact[3]))
        mutations = [
            ('cleared-hook', original.replace('hooks.release = free;', 'hooks.release = NULL;'), 1),
            ('missing-cleanup', original.replace('if (p) hooks.release(p);', '(void)p;'), 1),
            ('changed-state-layout', original.replace('} hooks;', 'unsigned revision; } hooks;'), 0),
            ('changed-object-layout-uninitialized', original.replace('struct node { unsigned value;', 'struct node { unsigned tag; unsigned value;'), 1),
            ('changed-object-layout', original.replace('struct node { unsigned value;', 'struct node { unsigned tag; unsigned value;').replace('p->value =', 'p->tag = 0; p->value ='), 0),
        ]
        for label, changed, expected in mutations:
            assert changed != original, 'empty source mutation'
            implementation.write_text(changed)
            cached, fresh = analyze(label), analyze(label + '-off', False)
            assert cached[:3] == fresh[:3] and cached[0] == expected, 'stale contract reused: ' + label
            assert cached[3].get('function_analyses', 0) > 0
            results.append(dict(name=label, passed=True, counters=cached[3]))
        implementation.write_text(original)
        for path in (work / 'cache').glob('*.wcache'):
            path.write_bytes(path.read_bytes()[:91])
        corrupt, fresh = analyze('corrupt'), analyze('corrupt-off', False)
        assert corrupt[:3] == fresh[:3] and corrupt[0] == 0
        assert corrupt[3].get('cache_hits', 0) == 0
        results.append(dict(name='corrupt-cache-recomputed', passed=True))
        for i, source in enumerate(('hooks-good.c', 'library.c')):
            run, _ = invoke(args, 'compile-' + str(i),
                [str(args.cc), '-std=c11', '-c', source, '-o', str(i) + '.o'], work)
            assert run.returncode == 0, 'object compilation failed'

        def link(label):
            return invoke(args, label, [str(args.cc), '-fweavec-checked-function=main',
                '-fweavec-checked-report=' + str(args.output / (label + '.json')),
                '0.o', '1.o', '-o', 'client'], work)[0]

        assert link('sidecar-good').returncode == 0
        implementation.write_text(original + '\n/* changed source binding */\n')
        stale = link('sidecar-stale')
        assert stale.returncode == 1 and 'source input changed' in stale.stderr
        results.append(dict(name='stale-sidecar-rejected', passed=True))
        implementation.write_text(original)
        sidecar = work / '1.o.weavec'
        text = sidecar.read_text()
        version = re.match(r'weavec-summaries (\d+)\n', text)
        assert version
        old = int(version[1]) - 1
        sidecar.write_text(re.sub(r'^weavec-summaries \d+\n', 'weavec-summaries ' + str(old) + '\n', text, count=1))
        rejected = link('sidecar-old')
        assert rejected.returncode == 1 and 'unsupported format ' + str(old) in rejected.stderr
        results.append(dict(name='old-sidecar-rejected', passed=True))
    save(args, results)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--population', choices=('source', 'objects', 'cache', 'regressions', 'regression-objects', 'upstream', 'upstream-objects'), required=True)
    parser.add_argument('--weavec', type=Path, required=True)
    parser.add_argument('--cc', type=Path)
    parser.add_argument('--clang', default=shutil.which('clang'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=600)
    args = parser.parse_args()
    if args.population in ('objects', 'regression-objects', 'upstream-objects', 'cache') and not args.cc:
        parser.error('this population requires --cc')
    args.weavec = args.weavec.resolve()
    args.cc = args.cc.resolve() if args.cc else None
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    args.hashes = {str(binary): digest(binary) for binary in (args.weavec, args.cc) if binary}
    try:
        verify(FIXTURES)
        results = {'objects': objects, 'regression-objects': objects, 'upstream-objects': objects, 'cache': cache}.get(args.population, source)(args)
    except (OSError, ValueError, AssertionError, subprocess.SubprocessError) as error:
        save(args, getattr(args, 'progress', []) + [dict(name='population', passed=False, reason=str(error))])
        parser.error(str(error))
    return 0 if results and all(row['passed'] for row in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
