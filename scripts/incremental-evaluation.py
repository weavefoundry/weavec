#!/usr/bin/env python3
"""RFC 0020: cold/warm equivalence and adversarial cache invalidation."""
import argparse
import concurrent.futures
import importlib.util
import io
import json
import os
import resource
import signal
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location('checked_report', ROOT / 'scripts/checked-report.py')
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)


class Evaluation:
    def __init__(self, directory, weavec, cc):
        self.directory = directory
        self.weavec = str(weavec.resolve())
        self.cc = str(cc.resolve())
        self.cache = directory / 'cache'
        self.sequence = 0

    def write(self, name, content):
        path = self.directory / name
        path.write_text(content)
        return path

    def run(self, files, *, cache=True, flags=(), compiler=(), checked=True, compact=False):
        self.sequence += 1
        stem = self.directory / f'run-{self.sequence}'
        command = [self.weavec, '--whole-program', f'--analysis-stats={stem}.stats',
                   f'--checked-report={stem}.json']
        if checked:
            command.append('--checked')
        if compact:
            command.append('--checked-report-format=compact')
        if cache:
            command.append(f'--analysis-cache={self.cache}')
        command += list(flags) + [str(self.directory / file) for file in files]
        command += ['--', '-std=c17', '-Wno-unused-value', *compiler]
        process = subprocess.run(command, capture_output=True, text=True, timeout=120)
        assert process.returncode in (0, 1), process.stderr
        report = json.loads(Path(str(stem) + '.json').read_text())
        stats = json.loads(Path(str(stem) + '.stats').read_text())['counters']
        return process.returncode, REPORT.expand_report(report), process.stderr, stats

    def equivalent(self, files, **options):
        cached = self.run(files, **options)
        fresh = self.run(files, cache=False, **options)
        assert cached[:3] == fresh[:3], 'cached and fresh outputs disagree'
        return cached

    def warm(self, files, **options):
        cold = self.run(files, **options)
        warm = self.run(files, **options)
        assert cold[:3] == warm[:3], 'cold and warm outputs disagree'
        assert warm[3].get('cache_hits', 0) == len(files), warm[3]
        assert warm[3].get('function_analyses', 0) == 0, warm[3]
        return warm


