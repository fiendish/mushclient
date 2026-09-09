"""Check failed reruns and comparator extraction with temporary source copies.

Run with Python 3 and clang++. Successful runs compile and execute the harnesses.
Only temporary copies are changed; all generated artifacts are temporary.
"""
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


class HarnessReportTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='mushclient-harness-reports-')
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.root = self.directory / 'source'
        self.output = self.directory / 'output'
        root = Path(__file__).resolve().parents[1]
        for name in [
            'tests/check_item_callbacks.py', 'tests/check_list_replacement.py',
            'tests/list_replacement.cpp.in', 'xml/xml_load_world.cpp',
            'doc.cpp', 'doc.h', 'dialogs/world_prefs/genpropertypage.cpp',
        ]:
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(root / name, target)
        self.env = os.environ.copy()
        self.env.pop('CXX', None)
        self.env['PYTHONDONTWRITEBYTECODE'] = '1'

    def run_harness(self, name, env=None, output=None, cwd=None):
        return subprocess.run(
            [sys.executable, str(self.root / 'tests' / name),
             '--output', str(self.output if output is None else output)],
            cwd=self.root if cwd is None else cwd,
            env=self.env if env is None else env, capture_output=True, text=True,
        )

    def callbacks_pass(self):
        result = self.run_harness('check_item_callbacks.py')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.report = self.output / 'validation.json'
        report = json.loads(self.report.read_text())
        self.assertEqual([entry['name'] for entry in report],
                         ['warning_alias', 'warning_trigger', 'warning_timer', 'editor_keys'])
        self.assertTrue(all(entry['result'] == 'pass' for entry in report))
        self.assertFalse((self.root / '.test-output').exists())

    def callbacks_fail(self, message, env=None):
        result = self.run_harness('check_item_callbacks.py', env)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(message, result.stderr)
        self.assertFalse(self.report.exists(), 'Failed rerun retained validation.json')
        return result

    def test_current_directory_output(self):
        for name in ('check_item_callbacks.py', 'check_list_replacement.py'):
            with self.subTest(harness=name):
                directory = self.directory / name
                directory.mkdir()
                result = self.run_harness(name, output='.', cwd=directory)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue(any(directory.iterdir()))

    def test_zero_comparison_measurement_fails(self):
        path = self.root / 'tests/list_replacement.cpp.in'
        source = path.read_bytes()
        marker = b'++comparisons;'
        self.assertGreater(source.count(marker), 0)
        path.write_bytes(source.replace(marker, b''))
        result = self.run_harness('check_list_replacement.py')
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn('Large-list comparison bounds and ownership checks passed', result.stdout)

    def test_compile_failure_removes_previous_pass_report(self):
        self.callbacks_pass()
        env = self.env.copy()
        env['CXX'] = shlex.join([sys.executable, '-c', 'raise SystemExit(23)'])
        self.callbacks_fail('returned non-zero exit status 23', env)

    def test_runtime_failure_removes_previous_pass_report(self):
        self.callbacks_pass()
        path = self.root / 'xml/xml_load_world.cpp'
        source = path.read_bytes()
        marker = b'  a->nUpdateNumber    = App.GetUniqueNumber ();'
        self.assertEqual(source.count(marker), 1)
        path.write_bytes(source.replace(
            marker, marker + b' throw std::logic_error("forced callback runtime failure");', 1))
        self.callbacks_fail('CalledProcessError')
        self.assertIn('forced callback runtime failure',
                      (self.output / 'warning_alias.log').read_text())

    def test_source_read_failure_removes_previous_pass_report(self):
        self.callbacks_pass()
        (self.root / 'doc.h').unlink()
        self.callbacks_fail('FileNotFoundError')

    def test_extraction_failure_removes_previous_pass_report(self):
        self.callbacks_pass()
        path = self.root / 'xml/xml_load_world.cpp'
        source = path.read_bytes()
        marker = b'bool CMUSHclientDoc::Load_One_Alias_XML ('
        self.assertEqual(source.count(marker), 1)
        path.write_bytes(source.replace(marker, marker.replace(b'Alias', b'AliasDrift'), 1))
        self.callbacks_fail('xml/xml_load_world.cpp: missing source marker')

    def test_current_list_records_nonzero_comparison_counts(self):
        result = self.run_harness('check_list_replacement.py')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        measurements = re.findall(
            r'^(rows=\d+|large rollback) comparisons=(\d+)$', result.stdout, re.MULTILINE)
        self.assertEqual([label for label, _ in measurements],
                         ['rows=100', 'rows=1000', 'rows=10000', 'large rollback'])
        for label, value in measurements:
            self.assertGreater(int(value), 0, label)
        self.assertIn('Large-list comparison bounds and ownership checks passed', result.stdout)
        self.assertFalse((self.root / '.test-output').exists())

    def test_list_comparator_spelling_drift_fails_before_measurement(self):
        path = self.root / 'dialogs/world_prefs/genpropertypage.cpp'
        source = path.read_bytes()
        start = source.index(b'void CGenPropertyPage::LoadList')
        start = source.index(b'  CString strObjectName;', start)
        end = source.index(b'  // sort filtered items', start)
        excerpt = source[start:end]
        comparator = b'std::less<CString *> ()'
        self.assertEqual(excerpt.count(comparator), 4)
        for changed in (1, 4):
            with self.subTest(changed_comparators=changed):
                mutated = excerpt.replace(comparator, b'std::less<CString *>()', changed)
                path.write_bytes(source[:start] + mutated + source[end:])
                result = self.run_harness('check_list_replacement.py')
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(f'instrumentation expected 4 comparator matches; found {4 - changed}',
                              result.stderr)
                self.assertNotIn('comparisons=', result.stdout)
                self.assertFalse((self.output / 'list_replacement.cpp').exists())
                self.assertFalse((self.output / 'result.log').exists())


if __name__ == '__main__':
    unittest.main()
