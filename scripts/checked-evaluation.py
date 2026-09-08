#!/usr/bin/env python3
"""Run the frozen RFC 0018 checked-mode cases; keep rejections separate from bugs."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--weavec', required=True)
    parser.add_argument('--manifest', type=Path, default=Path(__file__).resolve().parent.parent / 'test/evaluation/rfc0018/manifest.json')
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    results = []
    with tempfile.TemporaryDirectory(prefix='weavec-checked-') as temporary:
        for case in manifest['cases']:
            report = Path(temporary) / (case['name'] + '.json')
            command = [str(Path(args.weavec).resolve()), '--checked-function=' + case['function'],
                       '--checked-report=' + str(report), *case['flags'],
                       str(args.manifest.parent / case['source']), '--', '-ferror-limit=0']
            run = subprocess.run(command, capture_output=True, text=True, timeout=60)
            document = json.loads(report.read_text()) if report.exists() else {}
            scope = [fn for unit in document.get('units', []) for fn in unit['functions'] if fn['selected']]
            accepted = run.returncode == 0
            valid = bool(scope) and run.returncode in (0, 1)
            matched = valid and accepted == (case['expect'] == 'accepted')
            if accepted:
                matched &= all(fn['complete'] for fn in scope)
            results.append(dict(name=case['name'], expected=case['expect'], accepted=accepted,
                                passed=bool(matched), returncode=run.returncode,
                                stderr=run.stderr, report=document))
            print(('PASS' if matched else 'FAIL') + ' ' + case['name'])
    if args.json:
        args.json.write_text(json.dumps(dict(version=1, cases=results), indent=2) + '\n')
    return 0 if all(case['passed'] for case in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
