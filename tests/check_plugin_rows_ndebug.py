"""Check that a stale-row regression fails when CXX defines NDEBUG.

Compile the current plugin-row harness against unchanged and altered source.
Only a temporary copy of the plugin dialog source is altered. This check uses
the harness substitutes and does not run native MFC dialogs.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def run(output):
    output.mkdir(parents=True, exist_ok=True)
    report = output / 'result.json'
    report.unlink(missing_ok=True)
    command = shlex.split(os.environ['CXX']) if os.environ.get('CXX') else [
        shutil.which('clang++') or shutil.which('c++')]
    if not command or not command[0]:
        raise ValueError('No C++ compiler found. Set CXX or install clang++ or c++.')
    env = os.environ.copy()
    env['CXX'] = shlex.join([*command, '-DNDEBUG'])
    checks = []

    def invoke(source, label):
        invocation = [sys.executable, str(ROOT / 'tests/check_plugin_rows.py'),
                      '--source', str(source), '--output', str(output / label)]
        result = subprocess.run(invocation, capture_output=True, text=True, env=env)
        log = output / (label + '.log')
        log.write_text(result.stdout + result.stderr)
        print(result.stdout + result.stderr, end='', flush=True)
        checks.append({'name': label, 'command': invocation,
                       'exit_code': result.returncode, 'log': str(log)})
        return result

    invoke(ROOT, 'unchanged').check_returncode()
    with tempfile.TemporaryDirectory(prefix='mushclient-row-regression-') as directory:
        source = Path(directory)
        for name in ('PluginsDlg.cpp', 'PluginsDlg.h'):
            target = source / 'dialogs/plugins' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / 'dialogs/plugins' / name, target)
        path = source / 'dialogs/plugins/PluginsDlg.cpp'
        original = path.read_bytes()
        newline = b'\r\n' if b'\r\n' in original else b'\n'
        condition = (b'if (!pPlugin ||' + newline +
                     b'      pPlugin->m_iPluginInstanceNumber != m_PluginInstanceNumbers [iIndex])')
        if original.count(condition) != 1:
            raise ValueError('Expected one plugin row instance check')
        path.write_bytes(original.replace(condition, b'if (!pPlugin)'))
        result = invoke(source, 'stale-instance-regression')
        diagnostic = 'check failed: (dialog.edited==vector<CString>{"other.xml"})'
        if result.returncode == 0 or diagnostic not in result.stdout + result.stderr:
            raise AssertionError('The stale-row regression did not fail at the expected check under NDEBUG.')
    report.write_text(json.dumps({'status': 'pass', 'CXX': env['CXX'], 'checks': checks,
                                 'limits': 'Extracted source with MFC substitutes. No native UI.'},
                                indent=2) + '\n')
    print('NDEBUG: unchanged source passed; stale-row regression failed at the expected check.', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output:
        run(args.output.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix='mushclient-rows-ndebug-') as directory:
            run(Path(directory))
