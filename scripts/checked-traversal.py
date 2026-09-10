#!/usr/bin/env python3
"""Validate RFC 0021 against frozen, unchanged upstream source and callers."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent
FROZEN = ROOT / 'test/evaluation/rfc0021'
IDENTITIES = {
    'real-modules.json': '951c3c72b5ee84424f7391db64e3606c9713a309c770b49b1c376e9f984614f5',
    'callers.json': 'cd983030597d5470e00df76ea46992a7cc381af404a348e03e7b3685c92d8421',
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verified_module(selection, corpus):
    directory = corpus / selection['project']
    head = subprocess.check_output(['git', '-C', str(directory), 'rev-parse', 'HEAD'], text=True).strip()
    if head != selection['revision']:
        raise ValueError(f"{selection['project']}: unexpected revision {head}")
    tracked = subprocess.check_output(['git', '-C', str(directory), 'ls-files', '-z']).split(b'\0')
    identities = {}
    for name in tracked:
        if not name or Path(name.decode()).suffix not in ('.c', '.h'):
            continue
        relative = name.decode()
        original = subprocess.check_output(['git', '-C', str(directory), 'show', 'HEAD:' + relative])
        current = directory / relative
        expected = hashlib.sha256(original).hexdigest()
        if not current.is_file() or digest(current) != expected:
            raise ValueError(f"{selection['project']}: modified upstream file {relative}")
        identities[relative] = expected
    if selection['source'] not in identities:
        raise ValueError('selected upstream source is not tracked')
    return directory, identities


def run_case(binary, compiler, selection, directory, case, temporary, timeout, assess):
    name = case['name']
    report = temporary / (name + '.json')
    sources = [directory / selection['source']]
    functions = selection['functions'] if case.get('interface') else ['main']
    if not case.get('interface'):
        caller = FROZEN / case['source']
        if digest(caller) != case['sha256']:
            raise ValueError('frozen caller changed: ' + case['source'])
        sources.append(caller)
    arguments = ['-std=c99', '-ferror-limit=0', '-I' + str(directory), '-I' + str(directory / 'src')]
    if selection['project'] == 'jansson':
        arguments += ['-DHAVE_CONFIG_H', '-I' + str(ROOT / 'scripts/corpus/support/jansson')]
    syntax = subprocess.run([compiler, '-fsyntax-only', *map(str, sources), *arguments],
                            text=True, capture_output=True, timeout=timeout)
    if syntax.returncode:
        return dict(name=name, passed=False, reason='C syntax failure', stderr=syntax.stderr)
    command = [binary, *['--checked-function=' + fn for fn in functions],
               '--checked-report=' + str(report)]
    if len(sources) > 1:
        command.append('--whole-program')
    command += [*map(str, sources), '--', *arguments]
    process = subprocess.run(command, text=True, capture_output=True, timeout=timeout)
    document = json.loads(report.read_text()) if report.is_file() else {}
    expected = dict(case, functions=functions)
    passed, reason = assess(expected, process.returncode, document)
    return dict(name=name, passed=passed, reason=reason, command=command,
                returncode=process.returncode, stderr=process.stderr, report=document)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--weavec', required=True, type=Path)
    parser.add_argument('--corpus-root', type=Path, default=ROOT / 'build/corpus')
    parser.add_argument('--clang', default=shutil.which('clang'))
    parser.add_argument('--timeout', type=float, default=600)
    parser.add_argument('--json', required=True, type=Path)
    args = parser.parse_args()
    if not args.clang:
        parser.error('a C compiler is required for independent syntax validation')
    for name, identity in IDENTITIES.items():
        if digest(FROZEN / name) != identity:
            parser.error('frozen manifest changed: ' + name)
    module = importlib.util.spec_from_file_location('checked_evaluation', ROOT / 'scripts/checked-evaluation.py')
    evaluation = importlib.util.module_from_spec(module)
    module.loader.exec_module(evaluation)
    selections = json.loads((FROZEN / 'real-modules.json').read_text())['selections']
    callers = json.loads((FROZEN / 'callers.json').read_text())['cases']
    results = []
    sources = {}
    with tempfile.TemporaryDirectory(prefix='weavec-traversal-') as temporary:
        for selection in selections:
            name = selection['project']
            try:
                directory, identities = verified_module(selection, args.corpus_root.resolve())
                sources[name] = dict(revision=selection['revision'], files=identities)
            except (OSError, ValueError, subprocess.CalledProcessError) as error:
                results.append(dict(name=name, passed=False, reason=str(error)))
                continue
            cases = [dict(name=name + '-interfaces', interface=True, expect='accepted',
                          forbidden_trust=['unsafe', 'annotation'])]
            cases += [case for case in callers if case['module'] == name]
            for case in cases:
                try:
                    result = run_case(str(args.weavec.resolve()), args.clang, selection,
                                      directory, case, Path(temporary), args.timeout,
                                      evaluation.assess)
                except (OSError, ValueError, subprocess.SubprocessError) as error:
                    result = dict(name=case['name'], passed=False, reason=str(error))
                results.append(result)
                print(('PASS ' if result['passed'] else 'FAIL ') + result['name'] +
                      (': ' + result['reason'] if result['reason'] else ''), flush=True)
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(dict(version=1, manifests=IDENTITIES,
                                        sources=sources, cases=results), indent=2) + '\n')
    return 0 if results and all(case['passed'] for case in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