def evaluate(name, e):
    simple = 'void put(int *p) { *p = 1; }\nint main(void) { int x; put(&x); return x; }\n'
    if name in ('unchanged', 'cache-corruption', 'cache-unavailable', 'explanations'):
        e.write('main.c', simple)
        e.warm(['main.c'])
        if name == 'unchanged':
            # No report/selection flag: exercise the actual ordinary engine,
            # including the independent-source CLI path.
            for scope in ([], ['--whole-program']):
                command = [e.weavec, *scope, f'--analysis-cache={e.cache}',
                           f'--analysis-stats={e.directory}/ordinary.stats',
                           str(e.directory / 'main.c'), '--', '-std=c17']
                cold = subprocess.run(command, capture_output=True, text=True, timeout=120)
                warm = subprocess.run(command, capture_output=True, text=True, timeout=120)
                work = json.loads((e.directory / 'ordinary.stats').read_text())['counters']
                assert work.get('function_analyses', 0) == 0 and work.get('cache_hits', 0) > 0, work
                fresh = subprocess.run([arg for arg in command if not arg.startswith('--analysis-cache=')],
                                       capture_output=True, text=True, timeout=120)
                outputs = lambda run: (run.returncode, run.stdout, run.stderr)
                assert outputs(cold) == outputs(warm) == outputs(fresh)
        if name == 'cache-corruption':
            records = list(e.cache.glob('*.wcache'))
            assert records
            for path in records:
                path.write_bytes(path.read_bytes()[:91])
            result = e.equivalent(['main.c'])
            assert result[3].get('cache_hits', 0) == 0
            assert result[3].get('function_analyses', 0) > 0
            # Concurrent writers publish whole atomic records, never fragments.
            def writer(_):
                command = [e.weavec, '--checked', f'--analysis-cache={e.cache}',
                           str(e.directory / 'main.c'), '--', '-std=c17']
                return subprocess.run(command, capture_output=True, timeout=120).returncode
            for path in e.cache.glob('*.wcache'):
                path.unlink()
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                assert list(pool.map(writer, range(2))) == [0, 0]
            e.warm(['main.c'])
        elif name == 'cache-unavailable':
            e.cache = e.write('not-a-directory', 'x')
            result = e.equivalent(['main.c'])
            assert result[3].get('cache_hits', 0) == 0
            assert result[3].get('cache_write_failures', 0) > 0
            # A successful open followed by a failed write must remain a
            # cache miss, not trigger LLVM's unhandled-stream-error abort.
            def limit_output():
                signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
                resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))
            limited_cache = e.directory / 'limited-cache'
            command = [e.weavec, '--checked', f'--analysis-cache={limited_cache}',
                       str(e.directory / 'main.c'), '--', '-std=c17']
            run = subprocess.run(command, capture_output=True, text=True,
                                 timeout=120, preexec_fn=limit_output)
            assert run.returncode == 0, run.stderr
            assert not list(limited_cache.glob('*')), 'partial cache file survived'
            command.insert(1, f'--checked-report={e.directory}/too-large.json')
            run = subprocess.run(command, capture_output=True, text=True,
                                 timeout=120, preexec_fn=limit_output)
            assert run.returncode == 1 and 'cannot write checked report' in run.stderr, run.stderr
        elif name == 'explanations':
            e.write('main.c', 'int a(void){int x;return x;}\nint b(void){int y;return y;}\n'
                    'int main(void){return a()+b();}\n')
            expanded = e.run(['main.c'])
            compact = e.run(['main.c'], compact=True)
            assert expanded[:3] == compact[:3]
            compact_path = e.directory / f'run-{e.sequence}.json'
            streamed = io.StringIO()
            REPORT.stream_expand(compact_path, streamed)
            assert json.loads(streamed.getvalue()) == expanded[1]
            # Exercise escaped strings, Unicode, split numbers and small chunks.
            sample = {'units': [{'source': 'é.c', 'functions': [{'name': 'a"b', 'n': 12345}]}],
                      'version': 2}
            text = json.dumps(sample, ensure_ascii=False)
            expected_events = list(REPORT.report_events(io.StringIO(text)))
            for chunk_size in (1, 7, 32):
                assert list(REPORT.report_events(io.StringIO(text), chunk_size)) == expected_events
            spec = importlib.util.spec_from_file_location('scalability', ROOT / 'scripts/scalability-evaluation.py')
            scalability = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(scalability)
            summary = scalability.report_summary(compact_path)
            expanded_path = e.directory / 'streamed-expanded.json'
            expanded_path.write_text(streamed.getvalue())
            assert scalability.report_summary(expanded_path)['semantic_sha256'] == summary['semantic_sha256']
            limit_unit = dict(clang_errors=0, exit_code=1, failure='analysis reached an iteration limit',
                              diagnostics=[dict(id='analysis-incomplete', message=
                                                'analysis is incomplete: function dataflow iteration limit reached')])
            limit_report = dict(present=True, iteration_origins=[['f.c', 2, 3, 'iteration limit reached']],
                                limited_functions=[['f.c', 'f', False]], complete_with_iteration_limit=[])
            assert scalability.checked_coverage_valid([limit_unit], limit_report)
            for failure in ('timeout after 600 seconds', 'program analysis did not converge',
                            'checker exited with status -9'):
                assert not scalability.checked_coverage_valid([dict(limit_unit, failure=failure)], limit_report)
            for invalid in (dict(limit_report, present=False), dict(limit_report, iteration_origins=[]),
                            dict(limit_report, limited_functions=[['f.c', 'f', True]]),
                            dict(limit_report, complete_with_iteration_limit=[['f.c', 'f']])):
                assert not scalability.checked_coverage_valid([limit_unit], invalid)
            assert expanded[0] == 1
            origins = {tuple(sorted(o['location'].items())) for u in expanded[1]['units']
                       for f in u['functions'] for o in f['obligations'] if o['outcome'] in ('violation', 'unresolved')}
            assert len(origins) >= 2
            # This expected report comes from the preserved binary, before
            # call-origin grouping. Keep every outcome, requirement and route.
            fixture = ROOT / 'test/evaluation/rfc0020/projection.c'
            run = e.run([str(fixture)], compact=True)
            report = run[1]
            actual = dict(functions=report['units'][0]['functions'],
                          totals=report['totals'], invocation_ok=report['invocation_ok'])
            # RFC 0025 adds separately checked case records. This historical
            # oracle still pins every generic requirement and explanation;
            # the new case/cache population checks the additive fields.
            case_records = any('cases' in function for function in actual['functions'])
            for function in actual['functions']:
                function.pop('case_inputs', None)
                function.pop('cases', None)
            actual = json.loads(json.dumps(actual).replace(str(ROOT), '$ROOT'))
            # For diamond(1), middle(!n) is the route to the else-branch
            # arithmetic obligation. Preserve the original oracle alongside
            # the RFC 0025 snapshot of this more precise route and notes.
            oracle = ROOT / 'test/evaluation/rfc0025/projection.c' if case_records else fixture
            expected = json.loads(oracle.with_suffix('.json').read_text())
            assert actual == expected, 'propagation changed the frozen explanation report'
            expected_diagnostics = oracle.with_suffix('.stderr').read_text()
            assert run[2].replace(str(ROOT), '$ROOT') == expected_diagnostics, \
                'diagnostic deduplication changed the frozen messages or notes'
        return
    if name in ('callee-release', 'callee-initialization', 'leaf-edit', 'unrelated'):
        e.write('main.c', 'void put(int *); int main(void){int x; put(&x); return x;}\n')
        e.write('leaf.c', 'void put(int *p){*p=1;}\n')
        e.write('independent.c', 'int independent(void){return 7;}\n')
        files = ['main.c', 'leaf.c', 'independent.c']
        e.warm(files)
        if name == 'callee-release':
            e.write('leaf.c', 'void free(void *); void put(int *p){*p=1;free(p);}\n')
        elif name == 'callee-initialization':
            e.write('leaf.c', 'void put(int *p){(void)p;}\n')
        else:
            e.write('leaf.c', 'void put(int *p){*p=2;}\n')
        changed = e.equivalent(files)
        assert changed[3].get('function_analyses', 0) > 0
        assert changed[3].get('cache_hits', 0) >= 1, changed[3]
        if name.startswith('callee-'):
            assert changed[0] == 1
        e.warm(files)
        return
    if name == 'missing-definition':
        e.write('main.c', 'void put(int *); int main(void){int x; put(&x); return x;}\n')
        missing = e.warm(['main.c'])
        assert missing[0] == 1
        e.write('leaf.c', 'void put(int *p){*p=1;}\n')
        found = e.equivalent(['main.c', 'leaf.c'])
        assert found[0] == 0
        assert found[3].get('function_analyses', 0) > 0
        e.write('main.c', 'void put(int *); void (*cb)(int *)=put; '
                'int main(void){int x;cb(&x);return x;}\n')
        assert e.warm(['main.c'])[0] == 1
        assert e.equivalent(['main.c', 'leaf.c'])[0] == 0
        return
    if name in ('header', 'conditional-include', 'options'):
        e.write('main.c', '#include "config.h"\nint main(void){int x;\n#if GOOD\nx=1;\n#endif\nreturn x;}\n')
        header = e.write('config.h', '#define GOOD 1\n')
        e.warm(['main.c'])
        if name == 'header':
            before = header.stat()
            header.write_text('#define GOOD 0\n')
            os.utime(header, ns=(before.st_atime_ns, before.st_mtime_ns))
        elif name == 'conditional-include':
            header.write_text('#define GOOD (!__has_include("optional.h"))\n')
            e.warm(['main.c'])
            e.write('optional.h', '/* This file is probed but never included. */\n')
        else:
            header.write_text('#ifndef GOOD\n#define GOOD 1\n#endif\n')
            e.warm(['main.c'])
            changed = e.equivalent(['main.c'], compiler=['-DGOOD=0'])
            assert changed[0] == 1
            assert changed[3].get('cache_hits', 0) == 0
            target = e.equivalent(['main.c'], compiler=['-target', 'wasm32-unknown-unknown'])
            assert target[3].get('cache_hits', 0) == 0
            changed = e.equivalent(['main.c'], flags=['--checked-function=main'], checked=False)
            assert changed[3].get('cache_hits', 0) == 0
            e.write('main.c', 'const char *stamp=__TIME__; int main(void){return 0;}\n')
            e.run(['main.c'])
            volatile = e.run(['main.c'])
            assert volatile[3].get('cache_hits', 0) == 0
            assert volatile[3].get('function_analyses', 0) > 0
            return
        changed = e.equivalent(['main.c'])
        assert changed[0] == 1
        assert changed[3].get('cache_hits', 0) == 0
        return
    if name == 'callback':
        e.write('main.c', 'extern void (*cb)(int *); int main(void){int x=1; cb(&x); return x;}\n')
        e.write('callbacks.c', 'void keep(int *p){(void)p;} void (*cb)(int *)=keep;\n')
        files = ['main.c', 'callbacks.c']
        e.warm(files)
        e.write('callbacks.c', 'void free(void *); void kill(int *p){free(p);} void (*cb)(int *)=kill;\n')
        changed = e.equivalent(files)
        assert changed[0] == 1
        assert changed[3].get('function_analyses', 0) > 0
        return
    if name == 'global-facts':
        e.write('main.c', 'struct B {int *p; int n;}; int get(struct B *b){return b->p[b->n];}\n')
        e.write('facts.c', 'struct B {int *p; int n;}; void *malloc(unsigned long);\n'
                'void make(struct B *b){b->p=malloc(4);b->n=1;}\n')
        files = ['main.c', 'facts.c']
        e.warm(files)
        e.write('facts.c', 'struct B {int *p; int n;}; void change(struct B *b){b->n=0;}\n')
        changed = e.equivalent(files)
        assert changed[3].get('function_analyses', 0) > 0
        return
    if name == 'recursive':
        e.write('main.c', 'int f(int n){if(n>0)return f(n-1); int x;return x;}\n'
                'int main(void){return f(3);}\n')
        assert e.warm(['main.c'])[0] == 1
        e.equivalent(['main.c'])
        return
    if name in ('transport', 'stale-object'):
        e.write('main.c', 'void put(int *); int main(void){int x;put(&x);return x;}\n')
        e.write('leaf.c', 'void put(int *p){*p=1;}\n')
        for file in ('main', 'leaf'):
            command = [e.cc, '-fweavec-checked-function=main', '-c', str(e.directory / f'{file}.c'),
                       '-o', str(e.directory / f'{file}.o')]
            # Selection is a link requirement; the helper compile has no main.
            if file == 'leaf':
                command.remove('-fweavec-checked-function=main')
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=120)
            assert compiled.returncode == 0, compiled.stderr
        def link():
            command = [e.cc, f'-fweavec-analysis-cache={e.cache}', '-fweavec-checked-function=main',
                       f'-fweavec-analysis-stats={e.directory}/link.stats',
                       f'-fweavec-checked-report={e.directory}/link.json',
                       str(e.directory / 'main.o'), str(e.directory / 'leaf.o'),
                       '-o', str(e.directory / 'program')]
            run = subprocess.run(command, capture_output=True, text=True, timeout=120)
            assert run.returncode in (0, 1), run.stderr
            return run
        assert link().returncode == 0
        warm = link()
        assert warm.returncode == 0, warm.stderr
        stats = json.loads((e.directory / 'link.stats').read_text())['counters']
        assert stats.get('function_analyses', 0) == 0, stats
        assert stats.get('cache_hits', 0) == 2, stats
        if name == 'stale-object':
            e.write('leaf.c', 'void put(int *p){(void)p;}\n')
            stale = link()
            assert stale.returncode == 1 and 'source input changed' in stale.stderr, stale.stderr
            # The object contains a releasing helper; the newly available file
            # makes replay see a non-releasing body. No previously included file
            # changed, so only preprocessing binding rejects this stale object.
            e.write('leaf.c', 'void free(void *); void put(int *p){*p=1;\n'
                    '#if !__has_include("optional.h")\nfree(p);\n#endif\n}\n')
            rebuilt = subprocess.run([e.cc, '-c', str(e.directory / 'leaf.c'),
                                      '-o', str(e.directory / 'leaf.o')],
                                     capture_output=True, text=True, timeout=120)
            assert rebuilt.returncode == 0, rebuilt.stderr
            e.write('optional.h', '/* Changes preprocessing without being included. */\n')
            stale = link()
            assert stale.returncode == 1 and 'preprocessing changed' in stale.stderr, stale.stderr
        else:
            assert e.equivalent(['main.c', 'leaf.c'])[0] == 0
        return
    raise AssertionError('unimplemented frozen case: ' + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--weavec', type=Path, required=True)
    parser.add_argument('--cc', type=Path, required=True)
    parser.add_argument('--json', type=Path)
    parser.add_argument('--case', action='append', default=[])
    args = parser.parse_args()
    manifest = json.loads((ROOT / 'test/evaluation/rfc0020/manifest.json').read_text())
    results = []
    with tempfile.TemporaryDirectory(prefix='weavec-incremental-') as temporary:
        for case in manifest['cases']:
            if args.case and case['name'] not in args.case:
                continue
            directory = Path(temporary) / case['name']
            directory.mkdir()
            result = dict(name=case['name'], passed=False)
            try:
                evaluate(case['name'], Evaluation(directory, args.weavec, args.cc))
                result['passed'] = True
            except (AssertionError, OSError, ValueError, subprocess.TimeoutExpired) as error:
                result['reason'] = str(error)
            results.append(result)
            print(('PASS ' if result['passed'] else 'FAIL ') + case['name'], result.get('reason', ''), flush=True)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(dict(version=1, cases=results), indent=2) + '\n')
    return 0 if results and all(case['passed'] for case in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
