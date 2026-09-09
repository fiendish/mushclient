"""Run extracted output-find code with callback, window and regexp substitutes.

This checks snapshot ownership and selection mapping, not native MFC dispatch
or PCRE matching. Build failures and unexpected runtime failures propagate.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def block(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 0
    for end in range(brace, len(text)):
        depth += (text[end] == '{') - (text[end] == '}')
        if depth == 0:
            return text[start:end + 1]
    raise ValueError(f'Unclosed production block: {signature}')


def compiler_command():
    if os.environ.get('CXX'):
        command = shlex.split(os.environ['CXX'])
        if not command:
            raise SystemExit('CXX must name a C++ compiler')
        compiler = shutil.which(command[0])
        if compiler is None:
            raise SystemExit(f'CXX compiler not found: {command[0]}')
        return [compiler, *command[1:]]
    compiler = shutil.which('clang++') or shutil.which('c++')
    if compiler is None:
        raise SystemExit('No C++ compiler found')
    return [compiler]


def main():
    send = (ROOT / 'sendvw.cpp').read_text()
    header = (ROOT / 'sendvw.h').read_text()
    finding = (ROOT / 'Finding.cpp').read_text()
    stdafx = (ROOT / 'stdafx.h').read_text()
    doc = (ROOT / 'doc.h').read_text()
    production = send[send.index('class COutputSearchSnapshot :'):send.index('void CSendView::OnDisplayFind()')]
    tokens = {
        'FIND_INFO': block(stdafx, 'class CFindInfo :') + ';',
        'GUARD': block(doc, 'class CWorldDocumentOperationGuard') + ';',
        'FIND_ROUTINE': finding[finding.index('static void WrapUpFind'):],
        'PRODUCTION': production,
    }
    if 'std::shared_ptr<COutputSearchSnapshot> m_pOutputSearchSnapshot;' not in header:
        raise RuntimeError('The production view must own the latest snapshot')
    template = Path(__file__).with_name('output_search_snapshot.cpp.in').read_text()
    for name, value in tokens.items():
        marker = '@' + name + '@'
        if template.count(marker) != 1:
            raise RuntimeError(f'Expected one template marker: {marker}')
        template = template.replace(marker, value)

    out = Path(tempfile.mkdtemp(prefix='mushclient-output-search-'))
    print(f'Artifacts: {out}', flush=True)
    compiler = compiler_command()
    flags = ['-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
             '-fno-omit-frame-pointer']
    cpp = out / 'output_search_snapshot.cpp'
    cpp.write_text(template)
    for mode, extra in [('release', []), ('debug', ['-D_DEBUG'])]:
        exe = out / mode
        subprocess.run([*compiler, *flags, *extra, str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=30)

    # Prove that neither the oracle nor the key callback guards can be removed.
    disabled = subprocess.run([*compiler, *flags, '-DNDEBUG', str(cpp), '-o', str(out / 'ndebug')],
                              capture_output=True, text=True)
    (out / 'ndebug.log').write_text(disabled.stdout + disabled.stderr)
    if disabled.returncode == 0 or 'Output search checks require assertions' not in disabled.stderr:
        raise RuntimeError('The harness did not reject NDEBUG')
    mutants = {
        'text_validation': ('memcmp ((LPCTSTR) strText, pLine->text, pLine->len) == 0', 'true'),
        'nested_publication': ('m_pOutputSearchSnapshot == snapshot', 'true'),
    }
    for name, (before, after) in mutants.items():
        if template.count(before) != 1:
            raise RuntimeError(f'Mutation anchor changed: {name}')
        mutant = out / (name + '.cpp')
        mutant.write_text(template.replace(before, after))
        exe = out / name
        subprocess.run([*compiler, *flags, str(mutant), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        (out / (name + '.log')).write_text(result.stdout + result.stderr)
        if result.returncode == 0 or 'Assertion failed' not in result.stderr and 'Assertion ' not in result.stderr:
            raise RuntimeError(f'Mutation did not fail an assertion: {name}\n{result.stderr}')
        print(f'PASS: {name} mutation rejected')
    print('PASS: NDEBUG rejected; extracted output search checks passed')


if __name__ == '__main__':
    main()
