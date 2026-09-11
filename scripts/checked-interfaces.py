#!/usr/bin/env python3
"""RFC 0022: validate unchanged Jansson interfaces under actual callback bindings."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / 'test/evaluation/rfc0022'
SPEC = importlib.util.spec_from_file_location('checked_evaluation', ROOT / 'scripts/checked-evaluation.py')
EVALUATION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EVALUATION)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--weavec', type=Path, required=True)
    parser.add_argument('--source', type=Path, default=ROOT / 'build/corpus/jansson')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/rfc22-validation/real')
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--clang', default=shutil.which('clang'))
    args = parser.parse_args()
    manifest = json.loads((FIXTURES / 'real-modules.json').read_text())
    hashes = json.loads((FIXTURES / 'frozen-sha256.json').read_text())
    for name, expected in hashes.items():
        if hashlib.sha256((FIXTURES / name).read_bytes()).hexdigest() != expected:
            parser.error('frozen input changed: ' + name)
    revision = subprocess.run(['git', '-C', str(args.source), 'rev-parse', 'HEAD'],
                              capture_output=True, text=True, check=True).stdout.strip()
    if revision != manifest['revision']:
        parser.error('upstream revision differs from frozen selection')
    if subprocess.run(['git', '-C', str(args.source), 'diff', '--quiet', 'HEAD', '--', 'src']).returncode:
        parser.error('upstream source or headers have changed')
    if not args.clang:
        parser.error('clang is required for independent syntax validation')
    args.output.mkdir(parents=True, exist_ok=True)
    compiler_flags = ['-std=c99', '-DHAVE_CONFIG_H', '-ferror-limit=0',
                      '-I' + str(args.source / 'src'),
                      '-I' + str(ROOT / 'scripts/corpus/support/jansson')]
    results = []
    for case in manifest['cases']:
        report = args.output / (case['name'] + '.json')
        report.unlink(missing_ok=True)
        inputs = [args.source / name for name in manifest['sources']]
        inputs.append(FIXTURES / case['source'])
        command = [str(args.weavec.resolve()), '--whole-program', '--checked-function=main',
                   '--checked-report=' + str(report), *map(str, inputs), '--', *compiler_flags]
        start = time.monotonic()
        result = dict(name=case['name'], expected=case['expect'], command=command)
        try:
            syntax = subprocess.run([args.clang, '-fsyntax-only', *compiler_flags, *map(str, inputs)],
                                    capture_output=True, text=True, timeout=args.timeout)
            if syntax.returncode:
                raise ValueError('C syntax failure: ' + syntax.stderr)
            run = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
            document = json.loads(report.read_text()) if report.exists() else {}
            expected = dict(case, function='main', entry_requirements=0,
                            forbidden_trust=['unsafe', 'annotation assumptions'])
            passed, reason = EVALUATION.assess(expected, run.returncode, document)
            functions = [fn for unit in document.get('units', []) for fn in unit.get('functions', [])]
            present = {fn['name'] for fn in functions}
            if not set(manifest['functions']).issubset(present):
                passed, reason = False, 'a frozen upstream definition is absent'
            log = args.output / (case['name'] + '.log')
            log.write_text(run.stdout + run.stderr)
            result.update(passed=passed, reason=reason, returncode=run.returncode,
                          report=str(report), log=str(log),
                          generic_contracts=[dict(name=fn['name'], complete=fn['complete'],
                                                  requirements=fn['requirements']) for fn in functions
                                             if fn['name'] in manifest['functions']])
        except subprocess.TimeoutExpired:
            result.update(passed=False, reason='timeout')
        except (ValueError, OSError) as error:
            result.update(passed=False, reason=str(error))
        result['seconds'] = time.monotonic() - start
        result['inputs'] = [dict(path=str(p), sha256=hashlib.sha256(p.read_bytes()).hexdigest())
                            for p in inputs]
        results.append(result)
        print(('PASS ' if result['passed'] else 'FAIL ') + case['name'] + ': ' + result['reason'], flush=True)
    (args.output / 'results.json').write_text(json.dumps(dict(version=1, revision=revision,
                                                            cases=results), indent=2) + '\n')
    return 0 if all(case['passed'] for case in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
