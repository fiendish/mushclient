"""Check deferred notepad closure with extracted production C++ paths.

Run with Python 3 and clang++ or CXX. Requires ASan/UBSan support.
--revision reads a Git revision. --check-mutations checks each visibility filter.
The fixture supplies MFC controls and registries. It does not run native UI.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def block(text, signature):
    start = text.index(signature)
    depth = 0
    opened = False
    tokens = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\{|\}'
    for token in re.finditer(tokens, text[start:]):
        if token.group() == '{':
            depth += 1
            opened = True
        elif token.group() == '}':
            depth -= 1
            if opened and depth == 0:
                return text[start:start + token.end()]
    raise ValueError(f'Unclosed production block: {signature}')


def compiler_command():
    override = os.environ.get('CXX', '')
    if override:
        command = shlex.split(override)
        if not command:
            raise ValueError('CXX must name a C++ compiler')
        compiler = shutil.which(command[0])
        if not compiler:
            raise FileNotFoundError(f'CXX compiler not found: {command[0]}')
        return [compiler, *command[1:]]
    compiler = shutil.which('clang++') or shutil.which('c++')
    if not compiler:
        raise FileNotFoundError('No C++ compiler found')
    return [compiler]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--check-mutations', action='store_true')
    args = parser.parse_args()
    out = args.output_dir or Path(tempfile.mkdtemp(prefix='mushclient-notepad-close-'))
    out.mkdir(parents=True, exist_ok=True)
    print(f'Artifacts: {out}', flush=True)

    def source(path):
        if args.revision:
            return subprocess.check_output(
                ['git', '-C', str(ROOT), 'show', f'{args.revision}:{path}'], text=True)
        return (ROOT / path).read_text()

    textdoc = source('TextDocument.cpp')
    notepad = source('scripting/methods/methods_notepad.cpp')
    chooser = source('dialogs/ChooseNotepadDlg.cpp')
    app = source('MUSHclient.cpp')
    parts = {
        'GUARD': block(source('TextDocument.h'), 'class CTextDocumentOperationGuard') + ';',
        'DOCUMENT': '\n'.join(block(textdoc, signature) for signature in [
            'void CTextDocument::OnCloseDocument()',
            'void CTextDocument::BeginOperation',
            'void CTextDocument::EndOperation',
            'BOOL CTextDocument::SaveModified()']),
        'COMMAND': block(source('TextView.cpp'), 'BOOL CTextView::OnCmdMsg'),
        'FLIP': block(source('doc.cpp'), 'void CMUSHclientDoc::OnEditFliptonotepad()'),
        'NOTEPAD': '\n'.join(block(notepad, signature) for signature in [
            'bool CMUSHclientDoc::SwitchToNotepad',
            'CTextDocument * CMUSHclientDoc::FindNotepad',
            'long CMUSHclientDoc::CloseNotepad',
            'bool CMUSHclientDoc::AppendToTheNotepad',
            'BOOL CMUSHclientDoc::AppendToNotepad',
            'BOOL CMUSHclientDoc::ReplaceNotepad',
            'VARIANT CMUSHclientDoc::GetNotepadList']),
        'CHOOSER': '\n'.join(block(chooser, signature) for signature in [
            'void CChooseNotepadDlg::DoDataExchange',
            'CMUSHclientDoc * CChooseNotepadDlg::GetLiveWorld',
            'CTextDocument * CChooseNotepadDlg::GetNotepadForIndex',
            'void CChooseNotepadDlg::OnOpenExisting']),
        'DEFER': block(app, 'void CMUSHclientApp::DeferTextDocumentClose'),
        # Compile the exact deferred-close branch of OnIdle. Other idle work
        # needs the full application and is outside this test's scope.
        'IDLE_CLOSE': block(block(app, 'BOOL CMUSHclientApp::OnIdle'),
                            'if (!m_DeferredTextDocumentCloses.empty ())'),
    }
    template = Path(__file__).with_name('notepad_close.cpp.in').read_text()
    for name, value in parts.items():
        marker = '@' + name + '@'
        if template.count(marker) != 1:
            raise ValueError(f'Expected one template marker: {marker}')
        template = template.replace(marker, value)

    compiler = compiler_command()

    def compile_fixture(name, fixture, flags=()):
        cpp = out / (name + '.cpp')
        exe = out / name
        cpp.write_text(fixture)
        subprocess.run([*compiler, '-std=c++17', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        *flags, str(cpp), '-o', str(exe)], check=True)
        return exe.resolve()

    environment = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    cases = ['close_append', 'close_replace', 'cancelled_close', 'repeated_close',
             'stale_chooser', 'enumeration', 'switch', 'immediate_close',
             'flip_pending_only', 'flip_pending_before_live', 'flip_live', 'flip_absent']
    for mode, flags in [('release', []), ('debug', ['-D_DEBUG'])]:
        exe = compile_fixture(mode, template, flags)
        for case in cases:
            result = subprocess.run([str(exe), case], capture_output=True, text=True,
                                    env=environment, timeout=30)
            (out / f'{mode}-{case}.log').write_text(result.stdout + result.stderr)
            print(result.stdout + result.stderr, end='', flush=True)
            result.check_returncode()

    if args.check_mutations:
        mutations = [
            ('flip_selection', 'void CMUSHclientDoc::OnEditFliptonotepad()',
             '!pTextDoc->m_bClosePending &&', '', 'flip_pending_only'),
            ('title_lookup', 'CTextDocument * CMUSHclientDoc::FindNotepad',
             '!pTextDoc->m_bClosePending &&', '', 'close_append'),
            ('switch_count', 'bool CMUSHclientDoc::SwitchToNotepad',
             '!pTextDoc->m_bClosePending &&', '', 'switch'),
            ('list_count', 'VARIANT CMUSHclientDoc::GetNotepadList',
             '    if (pTextDoc->m_bClosePending)\n      continue;', '', 'enumeration'),
            ('list_fill', 'VARIANT CMUSHclientDoc::GetNotepadList',
             '      if (pTextDoc->m_bClosePending)\n        continue;', '', 'enumeration'),
            ('chooser_list', 'void CChooseNotepadDlg::DoDataExchange',
             'pDoc->m_bClosePending ||', '', 'stale_chooser'),
            ('chooser_resolution', 'CTextDocument * CChooseNotepadDlg::GetNotepadForIndex',
             '!pDoc->m_bClosePending &&', '', 'stale_chooser'),
        ]
        for name, signature, old, new, case in mutations:
            original = block(template, signature)
            if original.count(old) != 1:
                raise ValueError(f'Expected one mutation target: {name}')
            mutated = template.replace(original, original.replace(old, new), 1)
            exe = compile_fixture('mutation-' + name, mutated)
            result = subprocess.run([str(exe), case], capture_output=True, text=True,
                                    env=environment, timeout=30)
            (out / f'mutation-{name}.log').write_text(result.stdout + result.stderr)
            if result.returncode == 0 or 'CHECK failed:' not in result.stderr:
                raise RuntimeError(f'Mutation did not fail a check: {name}\n'
                                   + result.stdout + result.stderr)
            print(f'PASS: {name} mutation rejected by {case}', flush=True)


if __name__ == '__main__':
    main()
