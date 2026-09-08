#!/usr/bin/env python3
"""RFC 0019: exercise frozen memory contracts through compiler sidecars."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location('checked_evaluation', ROOT / 'scripts/checked-evaluation.py')
EVALUATION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EVALUATION)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, default=ROOT / 'test/evaluation/rfc0019/transport-manifest.json')
    parser.add_argument('--json', type=Path)
    parser.add_argument('--timeout', type=float, default=120)
    args = parser.parse_args()
    compiler = str(args.cc.resolve())
    manifest = json.loads(args.manifest.read_text())
    results = []
    with tempfile.TemporaryDirectory(prefix='weavec-memory-objects-') as temporary:
        for case in manifest['cases']:
            directory = Path(temporary) / case['name']
            directory.mkdir()
            objects = []
            diagnostics = []
            result = dict(name=case['name'], expected=case['expect'], passed=False)
            try:
                for index, source in enumerate(case['sources']):
                    source_path = (args.manifest.parent / source).resolve()
                    obj = directory / f'unit-{index}.o'
                    report = directory / f'unit-{index}.json'
                    selection = ['-fweavec-checked-function=main'] if source.endswith('-main.c') else []
                    command = [compiler, '-std=c99', '-D_POSIX_C_SOURCE=200809L',
                               '-fweavec-checked-report=' + str(report), *selection,
                               '-c', str(source_path), '-o', str(obj)]
                    run = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
                    diagnostics.append(run.stdout + run.stderr)
                    if run.returncode or not obj.exists() or not Path(str(obj) + '.weavec').exists():
                        result['reason'] = 'source compile or sidecar generation failed'
                        break
                    if selection:
                        provisional = json.loads(report.read_text())
                        selected = [fn for unit in provisional.get('units', [])
                                    for fn in unit.get('functions', []) if fn.get('selected')]
                        if (len(selected) != 1 or selected[0]['name'] != 'main' or
                                selected[0]['complete'] or not selected[0]['deferred']):
                            result['reason'] = 'unavailable helper was not reported as deferred'
                            break
                    objects.append(obj)
                else:
                    report = directory / 'link.json'
                    executable = directory / 'program'
                    command = [compiler, '-fweavec-checked-function=main',
                               '-fweavec-checked-report=' + str(report),
                               *map(str, objects), '-o', str(executable)]
                    run = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
                    diagnostics.append(run.stdout + run.stderr)
                    document = json.loads(report.read_text()) if report.exists() else {}
                    result['passed'], result['reason'] = EVALUATION.assess(case, run.returncode, document)
                    result['report'] = document
                    result['returncode'] = run.returncode
                    if result['passed'] and case['expect'] == 'rejected' and executable.exists():
                        result.update(passed=False, reason='failed checked link produced an executable')
                    if result['passed'] and case['expect'] == 'accepted' and not executable.exists():
                        result.update(passed=False, reason='successful checked link produced no executable')
            except (subprocess.TimeoutExpired, json.JSONDecodeError, OSError) as error:
                result['reason'] = str(error)
            result['diagnostics'] = ''.join(diagnostics)
            results.append(result)
            print(('PASS ' if result['passed'] else 'FAIL ') + case['name'], flush=True)
            if not result['passed']:
                print(result.get('reason', ''), result['diagnostics'], flush=True)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(dict(version=1, cases=results), indent=2) + '\n')
    return 0 if all(result['passed'] for result in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
