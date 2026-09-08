#!/usr/bin/env python3
"""RFC 0018 executable checks for reports, selection, compiler and artifact binding."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--weavec', required=True)
    parser.add_argument('--cc', required=True)
    args = parser.parse_args()
    tool, cc = map(lambda value: str(Path(value).resolve()), (args.weavec, args.cc))
    failures = []
    with tempfile.TemporaryDirectory(prefix='weavec-checked-build-') as directory:
        root = Path(directory)

        def source(name, text):
            (root / name).write_text(text)

        def run(name, command, expected=0, contains=None):
            result = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=120)
            passed = (result.returncode == 0) == (expected == 0) and result.returncode >= 0
            if contains:
                passed &= contains in result.stderr
            print(('PASS' if passed else 'FAIL') + ' ' + name, flush=True)
            if not passed:
                failures.append(name)
                print(result.stderr, flush=True)
            return result

        source('good.c', 'int f(void){int a[2]={1,2};return a[1];}\n')
        source('bad.c', 'int f(int i){int a[2]={1,2};return a[i];}\n')
        source('header.h', 'static int header_bad(int i){int a[1]={0};return a[i];}\n')
        source('header.c', '#include "header.h"\nint f(void){return 0;}\n')
        run('named-header-diagnostics', [tool, '--checked-function=header_bad', 'header.c', '--'], 1, 'cannot establish checked safety: access interval')
        source('header.h', '__attribute__((annotate("weavec.checked"))) static int header_bad(int i){int a[1]={0};return a[i];}\n')
        run('annotated-header-diagnostics', [tool, 'header.c', '--'], 1, 'cannot establish checked safety: access interval')
        report = str(root / 'report.json')
        run('named-selection', [tool, '--checked-function=f', '--checked-report=' + report, 'good.c', '--'])
        first = Path(report).read_bytes()
        run('deterministic-report', [tool, '--checked-function=f', '--checked-report=' + report, 'good.c', '--'])
        if Path(report).read_bytes() != first: failures.append('report-byte-determinism')
        run('empty-selection', [tool, '--checked-function=missing', 'good.c', '--'], 1, 'was not found')
        run('write-failure', [tool, '--checked-report=/nonexistent/weavec-report.json', 'good.c', '--'], 1, 'cannot write checked report')
        run('warning-demotion', [cc, '-fweavec-checked', '-Wno-error=weavec', '-c', 'bad.c', '-o', 'bad.o'], 1, 'checked safety requirements were not established')
        if (root / 'bad.o').exists(): failures.append('failed-compile-created-object')
        run('compile-report', [cc, '-fweavec-checked', '-fweavec-checked-report=' + report, '-c', 'good.c', '-o', 'good.o'])
        (root / 'blocked.o.weavec').mkdir()
        run('checked-sidecar-write-failure', [cc, '-fweavec-checked', '-c', 'good.c', '-o', 'blocked.o'], 1, 'weavec-cc: error:')
        source('write.c', 'void write_byte(char*p){*p=1;}\n')
        source('readonly.c', 'void write_byte(char*);int main(void){char*p="x";write_byte(p);return 0;}\n')
        run('compile-writable-contract', [cc, '-fweavec-checked', '-c', 'write.c', '-o', 'write.o'])
        run('compile-readonly-caller', [cc, '-fweavec-checked', '-c', 'readonly.c', '-o', 'readonly.o'])
        run('readonly-link-rejection', [cc, 'readonly.o', 'write.o', '-o', 'readonly'], 1, 'read-only storage')
        source('helper.c', 'void fill(char*p,unsigned n){for(unsigned i=0;i<n;++i){if(p[i]==42)break;p[i]=1;}}\n')
        source('size.h', '#define COUNT 4\n')
        source('main.c', '#include <stdlib.h>\n#include "size.h"\nvoid fill(char*,unsigned);int main(void){char*p=calloc(4,1);if(!p)return 0;fill(p,COUNT);free(p);return 0;}\n')
        run('compile-helper', [cc, '-fweavec-checked', '-c', 'helper.c', '-o', 'helper.o'])
        run('compile-deferred', [cc, '-fweavec-checked-function=main', '-c', 'main.c', '-o', 'main.o'])
        run('link-proves-contract', [cc, '-fweavec-checked-report=' + report, 'main.o', 'helper.o', '-o', 'good'])
        document = json.loads(Path(report).read_text())
        selected = [fn for unit in document['units'] for fn in unit['functions'] if fn['selected']]
        if not selected or not all(fn['complete'] for fn in selected): failures.append('settled-selected-report')
        helper_metadata = (root / 'helper.o.weavec').read_bytes()
        (root / 'helper.o.weavec').unlink()
        run('missing-helper-metadata', [cc, 'main.o', 'helper.o', '-o', 'missing'], 1, 'checked safety')
        (root / 'helper.o.weavec').write_bytes(helper_metadata)
        source('size.h', '#define COUNT 8\n')
        run('stale-header', [cc, 'main.o', 'helper.o', '-o', 'stale'], 1, 'source input changed')
        source('size.h', '#define COUNT 4\n')
        original = (root / 'helper.o').read_bytes()
        (root / 'helper.o').write_bytes(original + b'changed')
        run('stale-object', [cc, 'main.o', 'helper.o', '-o', 'stale'], 1, 'object contents changed')
        (root / 'helper.o').write_bytes(original)
        source('size.h', '#define COUNT 8\n')
        run('compile-bad-deferred', [cc, '-fweavec-checked-function=main', '-c', 'main.c', '-o', 'main.o'])
        run('link-rejects-contract', [cc, 'main.o', 'helper.o', '-o', 'bad'], 1, 'checked safety')
        run('whole-program-rejects', [tool, '--whole-program', '--checked-function=main', 'main.c', 'helper.c', '--'], 1, 'checked safety')
        source('size.h', '#define COUNT 4\n')
        run('whole-program-proves', [tool, '--whole-program', '--checked-function=main', 'main.c', 'helper.c', '--'])
        source('annotation.c', '#include <weavec.h>\nWEAVEC_CHECKED int main(void){int a[1]={0};return a[0];}\n')
        run('annotation-only', [cc, 'annotation.c', '-o', 'annotation'])
        run('disabled-cc1', [cc, '-cc1', '-fno-weavec', '-fweavec-checked', '-emit-obj', 'good.c', '-o', 'disabled.o'], 1, 'require C analysis')
        run('unsupported-language', [cc, '-fweavec-checked', '-x', 'c++', '-c', 'good.c', '-o', 'disabled.o'], 1, 'require C analysis')
        run('disabled-analysis', [cc, '-fno-weavec', '-fweavec-checked', '-c', 'good.c', '-o', 'disabled.o'], 1, 'require WeaveC analysis')
        run('disabled-link', [cc, '-fno-weavec-link', '-fweavec-checked', 'annotation.c', '-o', 'disabled'], 1, 'requires link verification')
        source('annotated-main.c', '#include <weavec.h>\n#include <stdlib.h>\nvoid fill(char*,unsigned);WEAVEC_CHECKED int main(void){char*p=calloc(4,1);if(!p)return 0;fill(p,4);free(p);return 0;}\n')
        run('annotation-whole-program', [tool, '--whole-program', 'annotated-main.c', 'helper.c', '--'])
        source('wrong-prototype.c', 'int fill(int);int main(void){return fill(1);}\n')
        run('prototype-mismatch', [tool, '--whole-program', '--checked-function=main', 'wrong-prototype.c', 'helper.c', '--'], 1, 'C signature differs')
        run('missing-driver-selection', [cc, '-fweavec-checked-function=missing', '-c', 'good.c', '-o', 'missing.o'], 1, 'was not found')
    print(f'{len(failures)} failed checks', flush=True)
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
