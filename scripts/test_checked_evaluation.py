#!/usr/bin/env python3
"""RFC 0019: the proof evaluation harness must not mistake failures for bugs."""
import importlib.util
import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
from unittest import mock
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('checked_evaluation', Path(__file__).with_name('checked-evaluation.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class AssessmentTest(unittest.TestCase):
    def setUp(self):
        self.case = dict(functions=['main'], expect='rejected', reason='initializ',
                         entry_requirements=0, forbidden_trust=['unsafe boundary'])
        self.function = dict(name='main', selected=True, complete=False,
                             requirements=[], obligations=[dict(outcome='unresolved',
                                                               reason='read interval must be initialized')])
        self.document = dict(invocation_ok=False, units=[dict(functions=[self.function])])

    def test_intended_rejection(self):
        self.assertTrue(module.assess(self.case, 1, self.document)[0])

    def test_crash_is_not_rejection(self):
        for status in [-11, 2, 127]:
            self.assertFalse(module.assess(self.case, status, self.document)[0])

    def test_missing_report_is_not_rejection(self):
        self.assertFalse(module.assess(self.case, 1, {})[0])

    def test_wrong_function_is_not_rejection(self):
        self.function['name'] = 'unrelated'
        self.assertFalse(module.assess(self.case, 1, self.document)[0])

    def test_unrelated_obligation_is_not_rejection(self):
        self.function['obligations'][0]['reason'] = 'callee has no body'
        self.assertFalse(module.assess(self.case, 1, self.document)[0])

    def test_entry_assumption_cannot_replace_proof(self):
        self.function['requirements'] = [dict(kind='initialized')]
        self.assertFalse(module.assess(self.case, 1, self.document)[0])

    def test_trust_cannot_replace_proof(self):
        self.function['obligations'].append(dict(outcome='trusted', reason='unsafe boundary: read'))
        self.assertFalse(module.assess(self.case, 1, self.document)[0])

    def test_incomplete_invocation_cannot_claim_acceptance(self):
        self.case['expect'] = 'accepted'
        self.case['reason'] = None
        self.function['complete'] = True
        self.assertFalse(module.assess(self.case, 0, self.document)[0])
        self.document['invocation_ok'] = True
        self.function['obligations'] = []
        self.assertTrue(module.assess(self.case, 0, self.document)[0])
        self.function['limited'] = True
        self.assertFalse(module.assess(self.case, 0, self.document)[0])


class BufferRunnerTest(unittest.TestCase):
    def test_missing_new_report_cannot_reuse_a_previous_success(self):
        spec = importlib.util.spec_from_file_location(
            'checked_buffers', Path(__file__).with_name('checked-buffers.py'))
        buffers = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(buffers)
        original_directory = Path.cwd()
        for code in (0, 1):
            with self.subTest(returncode=code), tempfile.TemporaryDirectory() as directory:
                output = Path(directory)
                stale = dict(cases=[dict(name='old', passed=True)])
                (output / 'cases.json').write_text(json.dumps(stale))
                (output / 'results.json').write_text(json.dumps(stale))
                args = ['checked-buffers.py', '--population', 'source',
                        '--weavec', sys.executable, '--output', str(output)]
                run = subprocess.CompletedProcess([], code, stdout='', stderr='')
                try:
                    with mock.patch.object(sys, 'argv', args), \
                         mock.patch.object(buffers.SUPPORT, 'invoke', return_value=(run, 0)), \
                         contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(buffers.main(), 1)
                finally:
                    os.chdir(original_directory)
                result = json.loads((output / 'results.json').read_text())
                self.assertEqual(len(result['cases']), 1)
                self.assertFalse(result['cases'][0]['passed'])
                self.assertNotEqual(result['cases'][0]['name'], 'old')


if __name__ == '__main__':
    unittest.main()
