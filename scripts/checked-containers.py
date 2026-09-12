#!/usr/bin/env python3
"""RFC 0023 compiler-object and unchanged-source contract validation."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / 'test/evaluation/rfc0023'
SPEC = importlib.util.spec_from_file_location('checked_evaluation', ROOT / 'scripts/checked-evaluation.py')
EVALUATION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EVALUATION)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_frozen(directory):
    inventory = json.loads((directory / 'frozen-sha256.json').read_text())
    for name, expected in inventory.items():
        if digest(directory / name) != expected:
            raise ValueError('frozen input changed: ' + str(directory / name))


def invoke(command, report, timeout, cwd=None):
    report.unlink(missing_ok=True)
    start = time.monotonic()
    run = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=timeout)
    document = json.loads(report.read_text()) if report.exists() else {}
    return run, document, time.monotonic() - start


def objects(args):
    directory = FIXTURES / 'transport'
    verify_frozen(directory)
    manifest = json.loads((directory / 'manifest.json').read_text())
    results = []
    for case in manifest['cases']:
        output = args.output / case['name']
        output.mkdir(parents=True, exist_ok=True)
        result = dict(name=case['name'], expected=case['expect'], stages=[],
                      inputs={name: digest(directory / name) for name in case['sources']})
        try:
            # Separate copies keep artifact binding checks local to the case.
            with tempfile.TemporaryDirectory(prefix='weavec-container-objects-') as temporary:
                work = Path(temporary)
                shutil.copy2(directory / 'node.h', work / 'node.h')
                for name in case['sources']:
                    shutil.copy2(directory / name, work / name)
                syntax = subprocess.run([args.clang, '-std=c11', '-fsyntax-only', *case['sources']],
                                        cwd=work, capture_output=True, text=True, timeout=args.timeout)
                if syntax.returncode:
                    raise ValueError('C syntax failure: ' + syntax.stderr)
                object_files = []
                for index, name in enumerate(case['sources']):
                    report = output / ('compile-' + str(index) + '.json')
                    obj = 'unit-' + str(index) + '.o'
                    command = [str(args.cc), '-std=c11', '-fweavec-checked-report=' + str(report)]
                    if index == 0:
                        command.append('-fweavec-checked-function=main')
                    command += ['-c', name, '-o', obj]
                    run, document, seconds = invoke(command, report, args.timeout, work)
                    (output / ('compile-' + str(index) + '.log')).write_text(run.stdout + run.stderr)
                    result['stages'].append(dict(stage='compile', source=name, seconds=seconds,
                                                 returncode=run.returncode, report=str(report)))
                    if run.returncode:
                        if index == 0 and case['expect'] == 'rejected':
                            passed, reason = EVALUATION.assess(case, run.returncode, document)
                            result.update(passed=passed, reason=reason, rejected_at='compile',
                                          report=str(report), returncode=run.returncode)
                            break
                        if index > 0 and case['expect'] == 'rejected':
                            # An ordinary hard error also prevents object
                            # generation. Require its matching checked violation,
                            # never accept a parser or infrastructure failure.
                            violations = [entry for unit in document.get('units', [])
                                          for function in unit.get('functions', [])
                                          for entry in function.get('obligations', [])
                                          if entry['outcome'] == 'violation' and
                                          'after it was freed' in entry['reason']]
                            if run.returncode == 1 and violations and '[weavec::use-after-free]' in run.stderr:
                                result.update(passed=True, reason='', rejected_at='compile-helper',
                                              report=str(report), returncode=run.returncode,
                                              violations=violations)
                                break
                        raise ValueError('object compilation failed before link verification')
                    metadata = work / (obj + '.weavec')
                    if not metadata.is_file():
                        raise ValueError('compiler produced no sidecar')
                    result['stages'][-1]['sidecar_sha256'] = digest(metadata)
                    object_files.append(obj)
                if 'rejected_at' in result:
                    # A checked violation in the caller is rejected before an
                    # object exists. Preserve that report; it is not a link test.
                    results.append(result)
                    print(('PASS ' if result['passed'] else 'FAIL ') + case['name'] +
                          ': compile-time checked rejection', flush=True)
                    write_results(args, results)
                    continue
                report = output / 'linked.json'
                command = [str(args.cc), '-fweavec-checked-function=main',
                           '-fweavec-checked-report=' + str(report), *object_files, '-o', 'client']
                run, document, seconds = invoke(command, report, args.timeout, work)
                (output / 'linked.log').write_text(run.stdout + run.stderr)
                passed, reason = EVALUATION.assess(case, run.returncode, document)
                result.update(passed=passed, reason=reason, seconds=seconds,
                              returncode=run.returncode, report=str(report))
                if passed and case['expect'] == 'accepted' and not (work / 'client').is_file():
                    result.update(passed=False, reason='successful checked link produced no executable')
        except subprocess.TimeoutExpired:
            result.update(passed=False, reason='timeout')
        except (OSError, ValueError) as error:
            result.update(passed=False, reason=str(error))
        result['inputs'] = {name: digest(directory / name) for name in case['sources']}
        results.append(result)
        print(('PASS ' if result['passed'] else 'FAIL ') + case['name'] + ': ' + result['reason'], flush=True)
        write_results(args, results)
    return results


def upstream(args):
    directory = FIXTURES / 'upstream'
    verify_frozen(directory)
    source = ROOT / 'build/corpus/cJSON-program'
    identity = json.loads((directory / 'upstream-identity.json').read_text())
    revision = subprocess.run(['git', '-C', str(source), 'rev-parse', 'HEAD'],
                              capture_output=True, text=True, check=True).stdout.strip()
    if revision != identity['commit']:
        raise ValueError('upstream revision differs from frozen selection')
    for name, expected in identity['files'].items():
        if digest(source / name) != expected:
            raise ValueError('upstream source changed: ' + name)
        original = subprocess.run(['git', '-C', str(source), 'show', 'HEAD:' + name],
                                  capture_output=True, check=True).stdout
        if hashlib.sha256(original).hexdigest() != expected:
            raise ValueError('frozen upstream source differs from its commit: ' + name)
    manifest = json.loads((directory / 'manifest.json').read_text())
    results = []
    for case in manifest['cases']:
        report = args.output / (case['name'] + '.json')
        caller = directory / case['source']
        result = dict(name=case['name'], expected=case['expect'])
        try:
            syntax = subprocess.run([args.clang, '-std=c11', '-fsyntax-only', str(caller)],
                                    capture_output=True, text=True, timeout=args.timeout)
            if syntax.returncode:
                raise ValueError('C syntax failure: ' + syntax.stderr)
            command = [str(args.weavec), '--checked-function=main', '--checked-report=' + str(report),
                       str(caller), '--', '-std=c11']
            run, document, seconds = invoke(command, report, args.timeout)
            (args.output / (case['name'] + '.log')).write_text(run.stdout + run.stderr)
            passed, reason = EVALUATION.assess(case, run.returncode, document)
            functions = [fn for unit in document.get('units', []) for fn in unit.get('functions', [])]
            target = ('cJSON_GetArraySize' if case['name'].startswith('size-') else
                      'cJSON_GetArrayItem' if case['name'].startswith('public-') else 'get_array_item')
            actual = [fn for fn in functions if fn['name'] == target]
            if not actual:
                passed, reason = False, 'unchanged target definition is absent'
            result.update(passed=passed, reason=reason, returncode=run.returncode,
                          seconds=seconds, report=str(report), target=target,
                          contracts=[dict(name=fn['name'], complete=fn['complete'],
                                          requirements=fn['requirements']) for fn in actual])
        except subprocess.TimeoutExpired:
            result.update(passed=False, reason='timeout')
        except (OSError, ValueError) as error:
            result.update(passed=False, reason=str(error))
        result['caller_sha256'] = digest(caller)
        results.append(result)
        print(('PASS ' if result['passed'] else 'FAIL ') + case['name'] + ': ' + result['reason'], flush=True)
        write_results(args, results)
    return results


def cache(args):
    """Container predicates obey the existing executable/input cache boundary."""
    directory = FIXTURES / 'transport'
    verify_frozen(directory)
    results = []
    with tempfile.TemporaryDirectory(prefix='weavec-container-cache-') as temporary:
        work = Path(temporary)
        names = ['borrowed-good.c', 'borrowed-good-impl.c']
        for name in [*names, 'node.h']:
            shutil.copy2(directory / name, work / name)
        original = (work / names[1]).read_text()
        sequence = 0

        def run(label, cached=True):
            nonlocal sequence
            sequence += 1
            stem = args.output / (str(sequence) + '-' + label)
            report = stem.with_suffix('.json')
            stats = stem.with_suffix('.stats.json')
            command = [str(args.weavec), '--whole-program', '--checked-function=main',
                       '--checked-report=' + str(report), '--analysis-stats=' + str(stats)]
            if cached:
                command.append('--analysis-cache=' + str(work / 'cache'))
            command += [str(work / name) for name in names] + ['--', '-std=c11']
            process, document, seconds = invoke(command, report, args.timeout)
            stem.with_suffix('.log').write_text(process.stdout + process.stderr)
            if process.returncode not in (0, 1) or not document:
                raise ValueError(label + ': invocation failure')
            counters = json.loads(stats.read_text())['counters']
            return process.returncode, document, process.stderr, counters, seconds

        cold, warm, fresh = run('cold'), run('warm'), run('uncached', False)
        assert cold[:3] == warm[:3] == fresh[:3], 'initial cached report differs'
        assert cold[0] == 0, 'valid borrowed caller did not check'
        assert warm[3].get('cache_hits') == 2 and warm[3].get('function_analyses', 0) == 0, warm[3]
        results.append(dict(name='unchanged', passed=True, counters=warm[3]))
        # Releasing a borrowed node before advancing invalidates helper and caller.
        assert 'p = p->next' in original
        (work / names[1]).write_text(original.replace('p = p->next', 'free((void *)p); p = p->next'))
        changed, fresh = run('changed-link'), run('changed-link-uncached', False)
        assert changed[:3] == fresh[:3], 'changed link reused a stale contract'
        assert changed[0] == 1 and changed[3].get('function_analyses', 0) > 0
        results.append(dict(name='changed-link', passed=True, counters=changed[3]))
        (work / names[1]).write_text(original)
        header = work / 'node.h'
        header.write_text(header.read_text().replace('unsigned value;', 'unsigned long value;'))
        changed, fresh = run('changed-layout'), run('changed-layout-uncached', False)
        assert changed[:3] == fresh[:3] and changed[0] == 0, 'changed layout differs'
        assert changed[3].get('function_analyses', 0) > 0
        results.append(dict(name='changed-layout', passed=True, counters=changed[3]))
        for record in (work / 'cache').glob('*.wcache'):
            record.write_bytes(record.read_bytes()[:91])
        changed, fresh = run('corrupt-cache'), run('corrupt-cache-uncached', False)
        assert changed[:3] == fresh[:3] and changed[0] == 0
        assert changed[3].get('cache_hits', 0) == 0
        results.append(dict(name='corrupt-cache', passed=True, counters=changed[3]))
        # Compile containers into actual objects, then invalidate their binding.
        for index, name in enumerate(names):
            report = args.output / ('sidecar-compile-' + str(index) + '.json')
            command = [str(args.cc), '-std=c11', '-fweavec-checked-report=' + str(report),
                       '-c', name, '-o', str(index) + '.o']
            process, _, _ = invoke(command, report, args.timeout, work)
            assert process.returncode == 0, process.stderr

        def link(label):
            report = args.output / (label + '.json')
            command = [str(args.cc), '-fweavec-checked-function=main',
                       '-fweavec-checked-report=' + str(report), '0.o', '1.o', '-o', 'client']
            process, document, _ = invoke(command, report, args.timeout, work)
            (args.output / (label + '.log')).write_text(process.stderr)
            return process, document

        process, document = link('sidecar-good')
        assert process.returncode == 0 and document.get('invocation_ok'), process.stderr
        (work / names[1]).write_text(original + '\n/* changed source binding */\n')
        process, _ = link('sidecar-stale')
        assert process.returncode == 1 and 'source input changed' in process.stderr, process.stderr
        results.append(dict(name='stale-container-sidecar', passed=True))
        (work / names[1]).write_text(original)
        metadata = work / '1.o.weavec'
        contents = metadata.read_text()
        assert contents.startswith('weavec-summaries 20\n')
        metadata.write_text(contents.replace('weavec-summaries 20\n', 'weavec-summaries 19\n', 1))
        process, _ = link('sidecar-old-format')
        assert process.returncode == 1 and 'unsupported format 19' in process.stderr, process.stderr
        results.append(dict(name='old-container-sidecar-format', passed=True))
    for result in results:
        print('PASS ' + result['name'], flush=True)
    write_results(args, results)
    return results


def scaling(args):
    results = []
    original = (FIXTURES / 'runtime-good.c').read_text()
    case = next(case for case in json.loads((FIXTURES / 'manifest.json').read_text())['cases']
                if case['name'] == 'runtime-good')
    with tempfile.TemporaryDirectory(prefix='weavec-container-scale-') as temporary:
        source = Path(temporary) / 'client.c'
        for length in (0, 1, 8, 64, 1024, 1048576):
            source.write_text(original.replace('build((unsigned)argc)', 'build(' + str(length) + 'U)'))
            report = args.output / (str(length) + '.json')
            stats = args.output / (str(length) + '.stats.json')
            syntax = subprocess.run([args.clang, '-std=c11', '-fsyntax-only', str(source)],
                                    capture_output=True, text=True, timeout=args.timeout)
            if syntax.returncode:
                raise ValueError('scaling C syntax failure: ' + syntax.stderr)
            command = [str(args.weavec), '--checked-function=main',
                       '--checked-report=' + str(report), '--analysis-stats=' + str(stats),
                       str(source), '--', '-std=c11']
            process, document, seconds = invoke(command, report, args.timeout)
            (args.output / (str(length) + '.log')).write_text(process.stderr)
            passed, reason = EVALUATION.assess(case, process.returncode, document)
            counters = json.loads(stats.read_text())['counters']
            results.append(dict(name=str(length), length=length, passed=passed, reason=reason,
                                seconds=seconds, counters=counters, source_sha256=digest(source)))
            print(('PASS ' if passed else 'FAIL ') + 'length ' + str(length), flush=True)
            write_results(args, results)
    return results


def write_results(args, cases):
    result = dict(version=1, rfc='0023', population=args.population, cases=cases,
                  executable_sha256=digest(args.cc if args.population == 'objects' else args.weavec))
    (args.output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--population', choices=['objects', 'upstream', 'cache', 'scaling'], required=True)
    parser.add_argument('--weavec', type=Path, required=True)
    parser.add_argument('--cc', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=600)
    parser.add_argument('--clang', default=shutil.which('clang'))
    args = parser.parse_args()
    if not args.clang or (args.population in ('objects', 'cache') and not args.cc):
        parser.error('clang is required; the objects population also needs --cc')
    args.weavec = args.weavec.resolve()
    args.cc = args.cc.resolve() if args.cc else None
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    try:
        verify_frozen(FIXTURES)
        results = {'objects': objects, 'upstream': upstream, 'cache': cache, 'scaling': scaling}[args.population](args)
    except (OSError, ValueError, AssertionError, subprocess.SubprocessError) as error:
        parser.error(str(error))
    return 0 if all(case['passed'] for case in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
