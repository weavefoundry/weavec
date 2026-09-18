#!/usr/bin/env python3
"""RFC 0029 frozen workflow, object and checkpoint acceptance checks."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / 'test/evaluation/rfc0029'
OBJECT_POPULATIONS = (
    'transport',
    'serializer',
    'readers',
    'construction',
    'mutual-construction',
    'construction-helpers',
    'output-construction',
    'reader-construction',
    'mutable-reader-construction',
    'numeric-input',
    'cursor-readers',
    'recursive-contexts',
    'recursive-state-cases',
    'recursive-output-cases',
    'record-arrays',
    'writer-return-aliases',
    'writer-return-expressions',
    'writer-position-bounds',
    'writer-forwarding',
    'container-local-frames',
    'container-call-frames',
    'mixed-allocation-ledger',
    'reallocation-ledger-failures',
    'scalar-write-offsets',
    'bounded-string-cursor',
    'cast-reader-intervals',
    'reader-index-loops',
    'paired-reader-counters',
    'character-pointer-slots',
    'pointer-reader-offsets',
    'initialized-spans',
    'span-outputs',
    'reverse-byte-writes',
    'span-counts',
    'span-count-joins',
    'local-callee-copies',
    'reverse-initialization',
    'conditional-count-arguments',
    'initialized-advance',
    'advance-outcomes',
    'counter-reset-ranges-reviewed',
    'guarded-advance',
    'guarded-cursor-bounds',
    'span-advances',
    'paired-cursor-loops',
    'independent-cursors',
    'fixed-span-steps',
    'helper-cursor-pairs',
    'byte-cursor-content',
    'byte-cursor-static',
    'byte-cursor-forwarding',
    'byte-cursor-frames',
    'byte-global-frames',
    'byte-helper-frames',
    'byte-comparisons',
    'byte-loop-partitions',
    'byte-switch-partitions-reviewed',
    'byte-callee-frames',
    'comparison-container-frames-reviewed',
    'numeric-container-frames',
    'temporary-release-frames',
    'container-outcome-frames',
    'string-length-copies',
    'container-alias-outputs',
    'floating-call-premises',
    'singleton-link-ownership',
    'pointer-difference-sizes',
    'payload-publication',
    'payload-write-frames',
    'payload-early-exits',
    'payload-reader-returns',
    'payload-relocation',
    'attached-payload-transfer',
    'numeric-text',
    'numeric-scans',
    'numeric-scan-locals',
    'numeric-short-circuit',
    'cursor-envelope-joins-reviewed',
    'pointer-count-readers',
    'shifted-pointee-facts',
    'buffer-entry-intervals-reviewed',
    'in-place-extension',
    'anonymous-private-state',
    'container-value-guards',
    'zero-counters',
    'zero-frames',
    'combined-callback-cases',
    'mutual-cases',
    'recursive-cases',
    'recursive-writer-reviewed',
    'writer-transport',
    'corpus-regressions',
    'recursive-transport',
    'output-transport',
)


def load_module(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), ROOT / 'scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


EVALUATION = load_module('checked-evaluation')
REPORT = load_module('checked-report')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def document(path):
    return json.loads(path.read_text()) if path.is_file() else {}


def verify(directory):
    inventory = document(directory / 'frozen-sha256.json')
    if not inventory:
        raise ValueError('missing frozen inventory: ' + str(directory))
    for name, expected in inventory.items():
        if digest(directory / name) != expected:
            raise ValueError('frozen input changed: ' + str(directory / name))
    return inventory


def invoke(args, label, command, cwd=None, timeout=None):
    for binary, expected in args.identities.items():
        if digest(Path(binary)) != expected:
            raise ValueError('executable changed during validation: ' + binary)
    for argument in command:
        if argument.startswith(('--checked-report=', '-fweavec-checked-report=', '--analysis-stats=')):
            Path(argument.split('=', 1)[1]).unlink(missing_ok=True)
    start = time.monotonic()
    process = subprocess.Popen(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=args.timeout if timeout is None else timeout)
    except subprocess.TimeoutExpired:
        if os.name == 'posix':
            os.killpg(process.pid, signal.SIGKILL)
        else:
            process.kill()
        stdout, stderr = process.communicate()
        (args.output / (label + '.log')).write_text(stdout + stderr)
        raise
    run = subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
    (args.output / (label + '.log')).write_text(run.stdout + run.stderr)
    return run, time.monotonic() - start


def population_manifest(directory):
    verify(directory)
    manifest = directory / 'manifest.json'
    variant = {'readers': 'reviewed', 'cursor-readers': 'audited', 'zero-counters': 'audited', 'traversal': 'reviewed', 'writer-forwarding': 'reviewed', 'scalar-write-offsets': 'audited',
               'container-value-guards': 'reviewed', 'byte-cursor-frames': 'reviewed', 'corpus-regressions': 'discovered'}.get(directory.name)
    if variant:
        inventory = document(directory / (variant + '-sha256.json'))
        if not inventory:
            raise ValueError('missing reviewed inventory: ' + str(directory))
        for name, expected in inventory.items():
            if digest(directory / name) != expected:
                raise ValueError('reviewed input changed: ' + name)
        manifest = directory / (variant + '-manifest.json')
    return manifest


def verify_upstream(directory):
    if (directory / 'upstream-identity.json').is_file():
        identity = document(directory / 'upstream-identity.json')
        upstream = ROOT / 'build/corpus/cJSON-program'
        revision = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse', 'HEAD'], text=True).strip()
        if revision != identity['commit']:
            raise ValueError('upstream revision differs')
        for name, expected in identity['files'].items():
            if digest(upstream / name) != expected:
                raise ValueError('upstream source differs: ' + name)


def source(args, directory):
    manifest = population_manifest(directory)
    verify_upstream(directory)
    path = args.output / 'source.json'
    path.unlink(missing_ok=True)
    # The inner runner gives each syntax check and checker its own deadline.
    # Do not impose that same single-process deadline on the whole population.
    population_timeout = len(document(manifest)['cases']) * (2 * args.timeout + 30)
    run, seconds = invoke(args, 'source', [args.python, str(ROOT / 'scripts/checked-evaluation.py'),
        '--weavec', str(args.weavec), '--clang', str(args.clang),
        '--manifest', str(manifest), '--json', str(path), '--timeout', str(args.timeout)],
        timeout=population_timeout)
    result = document(path)
    if not result.get('cases'):
        raise ValueError('source evaluation did not produce cases')
    if run.returncode not in (0, 1):
        raise ValueError('source runner failed abnormally')
    for case in result['cases']:
        yield dict(name=case['name'], passed=case['passed'], reason=case.get('reason', ''),
                   seconds=seconds, report=str(path))


def objects(args):
    for population in ([args.object_population] if getattr(args, 'object_population', None)
                       else OBJECT_POPULATIONS):
        directory = FIXTURES / population
        for case in document(population_manifest(directory))['cases']:
            case = dict(case, functions=case.get('functions', [case.get('function')]),
                        sources=case.get('sources', [case.get('source')]))
            if 'main' not in case['functions'] and population != 'zero-counters':
                # Only executable clients belong to this population. The
                # source population independently checks generic interfaces.
                if case['name'] != 'runtime-hex' and population not in (
                        'construction', 'mutual-construction', 'construction-helpers',
                        'corpus-regressions', 'output-construction', 'reader-construction', 'mutable-reader-construction', 'recursive-writer-reviewed'):
                    continue
                if population != 'recursive-writer-reviewed':
                    case = dict(case, functions=['main'])
            with tempfile.TemporaryDirectory(prefix='weavec-workflow-objects-') as temporary:
                work = Path(temporary)
                for path in directory.glob('*'):
                    if path.suffix in ('.c', '.h'):
                        (work / path.name).write_bytes(path.read_bytes())
                if population == 'zero-counters':
                    # These frozen cases select named closed functions rather
                    # than main. Supply an unused entry point for the ordinary
                    # executable link; keep every original selected function.
                    (work / 'weavec-test-entry.c').write_text('int main(void) { return 0; }\n')
                    case = dict(case, sources=[*case['sources'], 'weavec-test-entry.c'])
                stages = []
                for index, name in enumerate(case['sources']):
                    label = population + '-' + case['name'] + '-compile-' + str(index)
                    # Ordinary diagnostics may already detect a frozen mutant.
                    # Produce its object anyway so checked linking independently
                    # verifies the selected obligation; warning severity never
                    # changes whether a checked contract is complete.
                    run, seconds = invoke(args, label, [str(args.cc), '-std=c11', '-Wno-error=weavec', '-c', name,
                                                        '-o', str(index) + '.o'], work)
                    stages.append(dict(stage=label, seconds=seconds, returncode=run.returncode))
                    if run.returncode != 0 or not (work / (str(index) + '.o.weavec')).is_file():
                        raise ValueError('ordinary object compilation failed: ' + label)
                report = args.output / (population + '-' + case['name'] + '.json')
                run, seconds = invoke(args, population + '-' + case['name'] + '-link',
                    [str(args.cc), *['-fweavec-checked-function=' + name for name in case['functions']],
                     '-fweavec-checked-report=' + str(report),
                     *[str(i) + '.o' for i in range(len(case['sources']))], '-o', 'client'], work)
                passed, reason = EVALUATION.assess(case, run.returncode, document(report))
                if passed and case['expect'] == 'accepted' and not (work / 'client').is_file():
                    passed, reason = False, 'checked link produced no executable'
                yield dict(name=population + '-' + case['name'], passed=passed, reason=reason,
                           stages=stages, seconds=seconds, report=str(report))


def upstream_cases():
    for population in ('upstream', 'upstream-extended', 'upstream-construction', 'upstream-static-inputs', 'upstream-lifetime-audit'):
        directory = FIXTURES / population
        manifest = population_manifest(directory)
        verify_upstream(directory)
        for case in document(manifest)['cases']:
            yield population + '-' + case['name'], directory, case


def upstream_objects(args):
    with tempfile.TemporaryDirectory(prefix='weavec-upstream-objects-') as temporary:
        work = Path(temporary)
        compiled = {}
        for label, directory, case in upstream_cases():
            objects = []
            stages = []
            for name in case['sources']:
                path = (directory / name).resolve()
                if path not in compiled:
                    target = work / (str(len(compiled)) + '.o')
                    run, seconds = invoke(args, label + '-compile-' + str(len(compiled)),
                        [str(args.cc), '-std=c11', '-Wno-error=weavec', '-c', str(path),
                         '-o', str(target)], work)
                    if run.returncode or not Path(str(target) + '.weavec').is_file():
                        raise ValueError('upstream ordinary object compilation failed: ' + label)
                    compiled[path] = target
                    stages.append(dict(source=str(path), seconds=seconds,
                                       object_bytes=target.stat().st_size,
                                       sidecar_bytes=Path(str(target) + '.weavec').stat().st_size))
                objects.append(str(compiled[path]))
            report = args.output / (label + '.json')
            binary = work / label
            run, seconds = invoke(args, label + '-link',
                [str(args.cc), *['-fweavec-checked-function=' + name for name in case['functions']],
                 '-fweavec-checked-report=' + str(report), *objects, '-lm', '-o', str(binary)], work)
            passed, reason = EVALUATION.assess(case, run.returncode, document(report))
            if passed and case['expect'] == 'accepted' and not binary.is_file():
                passed, reason = False, 'checked link produced no executable'
            yield dict(name=label, passed=passed, reason=reason, stages=stages,
                       seconds=seconds, report=str(report))


def upstream_checkpoints(args):
    for label, directory, case in upstream_cases():
        with tempfile.TemporaryDirectory(prefix='weavec-upstream-cache-') as temporary:
            cache = Path(temporary) / 'cache'
            observations = {}
            for phase in ('uncached', 'cold', 'warm', 'compact'):
                report = args.output / (label + '-' + phase + '.json')
                stats = args.output / (label + '-' + phase + '.stats.json')
                command = [str(args.weavec), '--whole-program',
                           *['--checked-function=' + name for name in case['functions']],
                           '--checked-report=' + str(report), '--analysis-stats=' + str(stats)]
                if phase != 'uncached':
                    command.append('--analysis-cache=' + str(cache))
                if phase == 'compact':
                    command.append('--checked-report-format=compact')
                command += [*[str((directory / name).resolve()) for name in case['sources']],
                            '--', '-std=c11']
                run, seconds = invoke(args, label + '-' + phase, command)
                doc = document(report)
                if run.returncode not in (0, 1) or not doc:
                    raise ValueError('upstream checkpoint analysis failed: ' + label + '-' + phase)
                expanded = REPORT.expand_report(doc)
                counters = document(stats).get('counters', {})
                observations[phase] = dict(returncode=run.returncode, report=expanded,
                                           stderr=run.stderr, counters=counters, seconds=seconds,
                                           report_bytes=report.stat().st_size,
                                           checkpoint_bytes=sum(p.stat().st_size for p in cache.glob('*.wcache')))
            first = observations['uncached']
            passed, reason = EVALUATION.assess(case, first['returncode'], first['report'])
            if any((entry['returncode'], entry['report'], entry['stderr']) !=
                   (first['returncode'], first['report'], first['stderr'])
                   for entry in observations.values()):
                passed, reason = False, 'canonical upstream cached report differs'
            if any(observations[phase]['counters'].get('cache_hits', 0) < len(case['sources']) or
                   observations[phase]['counters'].get('function_analyses', 0) != 0
                   for phase in ('warm', 'compact')):
                passed, reason = False, 'unchanged upstream workflow did not fully reuse checkpoints'
            for entry in observations.values():
                del entry['report'], entry['stderr']
            yield dict(name=label, passed=passed, reason=reason, observations=observations)


def upstream_oracle(args):
    directory = FIXTURES / 'upstream-oracle'
    verify(directory)
    verify_upstream(directory)
    with tempfile.TemporaryDirectory(prefix='weavec-upstream-oracle-') as temporary:
        binary = Path(temporary) / 'oracle'
        run, _ = invoke(args, 'compile', [str(args.clang), '-std=c11', '-g', '-O1',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            str(directory / 'oracle.c'), str(ROOT / 'build/corpus/cJSON-program/cJSON.c'),
            '-lm', '-o', str(binary)])
        if run.returncode != 0:
            raise ValueError('upstream oracle compilation failed')
        run, seconds = invoke(args, 'oracle', ['env', 'ASAN_OPTIONS=detect_leaks=0',
            'UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1', str(binary)])
        if run.returncode != 0:
            raise ValueError('upstream allocation ledger or sanitizer failed')
        observation = json.loads(run.stdout)
        if observation != dict(documents=8, runs=68, injected_failures=52):
            raise ValueError('upstream oracle population changed')
        yield dict(name='upstream-allocation-failure-oracle', passed=True,
                   seconds=seconds, observation=observation)
    yield from upstream_lifetime_oracle(args)


def upstream_lifetime_oracle(args):
    directory = FIXTURES / 'upstream-lifetime-audit'
    verify(directory)
    verify_upstream(directory)
    with tempfile.TemporaryDirectory(prefix='weavec-upstream-lifetime-') as temporary:
        for storage in ('stack', 'static'):
            binary = Path(temporary) / storage
            command = [str(args.clang), '-std=c11', '-g', '-O1',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            if storage == 'static':
                command.append('-DSTATIC_INPUT')
            command += [str(directory / 'oracle.c'),
                        str(ROOT / 'build/corpus/cJSON-program/cJSON.c'),
                        '-lm', '-o', str(binary)]
            run, _ = invoke(args, 'lifetime-' + storage + '-compile', command)
            if run.returncode:
                raise ValueError('upstream lifetime oracle compilation failed')
            run, seconds = invoke(args, 'lifetime-' + storage,
                ['env', 'ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=1',
                 'UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1', str(binary)])
            passed = (run.returncode == 0 if storage == 'static' else
                      run.returncode != 0 and 'stack-use-after-' in run.stderr)
            yield dict(name='upstream-lifetime-' + storage, passed=passed,
                       seconds=seconds, returncode=run.returncode)


def construction_oracle(args):
    """Execute the frozen finite clients with every allocation failure point.

    This concrete ledger is independent of abstract footprints and progress
    graphs. It corroborates only these clients, not arbitrary runtime lengths.
    """
    for population, positive in (('construction', 'build'),
                                 ('mutual-construction', 'forwarding'),
                                 ('construction-helpers', 'build'),
                                 ('output-construction', 'build'),
                                 ('reader-construction', 'build'),
                                 ('mutable-reader-construction', 'build')):
        directory = FIXTURES / population
        verify(directory)
        variants = [(positive, 0), ('leak', 74)]
        if (directory / 'double.c').is_file():
            variants.append(('double', 73))
        for variant, code in variants:
            with tempfile.TemporaryDirectory(prefix='weavec-construction-oracle-') as temporary:
                work = Path(temporary)
                source = r'''
#include <stdio.h>
#include <stdlib.h>
static void *live[128];
static size_t calls, fail_at;
static void *oracle_calloc(size_t n, size_t size) {
    if (++calls == fail_at) return NULL;
    void *p = calloc(n, size);
    if (!p) { fputs("unexpected host allocation failure\n", stderr); exit(75); }
    for (size_t i = 0; i < 128; ++i) {
        if (!live[i]) { live[i] = p; return p; }
    }
    exit(76);
}
static void oracle_free(void *p) {
    if (!p) return;
    for (size_t i = 0; i < 128; ++i) {
        if (live[i] == p) { live[i] = NULL; free(p); return; }
    }
    fputs("release is not live\n", stderr);
    for (size_t i = 0; i < 128; ++i) free(live[i]);
    exit(73);
}
#define calloc oracle_calloc
#define free oracle_free
#define main frozen_client
'''
                source += '#include ' + json.dumps(str(directory / (variant + '.c'))) + '\n'
                source += r'''
#undef main
#undef free
#undef calloc
int main(void) {
    size_t count = 0;
    for (fail_at = 0; fail_at <= count; ++fail_at) {
        calls = 0;
        if (frozen_client() != 0) return 77;
        if (fail_at == 0) count = calls;
        for (size_t i = 0; i < 128; ++i) {
            if (live[i]) {
                fputs("live allocations remain\n", stderr);
                for (size_t j = 0; j < 128; ++j) free(live[j]);
                return 74;
            }
        }
    }
    printf("checked %zu allocation failure points\n", count);
    return 0;
}
'''
                (work / 'oracle.c').write_text(source)
                label = population + '-' + variant + '-oracle'
                compiled, _ = invoke(args, label + '-compile',
                    [str(args.clang), '-std=c11', '-fsanitize=address,undefined',
                     '-fno-sanitize-recover=all', 'oracle.c', '-o', 'oracle'], work)
                if compiled.returncode:
                    raise ValueError('concrete oracle did not compile: ' + label)
                run, seconds = invoke(args, label, [str(work / 'oracle')], work)
                marker = {0: 'allocation failure points', 73: 'release is not live',
                          74: 'live allocations remain'}[code]
                passed = run.returncode == code and marker in run.stdout + run.stderr
                # Repeating recursive cleanup first reads the freed child's
                # links, before it reaches the ledger's second release.
                if population == 'construction-helpers' and variant == 'double':
                    passed |= run.returncode != 0 and \
                        'ERROR: AddressSanitizer: heap-use-after-free' in run.stderr
                count = re.search(r'checked (\d+) allocation failure points', run.stdout)
                yield dict(name=label, passed=passed, returncode=run.returncode,
                           seconds=seconds,
                           allocation_failure_points=int(count[1]) if count else None,
                           reason='' if passed else 'concrete ledger differs')



def reader_index_oracle(args):
    """Exhaust the eight-bit arithmetic analogue, independently of the checker."""
    verify(FIXTURES / 'reader-index-loops')
    cases = 0
    accesses = 0
    for capacity in range(256):
        for position in range(capacity + 1):
            index = 0
            while ((position + index) & 255) < capacity:
                if not (index < capacity - position and position + index < 256):
                    raise ValueError('strict unit-stride induction differs from concrete arithmetic')
                accesses += 1
                index = (index + 1) & 255
                if index == 0:
                    raise ValueError('reader index unexpectedly wrapped')
            if index != capacity - position:
                raise ValueError('reader index exited at the wrong boundary')
            cases += 1
    yield dict(name='reader-index-eight-bit-oracle', passed=True,
               cases=cases, accesses=accesses, reason='')


def writer_oracle(args):
    """Check finite prefix contents and capacity canaries independently."""
    directory = FIXTURES / 'recursive-writer-reviewed'
    verify(directory)
    for variant, expected in (('emit', 0), ('false-prefix', 74)):
        with tempfile.TemporaryDirectory(prefix='weavec-writer-oracle-') as temporary:
            work = Path(temporary)
            (work / 'fixture.c').write_bytes((directory / (variant + '.c')).read_bytes())
            (work / 'oracle.c').write_text(r'''
#include <stdio.h>
#include <string.h>
#define main fixture_main
#include "fixture.c"
#undef main
int main(void) {
    unsigned char input[33], output[33];
    size_t cases = 0;
    for (size_t i = 0; i < sizeof input; ++i) input[i] = (unsigned char)(i + 1);
    for (size_t n = 0; n <= 32; ++n)
        for (size_t capacity = 0; capacity <= 32; ++capacity)
            for (size_t initial = 0; initial <= capacity; ++initial) {
                memset(output, 0xa5, sizeof output);
                struct writer w = {7, output, initial, capacity};
                int result = emit(input, n, &w);
                size_t copied = n < capacity - initial ? n : capacity - initial;
                if (w.used != initial + copied || w.capacity != capacity ||
                    w.data != output || w.flags != 7 || result != (copied == n)) {
                    puts("prefix or capacity differs"); return 74;
                }
                for (size_t i = 0; i < sizeof output; ++i) {
                    unsigned char expected = i >= initial && i < initial + copied
                        ? input[i - initial] : 0xa5;
                    if (output[i] != expected) {
                        puts("prefix or capacity differs"); return 74;
                    }
                }
                ++cases;
            }
    printf("checked %zu finite prefix cases\n", cases);
    return 0;
}
''')
            label = 'writer-oracle-' + variant
            compiled, _ = invoke(args, label + '-compile',
                [str(args.clang), '-std=c11', '-fsanitize=address,undefined',
                 '-fno-sanitize-recover=all', 'oracle.c', '-o', 'oracle'], work)
            if compiled.returncode:
                raise ValueError('writer oracle did not compile')
            run, seconds = invoke(args, label, [str(work / 'oracle')], work)
            count = re.search(r'checked (\d+) finite prefix cases', run.stdout)
            passed = run.returncode == expected and (
                count is not None if expected == 0 else
                'prefix or capacity differs' in run.stdout)
            yield dict(name=label, passed=passed, returncode=run.returncode,
                       seconds=seconds, finite_cases=int(count[1]) if count else None,
                       reason='' if passed else 'concrete prefix oracle differs')


def reader_oracle(args):
    directory = FIXTURES / 'cursor-readers'
    population_manifest(directory)
    for variant, expected in [('library', 0), ('partial', 0), ('escape', 74)]:
        with tempfile.TemporaryDirectory(prefix='weavec-reader-oracle-') as temporary:
            work = Path(temporary)
            for name in ['reader.h', variant + '.c']:
                (work / name).write_bytes((directory / name).read_bytes())
            (work / 'oracle.c').write_text(r'''
#include "reader.h"
#include <stdio.h>
int main(void) {
    unsigned char input[33];
    size_t cases=0;
    for (unsigned first=0; first<2; ++first) {
        for (size_t i=0; i<sizeof input; ++i) input[i]=(unsigned char)(first+i);
        for (size_t size=0; size<=32; ++size)
            for (size_t pos=0; pos<=size; ++pos) {
                struct reader r={7,input,size,pos};
                int result=take(&r);
                size_t next=pos<size?pos+1:pos;
                if (r.pos!=next || r.limit!=size || r.data!=input || r.depth!=7 ||
                    (pos==size && result!=-1)) {
                    puts("cursor invariant differs"); return 74;
                }
                for (size_t i=0; i<sizeof input; ++i)
                    if (input[i]!=(unsigned char)(first+i)) {
                        puts("cursor invariant differs"); return 74;
                    }
                ++cases;
            }
    }
    struct reader empty={0,0,0,0};
    if (take(&empty)!=-1 || empty.pos!=0) return 75;
    printf("checked %zu finite cursor cases\n",cases+1);
    return 0;
}
''')
            label = 'reader-oracle-' + variant
            compiled, _ = invoke(args, label + '-compile',
                [str(args.clang), '-std=c11', '-fsanitize=address,undefined',
                 '-fno-sanitize-recover=all', 'oracle.c', variant + '.c', '-o', 'oracle'], work)
            if compiled.returncode:
                raise ValueError('reader oracle did not compile')
            run, seconds = invoke(args, label, [str(work / 'oracle')], work)
            count = re.search(r'checked (\d+) finite cursor cases', run.stdout)
            passed = run.returncode == expected and (
                count is not None if expected == 0 else 'cursor invariant differs' in run.stdout)
            yield dict(name=label, passed=passed, returncode=run.returncode,
                       seconds=seconds, finite_cases=int(count[1]) if count else None,
                       reason='' if passed else 'concrete cursor oracle differs')


def checkpoints(args, population='transport'):
    directory = FIXTURES / population
    verify(directory)
    with tempfile.TemporaryDirectory(prefix='weavec-workflow-cache-') as temporary:
        work = Path(temporary)
        client = 'good.c' if population == 'byte-cursor-content' else 'client.c'
        payloads = population == 'payload-write-frames'
        sources = {'client.c': client, 'library.c': 'good.c' if payloads else 'library.c'}
        if payloads:
            sources['drop.c'] = 'drop.c'
        for path in directory.glob('*.h'):
            (work / path.name).write_bytes(path.read_bytes())
        for name, source in sources.items():
            (work / name).write_bytes((directory / source).read_bytes())

        def analyze(label, cached=True, compact=False):
            report = args.output / (label + '.json')
            stats = args.output / (label + '.stats.json')
            command = [str(args.weavec), '--whole-program', '--checked-function=main',
                       '--checked-report=' + str(report), '--analysis-stats=' + str(stats)]
            if cached:
                command.append('--analysis-cache=' + str(work / 'cache'))
            if compact:
                command.append('--checked-report-format=compact')
            command += [*[str(work / name) for name in sources], '--', '-std=c11']
            run, seconds = invoke(args, label, command)
            doc = document(report)
            if run.returncode not in (0, 1) or not doc:
                raise ValueError('analysis failed: ' + label)
            return (run.returncode, REPORT.expand_report(doc), run.stderr), document(stats).get('counters', {}), seconds

        cold, warm, uncached = analyze('cold'), analyze('warm'), analyze('uncached', False)
        case = next(case for case in document(population_manifest(directory))['cases']
                    if case['name'] == ('good' if population == 'byte-cursor-content' or payloads else
                                        'cursor-reader-client' if population == 'cursor-readers' else
                                        'composed-client' if population == 'transport' else 'client'))
        passed, reason = EVALUATION.assess(case, cold[0][0], cold[0][1])
        if not passed:
            raise ValueError('closed checkpoint client failed: ' + reason)
        if cold[0] != warm[0] or cold[0] != uncached[0] or cold[0][0] != 0:
            raise ValueError('valid cached workflow failed or differs from uncached analysis')
        if warm[1].get('cache_hits', 0) < 2 or warm[1].get('function_analyses', 0) != 0:
            raise ValueError('unchanged workflow did not fully reuse its checkpoints')
        yield dict(name='cold-warm-uncached-equivalent', passed=True, counters=warm[1])
        compact = analyze('compact', compact=True)
        if compact[0] != cold[0] or compact[1].get('function_analyses', 0):
            raise ValueError('compact checkpoint report differs')
        yield dict(name='compact-equivalent', passed=True, counters=compact[1])
        path = work / 'library.c'
        original = path.read_text()
        mutation = (original.replace('n->text=p;', 'n->text=p;n->name=p;')
                    if payloads else
                    original.replace("p[i]=='\\\\'", "p[i]=='b'")
                    if population == 'byte-cursor-content' else
                    original.replace('int value=r->data[r->pos];', 'int value=r->data[r->limit+1];')
                    if population == 'cursor-readers' else
                    original.replace('w->data[w->used]=input[0];', '(void)input[0];')
                    if population == 'writer-transport' else
                    original.replace('destroy(p->next);', '(void)p->next;')
                    if population != 'transport'
                    else original.replace('destroy_even(p->right);', '(void)p->right;'))
        if mutation == original:
            raise ValueError('contract mutation did not modify source')
        path.write_text(mutation)
        changed, fresh = analyze('changed'), analyze('changed-uncached', False)
        if changed[0] != fresh[0] or changed[0][0] != 1 or not changed[1].get('function_analyses'):
            raise ValueError('changed recursive member reused a stale proof')
        yield dict(name='changed-group-invalidates-proof', passed=True, counters=changed[1])
        path.write_text(original)
        if payloads:
            destructor = work / 'drop.c'
            original_cleanup = destructor.read_text()
            changed_cleanup = original_cleanup.replace('free(n->text)', '(void)n->text')
            if changed_cleanup == original_cleanup:
                raise ValueError('cleanup mutation did not modify source')
            destructor.write_text(changed_cleanup)
            changed, fresh = analyze('changed-cleanup'), analyze('changed-cleanup-uncached', False)
            if changed[0] != fresh[0] or changed[0][0] != 1 or not changed[1].get('function_analyses'):
                raise ValueError('changed imported ownership reused a stale proof')
            yield dict(name='changed-ownership-invalidates-proof', passed=True, counters=changed[1])
            destructor.write_text(original_cleanup)
        if population == 'byte-cursor-content':
            client_path = work / 'client.c'
            good = client_path.read_text()
            client_path.write_text(good.replace("'b'", "'\\\\'"))
            changed, fresh = analyze('changed-bytes'), analyze('changed-bytes-uncached', False)
            if changed[0] != fresh[0] or changed[0][0] != 1 or not changed[1].get('function_analyses'):
                raise ValueError('changed input bytes reused a stale proof')
            yield dict(name='changed-bytes-invalidates-proof', passed=True, counters=changed[1])
            client_path.write_text(good)
        for checkpoint in (work / 'cache').glob('*.wcache'):
            checkpoint.write_bytes(checkpoint.read_bytes()[:91])
        corrupt = analyze('corrupt')
        if corrupt[0] != uncached[0] or corrupt[1].get('cache_hits', 0):
            raise ValueError('corrupt checkpoint was not recomputed')
        yield dict(name='corrupt-checkpoint-recomputed', passed=True)
        for index, name in enumerate(sources):
            run, _ = invoke(args, 'compile-' + str(index),
                            [str(args.cc), '-std=c11', '-c', name, '-o', str(index) + '.o'], work)
            if run.returncode:
                raise ValueError('object compilation failed')
        metadata = work / '1.o.weavec'
        text = metadata.read_text()
        version = re.match(r'weavec-summaries (\d+)\n', text)
        if not version:
            raise ValueError('missing sidecar version')
        metadata.write_text(text.replace(version[0], 'weavec-summaries ' + str(int(version[1])-1) + '\n', 1))
        run, _ = invoke(args, 'old-sidecar', [str(args.cc), '-fweavec-checked-function=main',
                                             *[str(index) + '.o' for index in range(len(sources))],
                                             '-o', 'old'], work)
        if run.returncode != 1 or 'unsupported format' not in run.stderr:
            raise ValueError('old sidecar was accepted')
        yield dict(name='old-sidecar-rejected', passed=True)


def main():
    import sys
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--weavec', required=True, type=Path)
    parser.add_argument('--cc', required=True, type=Path)
    parser.add_argument('--clang', default='clang', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--population', choices=['source', 'transport', 'serializer', 'readers', 'offsets', 'traversal', 'construction', 'mutual-construction', 'construction-helpers', 'output-construction', 'reader-construction', 'mutable-reader-construction', 'numeric-input', 'cursor-readers', 'recursive-contexts', 'recursive-state-cases', 'recursive-output-cases', 'record-arrays', 'writer-return-aliases', 'writer-return-expressions', 'writer-position-bounds', 'writer-forwarding', 'container-local-frames', 'container-call-frames', 'mixed-allocation-ledger', 'reallocation-ledger-failures', 'scalar-write-offsets', 'bounded-string-cursor', 'cast-reader-intervals', 'reader-index-loops', 'paired-reader-counters', 'character-pointer-slots', 'pointer-reader-offsets', 'initialized-spans', 'span-outputs', 'reverse-byte-writes', 'span-counts', 'span-count-joins', 'local-callee-copies', 'reverse-initialization', 'conditional-count-arguments', 'initialized-advance', 'advance-outcomes', 'counter-reset-ranges-reviewed', 'guarded-advance', 'guarded-cursor-bounds', 'span-advances', 'paired-cursor-loops', 'independent-cursors', 'fixed-span-steps', 'helper-cursor-pairs', 'byte-cursor-content', 'byte-cursor-static', 'byte-cursor-forwarding', 'byte-cursor-frames', 'byte-global-frames', 'byte-helper-frames', 'byte-comparisons', 'byte-loop-partitions', 'byte-switch-partitions-reviewed', 'byte-callee-frames', 'comparison-container-frames-reviewed', 'numeric-container-frames', 'temporary-release-frames', 'container-outcome-frames', 'string-length-copies', 'container-alias-outputs', 'floating-call-premises', 'singleton-link-ownership', 'pointer-difference-sizes', 'payload-publication', 'payload-write-frames', 'payload-early-exits', 'payload-reader-returns', 'payload-relocation', 'attached-payload-transfer', 'payload-cache', 'byte-cache', 'numeric-text', 'numeric-scans', 'numeric-scan-locals', 'numeric-short-circuit', 'cursor-envelope-joins-reviewed', 'pointer-count-readers', 'shifted-pointee-facts', 'buffer-entry-intervals-reviewed', 'in-place-extension', 'anonymous-private-state', 'container-value-guards', 'zero-counters', 'zero-frames', 'combined-callback-cases', 'mutual-cases', 'recursive-cases', 'recursive-writer-reviewed', 'writer-transport', 'writer-cache', 'cursor-cache', 'cursor-oracle', 'reader-index-oracle', 'writer-oracle', 'construction-oracle', 'corpus-regressions', 'recursive-transport', 'recursive-cache', 'output-transport', 'output-cache', 'upstream', 'upstream-extended', 'upstream-construction', 'upstream-static-inputs', 'upstream-lifetime-audit', 'upstream-lifetime-oracle', 'upstream-oracle', 'upstream-objects', 'upstream-cache', 'objects', 'cache'], required=True)
    parser.add_argument('--object-population', choices=OBJECT_POPULATIONS,
                        help='Run one object population; omission runs all frozen object cases.')
    parser.add_argument('--timeout', default=600, type=float)
    args = parser.parse_args()
    args.python = sys.executable
    args.weavec, args.cc, args.output = args.weavec.resolve(), args.cc.resolve(), args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    args.identities = {str(binary): digest(binary) for binary in (args.weavec, args.cc)}
    results = []
    try:
        if args.population == 'objects':
            cases = objects(args)
        elif args.population == 'cache':
            cases = checkpoints(args)
        elif args.population == 'construction-oracle':
            cases = construction_oracle(args)
        elif args.population == 'upstream-objects':
            cases = upstream_objects(args)
        elif args.population == 'upstream-cache':
            cases = upstream_checkpoints(args)
        elif args.population == 'upstream-lifetime-oracle':
            cases = upstream_lifetime_oracle(args)
        elif args.population == 'upstream-oracle':
            cases = upstream_oracle(args)
        elif args.population == 'recursive-cache':
            cases = checkpoints(args, 'recursive-transport')
        elif args.population == 'output-cache':
            cases = checkpoints(args, 'output-transport')
        elif args.population == 'reader-index-oracle':
            cases = reader_index_oracle(args)
        elif args.population == 'cursor-oracle':
            cases = reader_oracle(args)
        elif args.population == 'payload-cache':
            cases = checkpoints(args, 'payload-write-frames')
        elif args.population == 'byte-cache':
            cases = checkpoints(args, 'byte-cursor-content')
        elif args.population == 'cursor-cache':
            cases = checkpoints(args, 'cursor-readers')
        elif args.population == 'writer-cache':
            cases = checkpoints(args, 'writer-transport')
        elif args.population == 'writer-oracle':
            cases = writer_oracle(args)
        else:
            directory = FIXTURES if args.population == 'source' else FIXTURES / args.population
            cases = source(args, directory)
        for result in cases:
            results.append(result)
            print(('PASS ' if result['passed'] else 'FAIL ') + result['name'] + ': ' + result.get('reason', ''), flush=True)
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired, subprocess.CalledProcessError) as error:
        results.append(dict(name=args.population, passed=False, reason=str(error)))
        print('FAIL ' + args.population + ': ' + str(error), flush=True)
    (args.output / 'results.json').write_text(json.dumps(dict(version=1, binaries=args.identities, cases=results), indent=2) + '\n')
    return 0 if results and all(result['passed'] for result in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
