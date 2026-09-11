"""Check that trigger-preservation reports describe the current run.

Production extraction runs against current files. Child-process results are
substitutes for compiler failures and runtime results. No native UI runs.
"""
from contextlib import redirect_stdout
import importlib.util
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

RUNNER = Path(__file__).with_name('check_trigger_variable_preservation.py')
spec = importlib.util.spec_from_file_location('trigger_preservation', RUNNER)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class TriggerReportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mushclient-trigger-report-')
        self.addCleanup(self.temp.cleanup)
        self.out = Path(self.temp.name)
        self.report = self.out / 'validation.json'
        self.report.write_text('{"results": [{"exit_code": 0}], "stale": true}')
        self.argv = patch.object(sys, 'argv', [str(RUNNER), '--output-dir', str(self.out)])
        self.argv.start()
        self.addCleanup(self.argv.stop)
        self.env = patch.dict(os.environ, {'CXX': shlex.join([sys.executable])})
        self.env.start()
        self.addCleanup(self.env.stop)

    def invoke(self, results):
        with patch.object(runner.subprocess, 'run', side_effect=results), \
                redirect_stdout(io.StringIO()):
            runner.main()

    def passed(self, pane=False):
        summary = 'PANE: 12 cases, 0 failed checks\n' if pane else 'Normal: 7 cases, 0 failed checks\n'
        return subprocess.CompletedProcess(['fixture'], 0, summary, '')

    def test_first_and_second_compile_failures_remove_stale_report(self):
        for compile_index in (0, 1):
            with self.subTest(compile_index=compile_index):
                self.report.write_text('{"results": [{"exit_code": 0}], "stale": true}')
                error = subprocess.CalledProcessError(23, ['compiler'])
                results = [self.passed(), self.passed()] * compile_index + [error]
                with self.assertRaises(subprocess.CalledProcessError) as caught:
                    self.invoke(results)
                self.assertIs(caught.exception, error)
                self.assertFalse(self.report.exists(), 'A failed compile retained the old report')

    def test_source_read_failure_removes_stale_report(self):
        with patch.object(runner, 'ROOT', self.out / 'missing-source'):
            with self.assertRaises(FileNotFoundError):
                runner.main()
        self.assertFalse(self.report.exists())

    def test_extraction_failure_removes_stale_report(self):
        error = ValueError('forced missing source marker')
        with patch.object(runner, 'block', side_effect=error):
            with self.assertRaises(ValueError) as caught:
                runner.main()
        self.assertIs(caught.exception, error)
        self.assertFalse(self.report.exists())

    def test_bad_compiler_override_removes_stale_report(self):
        with patch.dict(os.environ, {'CXX': "'unterminated"}):
            with self.assertRaisesRegex(SystemExit, 'Invalid CXX command'):
                runner.main()
        self.assertFalse(self.report.exists())

    def test_runtime_failure_records_both_results_and_propagates(self):
        for exit_codes in ((23, 0), (0, 23), (23, 24)):
            with self.subTest(exit_codes=exit_codes):
                results = []
                for code in exit_codes:
                    results += [self.passed(), subprocess.CompletedProcess(
                        ['fixture'], code, 'runtime output\n', 'runtime error\n' if code else '')]
                with self.assertRaises(subprocess.CalledProcessError) as caught:
                    self.invoke(results)
                self.assertEqual(caught.exception.returncode, next(c for c in exit_codes if c))
                report = json.loads(self.report.read_text())
                self.assertNotIn('stale', report)
                self.assertEqual([r['exit_code'] for r in report['results']], list(exit_codes))
                self.assertEqual([r['build'] for r in report['results']], ['normal', 'pane'])
                self.assertEqual(len(list(self.out.glob('*.log'))), 2)

    def test_missing_cases_and_wrong_build_are_rejected(self):
        for pane, summary in (
                (False, 'Normal: 6 cases, 0 failed checks\n'),
                (True, 'PANE: 11 cases, 0 failed checks\n'),
                (True, 'Normal: 7 cases, 0 failed checks\n'),
                (False, ''),
                (False, 'Normal: 7 cases, 1 failed checks\n')):
            with self.subTest(pane=pane, summary=summary):
                results = [self.passed(), self.passed(), self.passed(), self.passed(pane=True)]
                results[3 if pane else 1] = subprocess.CompletedProcess(['fixture'], 0, summary, '')
                with self.assertRaisesRegex(ValueError, 'expected .* cases and zero failed checks'):
                    self.invoke(results)
                report = json.loads(self.report.read_text())
                self.assertEqual(len(report['results']), 2)
                self.assertEqual(report['results'][int(pane)]['result'], 'fail')
                self.assertEqual(report['results'][1 - int(pane)]['result'], 'pass')

    def test_success_replaces_stale_report(self):
        self.invoke([self.passed(), self.passed(), self.passed(), self.passed(pane=True)])
        report = json.loads(self.report.read_text())
        self.assertNotIn('stale', report)
        self.assertEqual([r['exit_code'] for r in report['results']], [0, 0])
        self.assertEqual([r['result'] for r in report['results']], ['pass', 'pass'])
        self.assertEqual([r['case_count'] for r in report['results']], [7, 12])


if __name__ == '__main__':
    unittest.main()
