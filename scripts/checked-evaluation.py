#!/usr/bin/env python3
"""Run frozen checked-mode cases, distinguishing intended rejection from tool failure."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def assess(case, returncode, document):
    """An invocation failure or unrelated rejection never satisfies a case."""
    selected = [fn for unit in document.get('units', [])
                for fn in unit.get('functions', []) if fn.get('selected')]
    names = case.get('functions', [case.get('function')])
    if (returncode not in (0, 1) or
            sorted(fn['name'] for fn in selected) != sorted(names)):
        return False, 'invocation or selection failed'
    accepted = returncode == 0 and document.get('invocation_ok') and all(fn['complete'] for fn in selected)
    if accepted != (case['expect'] == 'accepted'):
        return False, 'unexpected proof outcome'
    if 'entry_requirements' in case:
        if any(len(fn.get('requirements', [])) != case['entry_requirements']
               for fn in selected):
            return False, 'unexpected entry assumptions'
    obligations = [entry for fn in selected for entry in fn.get('obligations', [])]
    if not accepted and not any(entry['outcome'] in ('unresolved', 'violation') for entry in obligations):
        return False, 'rejection has no checked obligation'
    for forbidden in case.get('forbidden_trust', []):
        if any(entry['outcome'] == 'trusted' and forbidden in entry['reason']
               for entry in obligations):
            return False, 'forbidden trust assumption'
    if case.get('reason') and not any(
            entry['outcome'] in ('unresolved', 'violation') and
            re.search(case['reason'], entry['reason'], re.IGNORECASE)
            for entry in obligations):
        return False, 'intended missing property was not reported'
    if accepted and any(fn.get('limited') or fn.get('deferred') for fn in selected):
        return False, 'incomplete analysis reported success'
    return True, ''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--weavec', required=True)
    parser.add_argument('--manifest', type=Path, default=Path(__file__).resolve().parent.parent / 'test/evaluation/rfc0018/manifest.json')
    parser.add_argument('--json', type=Path)
    parser.add_argument('--clang', default=shutil.which('clang'))
    parser.add_argument('--timeout', type=float, default=60)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    results = []
    with tempfile.TemporaryDirectory(prefix='weavec-checked-') as temporary:
        for case in manifest['cases']:
            report = Path(temporary) / (case['name'] + '.json')
            sources = case.get('sources', [case.get('source')])
            functions = case.get('functions', [case.get('function')])
            command = [str(Path(args.weavec).resolve()),
                       *['--checked-function=' + name for name in functions],
                       '--checked-report=' + str(report), *case.get('flags', [])]
            if len(sources) > 1:
                command.append('--whole-program')
            command += [str(args.manifest.parent / source) for source in sources]
            command += ['--', '-ferror-limit=0', '-D_POSIX_C_SOURCE=200809L']
            try:
                if args.clang:
                    syntax = subprocess.run(
                        [args.clang, '-fsyntax-only', '-D_POSIX_C_SOURCE=200809L',
                         '-I' + str(Path(__file__).resolve().parent.parent / 'resources/include'),
                         *[str(args.manifest.parent / source) for source in sources]],
                        capture_output=True, text=True, timeout=args.timeout)
                    if syntax.returncode:
                        results.append(dict(name=case['name'], expected=case['expect'],
                                            accepted=False, passed=False, reason='C syntax failure',
                                            stderr=syntax.stderr))
                        print('FAIL ' + case['name'] + ': C syntax failure', flush=True)
                        print(syntax.stderr, file=sys.stderr, end='', flush=True)
                        continue
                run = subprocess.run(command, capture_output=True, text=True,
                                     timeout=args.timeout)
                try:
                    document = json.loads(report.read_text()) if report.exists() else {}
                except (ValueError, OSError):
                    document = {}
                matched, reason = assess(case, run.returncode, document)
                result = dict(name=case['name'], expected=case['expect'],
                              accepted=run.returncode == 0, passed=matched,
                              returncode=run.returncode, reason=reason,
                              stderr=run.stderr, report=document)
            except subprocess.TimeoutExpired:
                result = dict(name=case['name'], expected=case['expect'],
                              accepted=False, passed=False, reason='timeout')
            results.append(result)
            print(('PASS' if result['passed'] else 'FAIL') + ' ' + case['name'] +
                  (': ' + result['reason'] if result['reason'] else ''), flush=True)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(dict(version=2, cases=results), indent=2) + '\n')
    return 0 if all(case['passed'] for case in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
