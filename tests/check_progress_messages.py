"""Run progress dispatch and lifetime regression tests from production C++.

Portable: python tests/check_progress_messages.py
Windows: from an MSVC developer prompt, python tests/check_progress_messages.py --native-windows
Historical fault: add --pump-revision 3318da8 --case blocked_chat (must fail).

Portable mode requires ASan/UBSan. Native mode uses Win32 queues, sent messages,
and WSAAsyncSelect with a loopback socket; MFC and Lua are test substitutes.
No source files are changed. Generated C++, executables and logs stay in the
output directory. Extraction, compilation and test errors propagate.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def block(source, signature):
    start = source.index(signature)
    depth = 0
    # Ignore braces in comments, strings and character literals.
    tokens = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|\{|\}'
    for token in re.finditer(tokens, source[start:]):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return source[start:start + token.end()]
    raise ValueError(f'Unclosed production block: {signature}')


def compiler_command(native):
    override = os.environ.get('CXX', '')
    if override:
        # A quoted compiler path works on Windows as well as POSIX. Native
        # callers can omit CXX and use cl from their developer prompt.
        command = shlex.split(override, posix=os.name != 'nt')
        command = [part[1:-1] if part.startswith('"') and part.endswith('"') else part
                   for part in command]
        if not command:
            raise ValueError('CXX must name a C++ compiler')
        compiler = shutil.which(command[0])
        if not compiler:
            raise FileNotFoundError(f'CXX compiler not found: {command[0]}')
        return [compiler, *command[1:]]
    names = ['cl'] if native else ['clang++', 'c++']
    for name in names:
        compiler = shutil.which(name)
        if compiler:
            return [compiler]
    raise FileNotFoundError(f'No compiler found: {names}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--revision', help='Read all production inputs from this Git revision')
    parser.add_argument('--pump-revision', help='Replace only PumpMessages with this historical body')
    parser.add_argument('--native-windows', action='store_true')
    parser.add_argument('--emit-only', action='store_true', help='Extract without compiling or running')
    parser.add_argument('--case', action='append', dest='cases')
    args = parser.parse_args()
    out = (args.output_dir or Path(tempfile.mkdtemp(prefix='mushclient-progress-'))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    print(f'Artifacts: {out}', flush=True)
    inputs = {}

    def source(path, revision=None):
        revision = revision or args.revision
        text = (subprocess.check_output(['git', '-C', str(ROOT), 'show', f'{revision}:{path}'], text=True)
                if revision else (ROOT / path).read_text())
        inputs[f'{revision or "working-tree"}:{path}'] = hashlib.sha256(text.encode()).hexdigest()
        return text

    progress = source('dialogs/ProgDlg.cpp')
    if args.pump_revision:
        signature = 'void CProgressDlg::PumpMessages()'
        progress = progress.replace(block(progress, signature),
                                    block(source('dialogs/ProgDlg.cpp', args.pump_revision), signature), 1)
    # Keep the full production class and implementation, removing only project
    # includes. Test substitutes provide the MFC boundary.
    progress = re.sub(r'^#include[^\n]*\n', '', progress, flags=re.MULTILINE)
    lua = source('scripting/lua_progressdlg.cpp')
    doc = source('doc.cpp')
    textdoc = source('TextDocument.cpp')
    app = source('MUSHclient.cpp')
    parts = {
        'PROGRESS_HEADER': source('dialogs/ProgDlg.h'),
        'PROGRESS_BODY': progress,
        'LUA': '\n'.join(block(lua, signature) for signature in [
            'static CProgressDlg * Lprogress_getdialog',
            'static int Lprogress_setstatus', 'static int Lprogress_setrange',
            'static int Lprogress_setposition', 'static int Lprogress_setstep',
            'static int Lprogress_stepit', 'static int Lprogress_checkcancel',
            'static int Lprogress_gc', 'static int Lprogress_new']),
        'SEND': block(source('chatsock.cpp'), 'void CChatSocket::OnSend('),
        'WORLD_GUARD': block(source('doc.h'), 'class CWorldDocumentOperationGuard') + ';',
        'TEXT_GUARD': block(source('TextDocument.h'), 'class CTextDocumentOperationGuard') + ';',
        'DOCUMENTS': '\n'.join([
            *(block(doc, s) for s in ['void CMUSHclientDoc::OnCloseDocument()',
                                     'void CMUSHclientDoc::BeginProgressOperation',
                                     'void CMUSHclientDoc::EndProgressOperation']),
            *(block(textdoc, s) for s in ['void CTextDocument::OnCloseDocument()',
                                         'void CTextDocument::BeginOperation',
                                         'void CTextDocument::EndOperation'])]),
        'DEFERRED_TYPE': block(source('MUSHclient.h'), 'struct CDeferredMessage') + ';',
        'APP': '\n'.join(block(app, s) for s in [
            'void CMUSHclientApp::DeferMessageUntilIdle',
            'void CMUSHclientApp::DeferWorldDocumentClose',
            'void CMUSHclientApp::DeferTextDocumentClose',
            'bool CMUSHclientApp::HasActiveDocumentOperations',
            'BOOL CMUSHclientApp::OnIdle(']),
        'NATIVE': Path(__file__).with_name('progress_messages_native.cpp.in').read_text()
                  if args.native_windows else '',
    }
    fixture = Path(__file__).with_name('progress_messages.cpp.in').read_text()
    for name, value in parts.items():
        marker = '@' + name + '@'
        if fixture.count(marker) != 1:
            raise ValueError(f'Expected exactly one template marker: {marker}')
        fixture = fixture.replace(marker, value)
    if re.search(r'@[A-Z_]+@', fixture):
        raise ValueError('Unexpanded template marker')
    cpp = out / 'progress_messages.cpp'
    cpp.write_text(fixture)
    (out / 'inputs.sha256').write_text(''.join(f'{value}  {key}\n' for key, value in inputs.items()))
    if args.emit_only:
        return
    if args.native_windows and os.name != 'nt':
        raise RuntimeError('Native execution requires Windows; use --emit-only for cross compilation')
    compiler = compiler_command(args.native_windows)
    exe = out / ('progress_messages.exe' if os.name == 'nt' else 'progress_messages')
    if args.native_windows:
        command = [*compiler, '/nologo', '/std:c++17', '/EHsc', '/W4', '/Od', '/Zi',
                   '/DPROGRESS_NATIVE_WINDOWS', str(cpp), '/Fe:' + str(exe),
                   '/Fo:' + str(out / 'progress_messages.obj'),
                   '/Fd:' + str(out / 'progress_messages.pdb'),
                   '/link', 'user32.lib', 'ws2_32.lib']
    else:
        command = [*compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra',
                   '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                   '-fno-omit-frame-pointer', str(cpp), '-o', str(exe)]
    print('Compile:', command, flush=True)
    subprocess.run(command, check=True, cwd=out)
    cases = args.cases or [
        'blocked_chat', 'cancel_quit', 'cancel_stops', 'late_quit', 'nested_depth', 'exception_depth',
        'lua_nested_close', 'lua_immediate_close', 'lua_control_close', 'lua_exception',
        'destroy_dispatch', 'destroy_sent', 'sent_foreign', 'destroy_control', 'source_control_order',
        'window_identity', 'parent_lifetime', 'lua_create', 'lua_create_failure',
        'lua_argument_failure', 'lua_argument_conversion_close',
        'world_close', 'text_close', 'close_during_pump', 'queued_then_active',
        'stale_identity', 'document_exception', 'deferred_messages']
    if args.native_windows and not args.cases:
        cases += ['native_socket']
    environment = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    for case in cases:
        result = subprocess.run([str(exe), case], capture_output=True, text=True,
                                env=environment, timeout=45, cwd=out)
        log = result.stdout + result.stderr
        (out / (case + '.log')).write_text(log)
        print(log, end='', flush=True)
        result.check_returncode()
    print(f'PASS: {len(cases)} cases', flush=True)


if __name__ == '__main__':
    main()
