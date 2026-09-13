#!/usr/bin/env python3
"""RFC 0025 frozen source, object, upstream and checkpoint acceptance."""

import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / 'test/evaluation/rfc0025'
SPEC = importlib.util.spec_from_file_location('checked_runtime', ROOT / 'scripts/checked-runtime.py')
SUPPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUPPORT)


def save(args, cases):
    (args.output / 'results.json').write_text(json.dumps(dict(
        version=1, rfc='0025', population=args.population, cases=cases,
        executable_sha256=SUPPORT.digest(args.cc if args.population == 'objects' else args.weavec)),
        indent=2) + '\n')


def cache(args):
    directory = FIXTURES / 'transport'
    SUPPORT.verify(directory)
    results = []
    with tempfile.TemporaryDirectory(prefix='weavec-case-cache-') as temporary:
        work = Path(temporary)
        sources = ['output-good.c', 'helpers.c', 'forward.c']
        for name in [*sources, 'runtime.h']:
            shutil.copy2(directory / name, work / name)
        original = (work / 'helpers.c').read_text()

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
            run, seconds = SUPPORT.invoke(args, label, command)
            document = SUPPORT.document(report)
            if run.returncode not in (0, 1) or not document:
                raise ValueError('analysis failed: ' + label)
            if compact:
                spec = importlib.util.spec_from_file_location('checked_report', ROOT / 'scripts/checked-report.py')
                module = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(module)
                document = module.expand_report(document)
            return run.returncode, document, run.stderr, SUPPORT.document(stats)['counters'], seconds

        cold, warm, uncached = analyze('cold'), analyze('warm'), analyze('uncached', False)
        assert cold[:3] == warm[:3] == uncached[:3], 'cached reports differ'
        assert cold[0] == 0, 'valid output member was rejected'
        assert warm[3].get('cache_hits') == 3 and warm[3].get('function_analyses', 0) == 0
        results.append(dict(name='equivalent-unchanged', passed=True, counters=warm[3]))
        compact = analyze('compact', False, True)
        assert compact[:3] == uncached[:3], 'compact proof cases differ'
        results.append(dict(name='compact-expanded-equivalent', passed=True))

        changed_source = original.replace('value->pointer=pointer;', '(void)pointer;value->number=7;')
        assert changed_source != original
        (work / 'helpers.c').write_text(changed_source)
        changed, fresh = analyze('changed-member'), analyze('changed-member-uncached', False)
        assert changed[:3] == fresh[:3] and changed[0] == 1, 'stale output member reused'
        assert changed[3].get('function_analyses', 0) > 0
        results.append(dict(name='changed-member-invalidates-output', passed=True, counters=changed[3]))

        (work / 'helpers.c').write_text(original)
        for path in (work / 'cache').glob('*.wcache'):
            path.write_bytes(path.read_bytes()[:91])
        corrupt, fresh = analyze('corrupt'), analyze('corrupt-uncached', False)
        assert corrupt[:3] == fresh[:3] and corrupt[0] == 0
        assert corrupt[3].get('cache_hits', 0) == 0
        results.append(dict(name='corrupt-cache-recomputed', passed=True))

        # A caller-specific proof is not a replacement for a selected generic.
        for name in ['readonly-good.c', 'helpers.c', 'forward.c']:
            shutil.copy2(directory / name, work / name)
        report = args.output / 'selected-generic.json'
        command = [str(args.weavec), '--whole-program', '--checked-function=main',
                   '--checked-function=read_if', '--checked-report=' + str(report),
                   *[str(work / name) for name in ['readonly-good.c', 'helpers.c', 'forward.c']], '--']
        run, _ = SUPPORT.invoke(args, 'selected-generic', command)
        assert run.returncode == 1, 'successful case replaced generic failure'
        selected = {fn['name']: fn for unit in SUPPORT.document(report)['units']
                    for fn in unit['functions'] if fn['selected']}
        assert selected['main']['complete'] and not selected['read_if']['complete']
        assert any(case['complete'] for case in selected['read_if']['cases'])
        results.append(dict(name='selected-generic-remains-independent', passed=True))
    save(args, results)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--population', choices=['source', 'transport', 'regressions', 'objects', 'cache', 'upstream'], required=True)
    parser.add_argument('--weavec', type=Path, required=True)
    parser.add_argument('--cc', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=600)
    args = parser.parse_args()
    args.weavec = args.weavec.resolve()
    args.cc = args.cc.resolve() if args.cc else None
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    # Reuse the existing strict build-binding and upstream-identity checks.
    SUPPORT.FIXTURES = FIXTURES
    SUPPORT.save = save
    try:
        if args.population == 'objects':
            if args.cc is None:
                parser.error('--cc is required for objects')
            cases = SUPPORT.objects(args)
        elif args.population == 'cache':
            cases = cache(args)
        elif args.population == 'upstream':
            cases = SUPPORT.upstream(args)
        else:
            directory = FIXTURES if args.population == 'source' else FIXTURES / args.population
            SUPPORT.verify(directory)
            report = args.output / 'cases.json'
            run, _ = SUPPORT.invoke(args, 'evaluation', [
                'python3', str(ROOT / 'scripts/checked-evaluation.py'), '--weavec', str(args.weavec),
                '--manifest', str(directory / 'manifest.json'), '--json', str(report),
                '--timeout', str(args.timeout)])
            if run.returncode not in (0, 1):
                raise ValueError('evaluator failed')
            cases = SUPPORT.document(report)['cases']
        save(args, cases)
        return 0 if all(case['passed'] for case in cases) else 1
    except (AssertionError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        save(args, [dict(name=args.population, passed=False, reason=str(error))])
        print('FAIL ' + args.population + ': ' + str(error), flush=True)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
