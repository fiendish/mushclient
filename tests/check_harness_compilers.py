"""Check harness compiler selection without source extraction."""
import ast
import os
from pathlib import Path
import shlex
import shutil
import tempfile
import unittest
from unittest.mock import patch


class CompilerSelectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compilers = []
        for name in ['check_item_callbacks.py', 'check_item_publication.py',
                     'check_plugin_timer_contracts.py', 'check_list_replacement.py',
                     'check_trigger_variable_preservation.py']:
            path = Path(__file__).with_name(name)
            tree = ast.parse(path.read_text(), filename=str(path))
            functions = [node for node in tree.body
                         if isinstance(node, ast.FunctionDef) and node.name == 'compiler_command']
            if len(functions) != 1:
                raise ValueError(f'{path}: expected one compiler_command function')
            namespace = {'os': os, 'shlex': shlex, 'shutil': shutil}
            exec(compile(ast.Module(body=functions, type_ignores=[]), str(path), 'exec'), namespace)
            cls.compilers.append((name, namespace['compiler_command']))

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mushclient-pr16-compilers-')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.environment = patch.dict(os.environ, {'PATH': str(self.directory)}, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def executable(self, name):
        path = self.directory / name
        path.write_text('#!/bin/sh\nexit 0\n')
        path.chmod(0o755)
        return str(path)

    def check_commands(self, expected):
        for name, resolve in self.compilers:
            with self.subTest(harness=name):
                self.assertEqual(resolve(), expected)

    def check_failure(self, message):
        for name, resolve in self.compilers:
            with self.subTest(harness=name):
                with self.assertRaisesRegex(SystemExit, message):
                    resolve()

    def test_path_prefers_clang(self):
        self.executable('c++')
        self.check_commands([self.executable('clang++')])

    def test_path_falls_back_to_cxx(self):
        self.check_commands([self.executable('c++')])

    def test_named_override_takes_precedence(self):
        self.executable('clang++')
        compiler = self.executable('chosen-cxx')
        os.environ['CXX'] = 'chosen-cxx'
        self.check_commands([compiler])

    def test_quoted_override_preserves_flags_without_a_shell(self):
        compiler = self.executable('chosen cxx')
        os.environ['CXX'] = shlex.join([compiler, '-fsanitize=address,undefined', '-DVALUE=a b', '$(false)'])
        self.check_commands([compiler, '-fsanitize=address,undefined', '-DVALUE=a b', '$(false)'])

    def test_missing_override_does_not_fall_back(self):
        self.executable('clang++')
        os.environ['CXX'] = 'missing-cxx'
        self.check_failure('CXX compiler not found or not executable: missing-cxx')

    def test_nonexecutable_override_fails(self):
        compiler = self.directory / 'not-executable'
        compiler.write_text('not executable')
        os.environ['CXX'] = str(compiler)
        self.check_failure('CXX compiler not found or not executable:')

    def test_missing_compiler_fails(self):
        self.check_failure('No C[+][+] compiler found')

    def test_empty_override_preserves_each_harness_contract(self):
        os.environ['CXX'] = ''
        compiler = self.executable('c++')
        for name, resolve in self.compilers:
            with self.subTest(harness=name):
                if name == 'check_plugin_timer_contracts.py':
                    with self.assertRaisesRegex(SystemExit, 'CXX must name a C[+][+] compiler'):
                        resolve()
                else:
                    self.assertEqual(resolve(), [compiler])

    def test_whitespace_override_fails(self):
        os.environ['CXX'] = '   '
        self.check_failure('CXX must name a C[+][+] compiler')

    def test_malformed_override_fails(self):
        os.environ['CXX'] = "'unterminated"
        self.check_failure('Invalid CXX command:')


if __name__ == '__main__':
    unittest.main()
