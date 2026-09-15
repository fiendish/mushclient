"""Check file ownership in the extracted production OnGamePastefile function.

Requires clang++ with ASan/UBSan. Both debug and NDEBUG builds run every selected
case. Checks remain active in both builds and under Python -O. MFC dialogs,
exceptions, and SendToMushHelper are substitutes; files use real C streams.
Native MFC behavior and Windows file sharing are not tested.

Use --revision a72eea4 to reproduce the non-file exception leaks.
Generated source, input files, binaries, and logs stay in --output-dir.
Extraction, compilation, and test errors cause a nonzero exit status.
"""

import argparse
import hashlib
from pathlib import Path
import re
import subprocess


CASES = (
    'normal', 'dialog_cancel', 'send_cancel', 'file_open_exception',
    'file_read_exception', 'file_eof', 'resource_exception', 'other_exception',
)


def extract_function(source):
    signature = 'void CMUSHclientDoc::OnGamePastefile()'
    if source.count(signature) != 1:
        raise ValueError('Expected exactly one OnGamePastefile definition')
    start = source.index(signature)
    depth = 0
    tokens = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|\{|\}'
    for token in re.finditer(tokens, source[start:]):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return source[start:start + token.end()]
    raise ValueError('Unclosed OnGamePastefile definition')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--revision', help='Extract doc.cpp from this Git revision')
    parser.add_argument('--case', choices=CASES, action='append', dest='cases')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)

    raw = (subprocess.check_output(['git', 'show', f'{args.revision}:doc.cpp'], cwd=root)
           if args.revision else (root / 'doc.cpp').read_bytes())
    function = extract_function(raw.decode('utf-8').replace('\r\n', '\n'))
    template_path = Path(__file__).with_name('paste_file_cleanup.cpp.in')
    template_bytes = template_path.read_bytes()
    template = template_bytes.decode('utf-8').replace('\r\n', '\n')
    marker = '@PASTE_FILE@'
    if template.count(marker) != 1:
        raise ValueError('Expected exactly one paste-file template marker')
    cpp = output / 'paste_file_cleanup.cpp'
    cpp.write_text(template.replace(marker, function))
    (output / 'inputs.sha256').write_text(
        f'{hashlib.sha256(raw).hexdigest()}  {args.revision or "working-tree"}:doc.cpp\n'
        f'{hashlib.sha256(template_bytes).hexdigest()}  {template_path.name}\n'
        f'{hashlib.sha256(function.encode()).hexdigest()}  extracted OnGamePastefile\n')
    input_file = output / 'paste-input.txt'
    input_file.write_bytes(b'first line\nsecond line\n')

    failures = []
    cases = args.cases or CASES
    for mode, flags in (('debug', ['-O0', '-D_DEBUG']),
                        ('ndebug', ['-O2', '-DNDEBUG'])):
        executable = output / f'paste_file_cleanup_{mode}'
        command = [
            'clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
            '-fno-omit-frame-pointer', *flags, str(cpp), '-o', str(executable),
        ]
        print('Compile:', command, flush=True)
        with (output / f'{mode}-compile.log').open('w') as log:
            result = subprocess.run(command, cwd=output, stdout=log, stderr=subprocess.STDOUT)
        print((output / f'{mode}-compile.log').read_text(), end='', flush=True)
        result.check_returncode()
        for case in cases:
            result = subprocess.run([str(executable), case, str(input_file)],
                                    cwd=output, capture_output=True, text=True, timeout=30)
            log = result.stdout + result.stderr
            (output / f'{mode}-{case}.log').write_text(log)
            print(f'{mode}/{case}: exit={result.returncode}\n{log}', end='', flush=True)
            if result.returncode:
                failures.append(f'{mode}/{case} (exit {result.returncode})')
    # Report all failed cases after both builds have run. Failures stay fatal.
    if failures:
        raise RuntimeError('Failed paste-file checks: ' + ', '.join(failures))
    print(f'PASS: {len(cases)} cases in both debug and NDEBUG builds', flush=True)


if __name__ == '__main__':
    main()
