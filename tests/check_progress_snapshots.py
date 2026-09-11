#!/usr/bin/env python3
"""Check output snapshots with the exact RecallText and CalculateMemoryUsage bodies.

Requires Python 3 and a C++17 compiler with ASan/UBSan. CXX overrides the compiler.
Run --baseline REV to check the same cases against an earlier Git revision.
Use --revision REV to test that revision as the candidate, without expected failures.

Limits: portable, synchronous adapters replace MFC strings, heap-backed lists,
dialogs, document retention, and time formatting. Regex cases use ASCII std::regex,
not PCRE. Historical source parenthesizes MSVC-only sizeof type expressions.
Memory totals use adapter sizes, not the Windows ABI. These checks do not
run Windows/Wine UI, message dispatch, native close handling, or locale.
The storage cases preserve raw UTF-8 bytes but do not test PCRE UTF-8 semantics.
Allocation probes count text allocations and bytes, not native heap overhead.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FUNCTIONS = {
    'RECALL': ('doc.cpp', 'CString CMUSHclientDoc::RecallText ('),
    'MEMORY': ('dialogs/world_prefs/prefspropertypages.cpp',
               'void CPrefsP15::CalculateMemoryUsage ()'),
}
STORAGE_CASES = ['recall_storage_content', 'recall_storage_cancel',
                 'recall_storage_short', 'recall_storage_failure']
CONTROLS = ['recall_control', 'recall_empty', 'recall_cancel',
            'memory_control', 'memory_empty', 'memory_cancel']
REGRESSIONS = [
    f'{operation}_{action}_{event}'
    for operation in ('recall', 'memory')
    for action in ('clear', 'prune', 'replace', 'rewrite')
    for event in ('create', 'setpos', 'poll')
] + ['recall_clear_cancel', 'memory_clear_cancel', 'memory_clear_publish']
REGRESSIONS += [f'memory_{action}_{event}'
                for action in ('destroy', 'reuse')
                for event in ('create', 'setpos', 'poll')]


def block(source, signature):
    if source.count(signature) != 1:
        raise ValueError(f'Expected one production function: {signature}')
    start = source.index(signature)
    depth = 0
    opened = False
    # Ignore braces in comments, ordinary strings, and character literals.
    tokens = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
    for match in re.finditer(tokens, source[start:]):
        token = match.group()
        if token == '{':
            opened = True
            depth += 1
        elif token == '}':
            depth -= 1
            if opened and depth == 0:
                return source[start:start + match.end()]
    raise ValueError(f'Unclosed production function: {signature}')


def compiler_command():
    override = os.environ.get('CXX', '')
    if override:
        command = shlex.split(override)
        if not command:
            raise ValueError('CXX must name a C++ compiler')
        executable = shutil.which(command[0])
        if executable is None:
            raise FileNotFoundError(f'CXX compiler not found: {command[0]}')
        return [executable, *command[1:]]
    executable = shutil.which('clang++') or shutil.which('c++')
    if executable is None:
        raise FileNotFoundError('No C++ compiler found. Set CXX or install clang++ or c++.')
    return [executable]


def extract(source, revision, template):
    parts = {}
    for name, (path, signature) in FUNCTIONS.items():
        if revision:
            text = subprocess.check_output(
                ['git', '-C', str(source), 'show', f'{revision}:{path}'], text=True)
        else:
            text = (source / path).read_text()
        parts[name] = block(text, signature)
        # Older MSVC source permits sizeof Type without parentheses.
        # Parenthesize only those type expressions for portable compilation.
        if revision:
            for typename in ('CLine', 'CStyle'):
                parts[name] = re.sub(r'\bsizeof ' + typename + r'\b',
                                     'sizeof (' + typename + ')', parts[name])
        marker = '@' + name + '@'
        if template.count(marker) != 1:
            raise ValueError(f'Expected one template marker: {marker}')
        # The extracted bodies are inserted without rewriting production code.
        template = template.replace(marker, parts[name])
    hashes = {name: hashlib.sha256(body.encode()).hexdigest()
              for name, body in parts.items()}
    return template, hashes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT)
    parser.add_argument('--revision')
    parser.add_argument('--baseline', help='Require baseline controls to pass and regressions to fail')
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    source = args.source.resolve()
    out = (args.output_dir or Path(tempfile.mkdtemp(prefix='mushclient-progress-snapshots-'))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    report_path = out / 'progress_snapshots.json'
    report_path.unlink(missing_ok=True)
    print(f'Artifacts: {out}', flush=True)
    compiler = compiler_command()
    template = Path(__file__).with_name('progress_snapshots.cpp.in').read_text()
    program, hashes = extract(source, args.revision, template)
    report = {'source': str(source), 'revision': args.revision or 'working tree',
              'function_sha256': hashes, 'limits': __doc__, 'runs': []}
    environment = dict(os.environ, ASAN_OPTIONS='halt_on_error=1',
                       UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')

    def compile_fixture(name, body, flags):
        cpp = out / f'{name}.cpp'
        exe = out / name
        cpp.write_text(body)
        command = [*compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra',
                   '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                   '-fno-omit-frame-pointer', *flags, str(cpp), '-o', str(exe)]
        subprocess.run(command, check=True, timeout=120)
        report.setdefault('compile_commands', []).append(command)
        return exe

    def run_case(exe, case, expected_failure=False):
        result = subprocess.run([str(exe), case], capture_output=True, text=True,
                                env=environment, timeout=30)
        log = out / f'{exe.name}-{case}.log'
        output = result.stdout + result.stderr
        log.write_text(output)
        reason = None
        if expected_failure:
            if 'ERROR: AddressSanitizer: heap-use-after-free' in result.stderr:
                reason = 'ASan heap-use-after-free'
            elif 'CHECK failed: snapshot result' in result.stderr:
                reason = 'wrong snapshot result'
            elif 'CHECK failed: cancellation result' in result.stderr:
                reason = 'wrong cancellation result'
            elif 'CHECK failed: page window result' in result.stderr:
                reason = 'access to a destroyed or replaced page window'
            if result.returncode == 0 or reason is None:
                raise RuntimeError(f'Baseline did not fail as expected: {case}\n{output}')
            print(f'EXPECTED BASELINE FAILURE: {case}: {reason} ({log.name})', flush=True)
        else:
            print(output, end='', flush=True)
            result.check_returncode()
            if f'PASS: {case}\n' not in result.stdout:
                raise RuntimeError(f'Missing case completion: {case}')
        report['runs'].append({'binary': exe.name, 'case': case,
                               'returncode': result.returncode,
                               'expected_failure': expected_failure,
                               'reason': reason, 'log': str(log)})

    # NDEBUG also checks that all fixture checks remain active in release builds.
    for mode, flags in [('debug', ['-D_DEBUG']), ('release', ['-DNDEBUG'])]:
        exe = compile_fixture(mode, program, flags)
        for case in CONTROLS + REGRESSIONS + STORAGE_CASES:
            run_case(exe, case)

    if args.baseline:
        baseline_revision = subprocess.check_output(
            ['git', '-C', str(source), 'rev-parse', '--verify', args.baseline + '^{commit}'],
            text=True).strip()
        baseline, baseline_hashes = extract(source, baseline_revision, template)
        report['baseline'] = {'revision': baseline_revision,
                              'function_sha256': baseline_hashes}
        exe = compile_fixture('baseline', baseline, ['-DNDEBUG'])
        for case in CONTROLS:
            run_case(exe, case)
        for case in REGRESSIONS:
            run_case(exe, case, expected_failure=True)

    # Do not report a pass for stale functions if another worker changed them.
    _, final_hashes = extract(source, args.revision, template)
    if final_hashes != hashes:
        raise RuntimeError('Production functions changed during the run. Run the check again.')
    report['result'] = 'pass'
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {len(CONTROLS) + len(REGRESSIONS) + len(STORAGE_CASES)} cases in each candidate mode', flush=True)
    print(f'Report: {report_path}', flush=True)


if __name__ == '__main__':
    main()
