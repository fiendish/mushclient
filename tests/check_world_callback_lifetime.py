"""Test world retention in callback and script execution paths.

Requires Python 3 and clang++. Uses ASan/UBSan and MFC/socket stubs.
The fixture delivers close requests during callbacks and drains deferred closes
at explicit safe points. The timer fixture extracts only marked-chat cleanup;
a continuation counter represents the remaining timer work. It does not prove
native modal-message dispatch or full timer scheduling.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from cpp_blocks import block

ROOT = Path(__file__).resolve().parents[1]


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f'Expected one mutation target: {old!r}')
    return source.replace(old, new)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision', help='Read production source from a Git revision.')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--skip-mutations', action='store_true')
    args = parser.parse_args()
    out = args.output_dir or Path(tempfile.mkdtemp(prefix='mushclient-world-callback-'))
    out.mkdir(parents=True, exist_ok=True)

    def read(path):
        if args.revision:
            return subprocess.check_output(
                ['git', '-C', str(ROOT), 'show', f'{args.revision}:{path}'], text=True)
        return (ROOT / path).read_text()

    def require_before(body, first, second, label):
        first_position = body.find(first)
        second_position = body.find(second)
        if first_position < 0 or second_position < 0 or first_position > second_position:
            raise ValueError(f'{label}: required guard order is missing')

    mushview = read('mushview.cpp')
    context_menu = block(mushview, 'void CMUSHView::OnContextMenu(')
    require_before(context_menu, 'CWorldDocumentOperationGuard operationGuard (pDoc);',
                   'CValueStateGuard<int> actionGuard', 'OnContextMenu')
    if context_menu.count('if (nCommand && !pDoc->m_bWorldClosePending)') != 2:
        raise ValueError('OnContextMenu: close-pending checks are missing')

    scriptengine = read('scripting/scriptengine.cpp')
    for signature in ('bool CScriptEngine::Execute (', 'bool CScriptEngine::Parse ('):
        require_before(block(scriptengine, signature),
                       'CWorldDocumentOperationGuard operationGuard (m_pDoc);',
                       'CPluginCallGuard callGuard', signature)

    lua = read('scripting/lua_scripting.cpp')
    require_before(block(lua, 'bool CScriptEngine::ParseLua ('),
                   'CWorldDocumentOperationGuard operationGuard (m_pDoc);',
                   'CPluginCallGuard callGuard', 'ParseLua')
    execute_lua_blocks = []
    offset = 0
    signature = 'bool CScriptEngine::ExecuteLua ('
    while (start := lua.find(signature, offset)) >= 0:
        function = block(lua[start:], signature)
        execute_lua_blocks.append(function)
        offset = start + len(function)
    if len(execute_lua_blocks) != 2:
        raise ValueError(f'Expected two ExecuteLua overloads, found {len(execute_lua_blocks)}')
    for index, function in enumerate(execute_lua_blocks, 1):
        require_before(function, 'CWorldDocumentOperationGuard operationGuard (m_pDoc);',
                       'CPluginCallGuard callGuard', f'ExecuteLua overload {index}')

    chat = read('chatsock.cpp')
    doc = read('doc.cpp')
    require_before(block(doc, 'void CMUSHclientDoc::SendTo ('),
                   'CWorldDocumentOperationGuard operationGuard (this);',
                   'switch (iWhere)', 'SendTo')

    miniwindows = read('scripting/methods/methods_miniwindows.cpp')
    require_before(block(miniwindows, 'BSTR CMUSHclientDoc::WindowMenu('),
                   'CWorldDocumentOperationGuard operationGuard (this);',
                   'it->second->Menu', 'WindowMenu')

    stdafx = read('stdafx.h')
    functions = '\n\n'.join([
        *(block(doc, signature) for signature in [
            'void CMUSHclientDoc::OnCloseDocument()',
            'void CMUSHclientDoc::BeginProgressOperation ()',
            'void CMUSHclientDoc::EndProgressOperation ()']),
        block(doc, 'CChatSocket * CMUSHclientDoc::GetChatSocket ('),
        block(read('childfrm.cpp'), 'void CChildFrame::OnClose()'),
        block(chat, 'void CChatSocket::OnReceive('),
        block(chat, 'void CChatSocket::OnClose('),
        block(read('scripting/methods/methods_chat.cpp'), 'long CMUSHclientDoc::ChatDisconnect('),
        block(chat, 'void CChatSocket::ProcessChatMessage ('),
        block(chat, 'void CChatSocket::Process_Send_command'),
        block(chat, 'void CChatSocket::Process_Message'),
        block(read('scripting/methods/methods_commands.cpp'), 'long CMUSHclientDoc::Execute('),
        block(read('scripting/scripting.cpp'), 'void CMUSHclientDoc::OnScriptFileChanged('),
    ])
    guards = '\n\n'.join([
        block(stdafx, 'template <class T>\nclass CValueStateGuard') + ';',
        block(stdafx, 'class CBoolStateGuard') + ';',
        block(read('doc.h'), 'class CWorldDocumentOperationGuard') + ';',
    ])
    cleanup = block(block(read('timers.cpp'), 'void CMUSHclientDoc::CheckTimerList ('),
                    'for (POSITION chatpos = m_ChatList.GetHeadPosition (); chatpos; )')
    template = Path(__file__).with_name('world_callback_lifetime.cpp.in').read_text()
    source = replace_once(replace_once(template, '@GUARDS@', guards), '@FUNCTIONS@', functions)
    source = replace_once(source, '@CHAT_CLEANUP@', cleanup)

    def compile_source(name, content, flags=()):
        cpp = out / f'{name}.cpp'
        cpp.write_text(content)
        exe = out / name
        subprocess.run(['clang++', '-std=c++17', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-omit-frame-pointer',
                        *flags, str(cpp), '-o', str(exe)], check=True)
        return exe

    evidence = {'production': [], 'mutations': []}
    for name, flags in [('world_callback_lifetime', ()),
                        ('world_callback_lifetime_ndebug', ('-DNDEBUG',))]:
        exe = compile_source(name, source, flags)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        (out / f'{name}.log').write_text(result.stdout + result.stderr)
        print(result.stdout + result.stderr, end='')
        result.check_returncode()
        evidence['production'].append({'name': name, 'exit_code': result.returncode})

    if not args.skip_mutations:
        chat_guard = '  CWorldDocumentOperationGuard operationGuard (m_pDoc);'
        execute_guard = 'CWorldDocumentOperationGuard executeOperationGuard (this);'
        script_guard = '  CWorldDocumentOperationGuard operationGuard (this);'
        inner_guard = '    CValueStateGuard<int> executionDepthGuard (m_pDoc->m_iExecutionDepth, 0);'
        no_chat_guard = replace_once(source, chat_guard, '')
        mutants = [
            ('remove_execute_guard', replace_once(source, execute_guard, ''),
                'execute', 'world retained during command'),
            ('remove_chat_guard', no_chat_guard, 'chat', 'world retained after command'),
            ('move_chat_guard_to_command', replace_once(no_chat_guard, inner_guard,
                chat_guard + '\n' + inner_guard), 'chat', 'world retained after command'),
            ('remove_script_guard', replace_once(source, script_guard, ''),
                'script', 'world retained during script reload'),
            ('remove_chat_cleanup_condition', replace_once(source,
                'pSocket->m_bDeleteMe && m_iActiveProgressOperations == 0',
                'pSocket->m_bDeleteMe'), 'timer', 'marked chat survives modal timer cleanup'),
            ('return_before_timer_cleanup', replace_once(source, cleanup,
                'if (m_iActiveProgressOperations != 0) return;\n' + cleanup),
                'timer', 'timer work continues during an active operation'),
        ]
        for name, mutant, case, expected in mutants:
            exe = compile_source(name, mutant, ('-DNDEBUG',))
            result = subprocess.run([str(exe), case], capture_output=True, text=True, timeout=30)
            log = result.stdout + result.stderr
            (out / f'{name}.log').write_text(log)
            if result.returncode == 0 or expected not in log:
                raise AssertionError(f'{name} did not fail at the expected check:\n{log}')
            evidence['mutations'].append({'name': name, 'exit_code': result.returncode,
                                          'expected_failure': expected})
            print(f'PASS: {name} fails at {expected}')
    (out / 'results.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(f'Artifacts: {out}')


if __name__ == '__main__':
    main()
