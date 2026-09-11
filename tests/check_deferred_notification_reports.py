"""Check deferred-notification reports with injected child-process results.

Source extraction uses the current files. Compilation and fixture execution
use controlled results to check malformed output and nonzero exit codes.
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


RUNNER = Path(__file__).with_name('check_deferred_notifications.py')
spec = importlib.util.spec_from_file_location('deferred_notifications', RUNNER)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class DeferredNotificationReportTests(unittest.TestCase):
    def check_report(self, stdout, exit_code, malformed):
        stderr = 'fixture diagnostic\n' if exit_code or malformed else ''
        with tempfile.TemporaryDirectory(prefix='mushclient-deferred-report-') as directory:
            out = Path(directory)
            report_path = out / 'validation.json'
            report_path.write_text('{"result": "stale pass"}\n')
            captured = io.StringIO()
            with patch.object(sys, 'argv', [str(RUNNER), '--output-dir', str(out)]), \
                    patch.dict(os.environ, {'CXX': shlex.join([sys.executable])}), \
                    patch.object(runner.subprocess, 'run', side_effect=[
                        subprocess.CompletedProcess(['compiler'], 0, '', ''),
                        subprocess.CompletedProcess(['fixture'], exit_code, stdout, stderr),
                    ]), redirect_stdout(captured):
                if exit_code:
                    with self.assertRaises(subprocess.CalledProcessError) as failure:
                        runner.main()
                    self.assertEqual(failure.exception.returncode, exit_code)
                elif malformed:
                    with self.assertRaises(json.JSONDecodeError):
                        runner.main()
                else:
                    runner.main()
            self.assertEqual((out / 'result.log').read_text(), stdout + stderr)
            self.assertEqual(captured.getvalue(), stdout + stderr)
            self.assertTrue(report_path.exists(), 'Failed run did not write validation.json')
            report = json.loads(report_path.read_text())
            self.assertEqual(report['result'], 'fail' if exit_code or malformed else 'pass')
            self.assertEqual(report['exit_code'], exit_code)
            self.assertEqual(report['stdout'], stdout)
            self.assertEqual(report['stderr'], stderr)
            self.assertEqual(report['cases'], [{'case': 'before'}, {'case': 'after'}])
            self.assertEqual(bool(report['parse_errors']), malformed)
            if malformed:
                errors = report['parse_errors']
                self.assertEqual([error['line'] for error in errors], [2, 4])
                self.assertEqual([error['output'] for error in errors],
                                 ['fixture plain diagnostic', '{"case":'])
                self.assertTrue(all(error['error'] for error in errors))

    def test_valid_output(self):
        for exit_code in (0, 23):
            with self.subTest(exit_code=exit_code):
                self.check_report('{"case":"before"}\n{"case":"after"}\n',
                                  exit_code, False)

    def test_malformed_output(self):
        for exit_code in (0, 23):
            with self.subTest(exit_code=exit_code):
                self.check_report(
                    '{"case":"before"}\nfixture plain diagnostic\n'
                    '{"case":"after"}\n{"case":', exit_code, True)


if __name__ == '__main__':
    unittest.main()
