#!/usr/bin/env python3
"""RFC 0026 frozen source, transport, checkpoint and finite-state checks."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / 'test/evaluation/rfc0026'
SPEC = importlib.util.spec_from_file_location('checked_runtime', ROOT / 'scripts/checked-runtime.py')
SUPPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUPPORT)


def save(args, cases):
    (args.output / 'results.json').write_text(json.dumps(dict(
        version=1, rfc='0026', population=args.population, cases=cases,
        executable_sha256=SUPPORT.digest(args.cc if args.population == 'objects' else args.weavec)),
        indent=2) + '\n')


def cache(args):
    directory = FIXTURES / 'transport'
    SUPPORT.verify(directory)
    results = []
    with tempfile.TemporaryDirectory(prefix='weavec-buffer-cache-') as temporary:
        work = Path(temporary)
        sources = ['runtime-append.c', 'helpers.c', 'forward.c']
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
        assert cold[0] == 0, 'runtime append was rejected'
        assert warm[3].get('cache_hits') == 3 and warm[3].get('function_analyses', 0) == 0
        results.append(dict(name='equivalent-unchanged', passed=True, counters=warm[3]))
        compact = analyze('compact', False, True)
        assert compact[:3] == uncached[:3], 'compact proof differs'
        results.append(dict(name='compact-expanded-equivalent', passed=True))

        # Keep the advertised length increment and omit its initializing store.
        changed_source = original.replace('b->data[b->length] = value;', '(void)value;')
        assert changed_source != original
        (work / 'helpers.c').write_text(changed_source)
        changed, fresh = analyze('skipped-store'), analyze('skipped-store-uncached', False)
        assert changed[:3] == fresh[:3] and changed[0] == 1, 'stale initialized prefix reused'
        assert changed[3].get('function_analyses', 0) > 0
        results.append(dict(name='skipped-store-invalidates-prefix', passed=True, counters=changed[3]))
        (work / 'helpers.c').write_text(original)
        for path in (work / 'cache').glob('*.wcache'):
            path.write_bytes(path.read_bytes()[:91])
        corrupt, fresh = analyze('corrupt'), analyze('corrupt-uncached', False)
        assert corrupt[:3] == fresh[:3] and corrupt[0] == 0
        assert corrupt[3].get('cache_hits', 0) == 0
        results.append(dict(name='corrupt-cache-recomputed', passed=True))
    return results


def oracle(args):
    """Compare generated closed C clients with an independent concrete model.

    The oracle checks establishment of the buffer invariant, including length
    at most capacity. A rejected case is not automatically a C UB claim.
    The oracle enumerates physical storage and initialized byte sets. It does
    not call the analyzer's descriptor, range, relation, join or transfer code.
    False negatives and invocation failures remain visible alongside false
    proofs; no development observation is removed from the population.
    """
    SUPPORT.verify(FIXTURES)
    directory = args.output / 'oracle-sources'
    directory.mkdir(exist_ok=True)
    prelude = '''#include <stdlib.h>
struct buffer { unsigned char *data; size_t length, capacity; };
static unsigned inspect(struct buffer *b) {
    unsigned total=0;
    for(size_t i=0;i<b->length;++i) total+=b->data[i];
    if(b->capacity) b->data[b->capacity-1]=0;
    return total;
}
'''
    cases = []
    for allocated in range(1, 4):
        for capacity in range(1, 5):
            for length in range(capacity + 2):
                for mask in range(1 << allocated):
                    name = f'a{allocated}-c{capacity}-l{length}-m{mask}'
                    # The inspect loop reads every logical byte; the final
                    # store independently demands the advertised capacity.
                    safe = (length <= capacity <= allocated and
                            all(mask & (1 << i) for i in range(length)))
                    stores = ''.join(f'b.data[{i}]=7;' for i in range(allocated) if mask & (1 << i))
                    source = prelude + f'''int main(void) {{
struct buffer b={{malloc({allocated}),{length},{capacity}}};
if(!b.data)return 0;{stores}
int result=(int)inspect(&b);free(b.data);return result;
}}
'''
                    path = directory / (name + '.c')
                    path.write_text(source)
                    cases.append(dict(name=name, source=path.name, functions=['main'],
                                      expect='accepted' if safe else 'rejected',
                                      entry_requirements=0, forbidden_trust=['unsafe', 'annotation']))
    # Independently enumerate joins of concrete logical states. The backing
    # allocation stays two bytes; each possible branch must satisfy the
    # invariant and each byte read must have a real initializing store.
    states = [(0, 1), (1, 1), (2, 2), (2, 1), (1, 3), (3, 2)]
    for left, (left_length, left_capacity) in enumerate(states):
        for right, (right_length, right_capacity) in enumerate(states):
            for mask in range(4):
                name = f'join-{left}-{right}-{mask}'
                safe = all(length <= capacity <= 2 and
                           all(mask & (1 << i) for i in range(length))
                           for length, capacity in (states[left], states[right]))
                stores = ''.join(f'b.data[{i}]=7;' for i in range(2) if mask & (1 << i))
                source = prelude + f'''int main(int argc,char **argv) {{
(void)argv;struct buffer b={{malloc(2),0,2}};if(!b.data)return 0;{stores}
if(argc>1){{b.length={left_length};b.capacity={left_capacity};}}
else{{b.length={right_length};b.capacity={right_capacity};}}
int result=(int)inspect(&b);free(b.data);return result;}}
'''
                path = directory / (name + '.c')
                path.write_text(source)
                cases.append(dict(name=name, source=path.name, functions=['main'],
                                  expect='accepted' if safe else 'rejected',
                                  entry_requirements=0, forbidden_trust=['unsafe', 'annotation']))
    # Concrete release permission is independent of initialized pointer bytes:
    # heap bases may be freed, borrowed storage and interior/stale values may
    # not, and abandoning a live owned pointee does not discharge its duty.
    for kind, setup, value, release, safe in [
        ('owned-released', 'unsigned char *p=malloc(2);if(!p)return 0;', 'p', True, True),
        ('owned-lost', 'unsigned char *p=malloc(2);if(!p)return 0;', 'p', False, False),
        ('borrowed-kept', 'unsigned char local[2]={7};unsigned char *p=local;', 'p', False, True),
        ('borrowed-freed', 'unsigned char local[2]={7};unsigned char *p=local;', 'p', True, False),
        ('interior-freed', 'unsigned char *p=malloc(2);if(!p)return 0;', 'p+1', True, False),
        ('stale-freed', 'unsigned char *p=malloc(2);if(!p)return 0;free(p);', 'p', True, False),
        ('null-released', 'unsigned char *p=0;', 'p', True, True),
    ]:
        path = directory / ('ownership-' + kind + '.c')
        failure = 'free(p);' if kind.startswith(('owned-', 'interior-')) else ''
        cleanup = 'for(size_t i=0;i<b.length;++i)free(b.data[i]);' if release else ''
        path.write_text('#define ELEMENT unsigned char *\n#include "' +
                        str(FIXTURES / 'buffer.h') + '"\n' +
                        f'int main(void){{struct buffer b={{0}};{setup}' +
                        f'if(append(&b,{value})){{{failure}destroy(&b);return 0;}}' +
                        cleanup + 'destroy(&b);return 0;}\n')
        cases.append(dict(name='ownership-' + kind, source=path.name, functions=['main'],
                          expect='accepted' if safe else 'rejected',
                          entry_requirements=0, forbidden_trust=['unsafe', 'annotation']))
    manifest = directory / 'manifest.json'
    manifest.write_text(json.dumps(dict(version=1, cases=cases), indent=2) + '\n')
    # Publish every input identity before running the checker.
    identities = {p.name: SUPPORT.digest(p) for p in sorted(directory.iterdir()) if p.suffix in ('.c', '.json')}
    identities[str(FIXTURES / 'buffer.h')] = SUPPORT.digest(FIXTURES / 'buffer.h')
    (args.output / 'oracle-inputs.json').write_text(json.dumps(identities, indent=2) + '\n')
    report = args.output / 'oracle.json'
    report.unlink(missing_ok=True)
    run, _ = SUPPORT.invoke(args, 'oracle', ['python3', str(ROOT / 'scripts/checked-evaluation.py'),
                            '--weavec', str(args.weavec), '--manifest', str(manifest),
                            '--json', str(report), '--timeout', '60'])
    if run.returncode not in (0, 1):
        raise ValueError('oracle evaluator failed')
    return SUPPORT.document(report)['cases']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--population', choices=['source', 'transport', 'objects', 'cache', 'oracle', 'upstream'], required=True)
    parser.add_argument('--weavec', type=Path, required=True)
    parser.add_argument('--cc', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=600)
    args = parser.parse_args()
    # Frozen upstream include flags are relative to the repository root.
    # Resolve command-line paths first, then give all populations that base.
    args.weavec = args.weavec.resolve()
    args.cc = args.cc.resolve() if args.cc else None
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'results.json').unlink(missing_ok=True)
    os.chdir(ROOT)
    SUPPORT.FIXTURES = FIXTURES
    SUPPORT.save = save
    try:
        if args.population == 'objects':
            if args.cc is None:
                parser.error('--cc is required for objects')
            cases = SUPPORT.objects(args)
        elif args.population == 'cache':
            cases = cache(args)
        elif args.population == 'oracle':
            cases = oracle(args)
        elif args.population == 'upstream':
            (args.output / 'cases.json').unlink(missing_ok=True)
            cases = SUPPORT.upstream(args)
        else:
            directory = FIXTURES if args.population == 'source' else FIXTURES / 'transport'
            SUPPORT.verify(directory)
            report = args.output / 'cases.json'
            report.unlink(missing_ok=True)
            run, _ = SUPPORT.invoke(args, 'evaluation', ['python3', str(ROOT / 'scripts/checked-evaluation.py'),
                                    '--weavec', str(args.weavec), '--manifest', str(directory / 'manifest.json'),
                                    '--json', str(report), '--timeout', '60'])
            if run.returncode not in (0, 1):
                raise ValueError('evaluator failed')
            cases = SUPPORT.document(report)['cases']
        save(args, cases)
        return 0 if all(case['passed'] for case in cases) else 1
    except (AssertionError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        save(args, [dict(name=args.population, passed=False, reason=str(error))])
        print('FAIL ' + args.population + ': ' + str(error), flush=True)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
