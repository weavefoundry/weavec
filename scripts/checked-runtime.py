#!/usr/bin/env python3
"""RFC 0024 object, cache and unchanged-source validation with frozen inputs."""
import argparse
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
FIXTURES = ROOT / 'test/evaluation/rfc0024'
SPEC = importlib.util.spec_from_file_location('checked_evaluation', ROOT / 'scripts/checked-evaluation.py')
EVALUATION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EVALUATION)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(directory):
    for name, expected in json.loads((directory / 'frozen-sha256.json').read_text()).items():
        if digest(directory / name) != expected:
            raise ValueError('frozen input differs: ' + str(directory / name))


def invoke(args, label, command, cwd=None):
    # A repeated output directory must never supply a previous run's proof
    # when this invocation fails before writing its requested artifacts.
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


def objects(args):
    directory = FIXTURES / 'transport'
    verify(directory)
    results = []
    for case in document(directory / 'manifest.json')['cases']:
        result = dict(name=case['name'], passed=False, stages=[])
        try:
            with tempfile.TemporaryDirectory(prefix='weavec-runtime-object-') as temporary:
                work = Path(temporary)
                for name in [*case['sources'], 'runtime.h']:
                    shutil.copy2(directory / name, work / name)
                for index, name in enumerate(case['sources']):
                    label = case['name'] + '-compile-' + str(index)
                    report = args.output / (label + '.json')
                    command = [str(args.cc), '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                               '-fweavec-checked-report=' + str(report)]
                    if index == 0:
                        command.append('-fweavec-checked-function=main')
                    command += ['-c', name, '-o', str(index) + '.o']
                    run, seconds = invoke(args, label, command, work)
                    result['stages'].append(dict(label=label, returncode=run.returncode, seconds=seconds))
                    if run.returncode:
                        passed, reason = EVALUATION.assess(case, run.returncode, document(report))
                        result.update(passed=passed, reason=reason, rejected_at='compile')
                        break
                    metadata = work / (str(index) + '.o.weavec')
                    if not metadata.is_file():
                        raise ValueError('object has no sidecar')
                    result['stages'][-1]['sidecar_sha256'] = digest(metadata)
                else:
                    label = case['name'] + '-link'
                    report = args.output / (label + '.json')
                    command = [str(args.cc), '-fweavec-checked-function=main',
                               '-fweavec-checked-report=' + str(report),
                               *[str(i) + '.o' for i in range(len(case['sources']))], '-o', 'client']
                    run, seconds = invoke(args, label, command, work)
                    passed, reason = EVALUATION.assess(case, run.returncode, document(report))
                    if passed and case['expect'] == 'accepted' and not (work / 'client').is_file():
                        passed, reason = False, 'checked link produced no executable'
                    result.update(passed=passed, reason=reason, seconds=seconds, returncode=run.returncode)
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
    with tempfile.TemporaryDirectory(prefix='weavec-runtime-cache-') as temporary:
        work = Path(temporary)
        sources = ['format-good.c', 'render.c']
        for name in [*sources, 'runtime.h']:
            shutil.copy2(directory / name, work / name)
        original = (work / 'render.c').read_text()

        def analyze(label, cached=True):
            report = args.output / (label + '.json')
            stats = args.output / (label + '.stats.json')
            command = [str(args.weavec), '--whole-program', '--checked-function=main',
                       '--checked-report=' + str(report), '--analysis-stats=' + str(stats)]
            if cached:
                command.append('--analysis-cache=' + str(work / 'cache'))
            command += [str(work / name) for name in sources] + ['--', '-std=c11']
            run, seconds = invoke(args, label, command)
            if run.returncode not in (0, 1) or not document(report):
                raise ValueError('analysis failed: ' + label)
            return run.returncode, document(report), run.stderr, document(stats)['counters'], seconds

        cold, warm, uncached = analyze('cold'), analyze('warm'), analyze('uncached', False)
        assert cold[:3] == warm[:3] == uncached[:3], 'cached reports differ'
        assert cold[0] == 0, 'valid caller was rejected'
        assert warm[3].get('cache_hits') == 2 and warm[3].get('function_analyses', 0) == 0
        results.append(dict(name='equivalent-unchanged', passed=True, counters=warm[3]))
        (work / 'render.c').write_text(original.replace('return r;', 'return 0;'))
        changed, fresh = analyze('changed-result'), analyze('changed-result-uncached', False)
        assert changed[:3] == fresh[:3] and changed[0] == 1, 'stale output predicate reused'
        assert changed[3].get('function_analyses', 0) > 0
        results.append(dict(name='changed-result-invalidates-output', passed=True, counters=changed[3]))
        (work / 'render.c').write_text(original)
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
        (work / 'render.c').write_text(original + '\n/* changed binding */\n')
        run = link('sidecar-stale')
        assert run.returncode == 1 and 'source input changed' in run.stderr
        results.append(dict(name='stale-sidecar-rejected', passed=True))
        (work / 'render.c').write_text(original)
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


def upstream(args):
    directory = FIXTURES / 'upstream'
    verify(directory)
    for name, identity in document(directory / 'upstream-identity.json').items():
        source = ROOT / 'build/corpus' / name
        revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
        assert revision == identity['commit'], 'upstream commit differs'
        for file, expected in identity['files'].items():
            assert digest(source / file) == expected, 'upstream source changed'
            original = subprocess.check_output(['git', '-C', str(source), 'show', 'HEAD:' + file])
            assert hashlib.sha256(original).hexdigest() == expected
    report = args.output / 'cases.json'
    run, _ = invoke(args, 'source', ['python3', str(ROOT / 'scripts/checked-evaluation.py'),
                    '--weavec', str(args.weavec), '--manifest', str(directory / 'manifest.json'),
                    '--timeout', str(args.timeout), '--json', str(report)])
    if run.returncode not in (0, 1):
        raise ValueError('upstream evaluator failed')
    results = document(report)['cases']
    save(args, results)
    return results


def save(args, cases):
    (args.output / 'results.json').write_text(json.dumps(dict(
        version=1, rfc='0024', population=args.population, cases=cases,
        executable_sha256=digest(args.cc if args.population == 'objects' else args.weavec)), indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--population', choices=['objects', 'cache', 'upstream'], required=True)
    parser.add_argument('--weavec', type=Path, required=True)
    parser.add_argument('--cc', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=600)
    args = parser.parse_args()
    if args.population != 'upstream' and not args.cc:
        parser.error('this population requires --cc')
    args.weavec = args.weavec.resolve()
    args.cc = args.cc.resolve() if args.cc else None
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    try:
        verify(FIXTURES)
        results = {'objects': objects, 'cache': cache, 'upstream': upstream}[args.population](args)
    except (OSError, ValueError, AssertionError, subprocess.SubprocessError) as error:
        save(args, [dict(name='population', passed=False, reason=str(error))])
        parser.error(str(error))
    return 0 if results and all(row['passed'] for row in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
