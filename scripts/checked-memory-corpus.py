#!/usr/bin/env python3
"""RFC 0019: check pinned, unmodified Jansson interfaces and their callers."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / 'test/evaluation/rfc0019'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--weavec', type=Path, required=True)
    parser.add_argument('--source', type=Path, default=ROOT / 'build/corpus/jansson')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/rfc19-validation/real')
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--clang', default=shutil.which('clang'))
    args = parser.parse_args()
    manifest = json.loads((FIXTURES / 'real-modules.json').read_text())
    revision = subprocess.run(['git', '-C', str(args.source), 'rev-parse', 'HEAD'],
                              capture_output=True, text=True, check=True).stdout.strip()
    if revision != manifest['revision']:
        parser.error('Jansson checkout does not match the frozen revision')
    dirty = subprocess.run(['git', '-C', str(args.source), 'diff', '--exit-code', 'HEAD',
                            '--', 'src'], capture_output=True, text=True)
    if dirty.returncode:
        parser.error('upstream source has been modified')
    if not args.clang:
        parser.error('clang is required for independent syntax validation')
    args.output.mkdir(parents=True, exist_ok=True)
    cases = [
        ('utf-encode', 1, 'accepted', None),
        ('utf-short', 1, 'rejected', 'extent'),
        ('utf-failure', 1, 'rejected', 'initializ'),
        ('strbuffer-lifecycle', 0, 'accepted', None),
        ('strbuffer-short', 0, 'rejected', 'extent'),
        ('strbuffer-failure', 0, 'rejected', 'null'),
        ('strbuffer-partial', 0, 'rejected', 'initializ'),
        ('strbuffer-stale', 0, 'rejected', 'freed'),
    ]
    results = []
    for name, selection, expected, reason in cases:
        interface = manifest['selections'][selection]
        selected = [*interface['functions'], 'main'] if expected == 'accepted' else ['main']
        source = args.source / interface['source']
        inputs = [source, FIXTURES / 'real' / (name + '.c')]
        if selection == 0:
            inputs.append(FIXTURES / 'real/allocators.c')
        report = args.output / (name + '.json')
        if report.exists():
            report.unlink()
        compile_flags = ['-std=c99', '-DHAVE_CONFIG_H', '-ferror-limit=0',
                         '-I' + str(args.source / 'src'),
                         '-I' + str(ROOT / 'scripts/corpus/support/jansson')]
        command = [str(args.weavec.resolve()), '--whole-program',
                   *['--checked-function=' + fn for fn in selected],
                   '--checked-report=' + str(report), *map(str, inputs), '--',
                   *compile_flags]
        start = time.monotonic()
        try:
            syntax = subprocess.run([args.clang, '-fsyntax-only', *compile_flags,
                                     *map(str, inputs)], capture_output=True,
                                    text=True, timeout=args.timeout)
            if syntax.returncode:
                raise ValueError('C syntax failure: ' + syntax.stderr)
            run = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
            document = json.loads(report.read_text()) if report.exists() else {}
            scope = [fn for unit in document.get('units', []) for fn in unit['functions'] if fn['selected']]
            complete = bool(scope) and all(fn['complete'] and not fn['limited'] and not fn['deferred'] for fn in scope)
            accepted = run.returncode == 0 and complete and document.get('invocation_ok', False)
            valid = run.returncode in (0, 1) and sorted(fn['name'] for fn in scope) == sorted(selected)
            valid &= all(not fn['requirements'] for fn in scope if fn['name'] == 'main')
            obligations = [entry for fn in scope for entry in fn['obligations']]
            if reason:
                valid &= any(entry['outcome'] in ('unresolved', 'violation') and reason in entry['reason'] for entry in obligations)
            valid &= not any(entry['outcome'] == 'trusted' and ('unsafe boundary' in entry['reason'] or 'annotation assumptions' in entry['reason']) for entry in obligations)
            passed = valid and accepted == (expected == 'accepted')
            log = args.output / (name + '.log')
            log.write_text(run.stdout + run.stderr)
            result = dict(name=name, expected=expected, passed=passed, returncode=run.returncode,
                          accepted=accepted, selected=selected, report=str(report), log=str(log),
                          report_bytes=report.stat().st_size if report.exists() else 0,
                          interfaces=[dict(name=fn['name'], complete=fn['complete'],
                                           requirements=fn['requirements'],
                                           trust=[entry for entry in fn['obligations'] if entry['outcome'] == 'trusted']) for fn in scope])
        except subprocess.TimeoutExpired:
            result = dict(name=name, expected=expected, passed=False, reason='timeout')
        except (ValueError, OSError) as error:
            result = dict(name=name, expected=expected, passed=False, reason=str(error))
        result['seconds'] = time.monotonic() - start
        result['inputs'] = [{'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()} for path in inputs]
        results.append(result)
        print(('PASS ' if result['passed'] else 'FAIL ') + name, flush=True)
    output = dict(version=1, revision=revision, selections=manifest['selections'], cases=results)
    (args.output / 'results.json').write_text(json.dumps(output, indent=2) + '\n')
    return 0 if all(case['passed'] for case in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
