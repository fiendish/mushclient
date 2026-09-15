"""Check stored trigger destinations across preferences replacement.

Run with Python 3 and a C++ compiler. Set CXX to select a compiler.
--revision reads production code from Git.
The fixture compiles the destination handling, runtime copy, and replacement
preparation from production source. MFC types and unrelated UI work are substitutes.
Both the normal build and the PANE branch must preserve stored names while
accepting explicit edits to the selected destination.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess


def compiler_command():
    override = os.environ.get('CXX')
    if override:
        try:
            command = shlex.split(override)
        except ValueError as error:
            raise SystemExit(f'Invalid CXX command: {error}') from error
        if not command:
            raise SystemExit('CXX must name a C++ compiler.')
        executable = shutil.which(command[0])
        if executable is None:
            raise SystemExit(f'CXX compiler not found or not executable: {command[0]}')
        return [executable, *command[1:]]
    executable = shutil.which('clang++') or shutil.which('c++')
    if executable is None:
        raise SystemExit('No C++ compiler found. Set CXX or install clang++ or c++ on PATH.')
    return [executable]


ROOT = Path(__file__).resolve().parents[1]


def block(text, start):
    begin = text.index(start)
    brace = text.index('{', begin)
    depth = 0
    for end in range(brace, len(text)):
        depth += (text[end] == '{') - (text[end] == '}')
        if depth == 0:
            return text[begin:end + 1]
    raise ValueError(start)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision')
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    out = args.output_dir or ROOT / '.test-output' / 'trigger-variable-preservation'
    out.mkdir(parents=True, exist_ok=True)
    # Remove the old report before source extraction or either build can fail.
    (out / 'validation.json').unlink(missing_ok=True)

    def source(path):
        if args.revision:
            return subprocess.check_output(
                ['git', '-C', str(ROOT), 'show', args.revision + ':' + path],
                text=True)
        return (ROOT / path).read_text()

    editor = source('dialogs/world_prefs/genpropertypage.cpp')
    prefs = source('dialogs/world_prefs/prefspropertypages.cpp')
    runtime = block(editor, 'static void CopyPropertyRuntimeState')
    trigger_copy = block(
        runtime, 'if (pOldItem->IsKindOf (RUNTIME_CLASS (CTrigger)))')
    trigger_copy = trigger_copy[trigger_copy.index('{'):]

    change = block(editor, 'bool CGenPropertyPage::ChangeOneItem')
    start = change.index('  std::unique_ptr<CObject> pReplacement')
    end = change.index('  CString strDispatchMessage;', start)
    preparation = change[start:end]
    # This fixture omits script lookup. Include the later runtime transfer.
    transfer = '  CopyPropertyRuntimeState (pItem, pUpdatedItem, bChanged);'
    if change.count(transfer) != 1:
        raise ValueError('Expected one property runtime-state transfer')
    if transfer not in preparation:
        preparation += change[change.index(transfer):change.index(transfer) + len(transfer)]

    load = block(prefs, 'void CPrefsP8::LoadDialog')
    unload = block(prefs, 'void CPrefsP8::UnloadDialog')
    changed = block(prefs, 'bool CPrefsP8::CheckIfChanged')
    variable_comparison = re.search(
        r'trigger_item->strVariable\s*==\s*dlg->m_strVariable', changed)
    if variable_comparison is None:
        raise ValueError('Trigger change detection no longer compares the variable control')

    def destination_assignment(function):
        match = re.search(
            r'(?:dlg->m_iSendTo\s*=\s*trigger_item->iSendTo|'
            r'trigger_item->iSendTo\s*=\s*dlg->m_iSendTo)\s*;', function)
        if match is None:
            raise ValueError('Missing trigger destination assignment')
        return match.group()

    parts = {
        '@COPY_RUNTIME@': trigger_copy,
        '@PREPARE_REPLACEMENT@': preparation,
        '@LOAD_DESTINATION@': destination_assignment(load) + '\n' +
            block(load, 'switch (trigger_item->iSendTo)'),
        '@UNLOAD_DESTINATION@': destination_assignment(unload) + '\n' +
            block(unload, 'switch (trigger_item->iSendTo)'),
        '@VARIABLE_COMPARISON@': variable_comparison.group(),
    }
    fixture = Path(__file__).with_suffix('.cpp.in').read_text()
    for marker, value in parts.items():
        if fixture.count(marker) != 1:
            raise ValueError('Expected one fixture marker: ' + marker)
        fixture = fixture.replace(marker, value)

    cpp = out / 'trigger_variable_preservation.cpp'
    cpp.write_text(fixture)
    results = []
    compiler = compiler_command()
    for pane in (False, True):
        label = 'pane' if pane else 'normal'
        exe = out / ('trigger_variable_preservation_' + label)
        command = compiler + ['-std=c++17', '-O1', '-g',
                   '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                   '-fno-omit-frame-pointer']
        if pane:
            command.append('-DPANE')
        command += [str(cpp), '-o', str(exe)]
        subprocess.run(command, check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        log = out / (label + '.log')
        log.write_text(result.stdout + result.stderr)
        print(result.stdout + result.stderr, end='', flush=True)
        expected_cases = 12 if pane else 7
        expected_label = 'PANE' if pane else 'Normal'
        summary = re.fullmatch(
            rf'{expected_label}: (\d+) cases, (\d+) failed checks\n?', result.stdout)
        case_count = int(summary[1]) if summary else None
        failed_checks = int(summary[2]) if summary else None
        valid_summary = case_count == expected_cases and failed_checks == 0
        results.append({'build': label, 'exit_code': result.returncode,
                        'result': 'pass' if result.returncode == 0 and valid_summary else 'fail',
                        'expected_cases': expected_cases, 'case_count': case_count,
                        'failed_checks': failed_checks,
                        'command': command, 'executable': str(exe),
                        'log': str(log)})

    (out / 'validation.json').write_text(json.dumps({
        'revision': args.revision or 'working-tree',
        'results': results,
        'limits': 'Extracted production blocks with MFC substitutes. '
                  'No native UI or live scripting session.'
    }, indent=2) + '\n')
    # Run both branches before reporting failure so neither result is hidden.
    for result in results:
        if result['exit_code'] != 0:
            raise subprocess.CalledProcessError(
                result['exit_code'], [result['executable']])
    for result in results:
        if result['result'] != 'pass':
            raise ValueError(
                f"{result['build']}: expected {result['expected_cases']} cases "
                f"and zero failed checks; see {result['log']}")


if __name__ == '__main__':
    main()
