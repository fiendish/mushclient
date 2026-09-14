"""Check OnChar action state and ReloadPlugin exception notifications.

Compile production methods with MFC, plugin loading, and callback substitutes.
This does not run the native message loop, parser, or Lua/COM engines.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def between(text, start, stop):
    if text.count(start) != 1:
        raise ValueError('Expected one source marker: ' + start)
    begin = text.index(start)
    return text[begin:text.index(stop, begin)]


def block(text, marker):
    if text.count(marker) != 1:
        raise ValueError('Expected one source marker: ' + marker)
    start = text.index(marker)
    brace = text.index('{', start)
    depth = 0
    for end in range(brace, len(text)):
        depth += (text[end] == '{') - (text[end] == '}')
        if depth == 0:
            return text[start:end + 1]
    raise ValueError('Unclosed source block: ' + marker)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision')
    parser.add_argument('--phase', choices=('action', 'reload', 'all'), default='all')
    parser.add_argument('--output-dir', required=True, type=Path)
    args = parser.parse_args()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = output / 'result.json'
    report.unlink(missing_ok=True)
    compiler = (shlex.split(os.environ['CXX']) if 'CXX' in os.environ else
                [shutil.which('clang++') or shutil.which('c++')])
    if not compiler or not compiler[0] or not shutil.which(compiler[0]):
        raise SystemExit('No C++ compiler found. Set CXX or install clang++ or c++.')
    hashes = {}

    def source(path):
        raw = (subprocess.check_output(['git', '-C', str(ROOT), 'show',
                                       args.revision + ':' + path])
               if args.revision else (ROOT / path).read_bytes())
        hashes[path] = hashlib.sha256(raw).hexdigest()
        return raw.decode().replace('\r\n', '\n')

    results = []
    phases = ('action', 'reload') if args.phase == 'all' else (args.phase,)
    for phase in phases:
        if phase == 'action':
            lua = between(source('scripting/lua_scripting.cpp'),
                          '    }   // end of having an optional style run thingo',
                          '// -----------------')
            parts = {
                '@ON_CHAR@': block(source('sendvw.cpp'), 'void CSendView::OnChar('),
                '@VALUE_GUARD@': block(source('stdafx.h'),
                                      'template <class T>\nclass CValueStateGuard') + ';',
                '@ACTION_ENUM@': between(source('doc.h'),
                                         '// values for m_iCurrentActionSource',
                                         '// value for m_iStopTriggerEvaluation'),
                '@GET_ACTION@': between(source('scripting/methods/methods_info.cpp'),
                                        '    case 239:', '    case 240:'),
                '@LUA_ACTION_GUARD@': between(lua,
                    '  CValueStateGuard<unsigned short> actionSourceGuard',
                    '  error = CallLuaWithTraceBack'),
            }
        else:
            plugins = source('plugins.cpp')
            idle = block(source('MUSHclient.cpp'), 'BOOL CMUSHclientApp::OnIdle(')
            parts = {
                '@RELOAD@': block(source('scripting/methods/methods_plugins.cpp'),
                                  'long CMUSHclientDoc::ReloadPlugin('),
                '@CONTEXT_GUARD@': block(source('plugins.h'),
                                        'class CPluginContextGuard\n') + ';',
                '@GUARD_METHODS@': '\n'.join(block(plugins, marker) for marker in (
                    'CPluginContextGuard::CPluginContextGuard (',
                    'CPluginContextGuard::~CPluginContextGuard ()')),
                '@NOTIFICATIONS@': '\n'.join(block(plugins, marker) for marker in (
                    'void  CMUSHclientDoc::PluginListChanged (',
                    'void CMUSHclientDoc::BeginPluginListChangedDeferral (',
                    'bool CMUSHclientDoc::EndPluginListChangedDeferral (')),
                '@IDLE@': block(idle, 'if (pDoc->m_bPluginListChangedPending &&'),
                '@ERROR_CODES@': source('scripting/errors.h'),
            }
        fixture = ROOT / 'tests' / ('check_' + phase + '_exit.cpp.in')
        program = fixture.read_text()
        for marker, value in parts.items():
            if program.count(marker) != 1:
                raise ValueError('Expected one fixture marker: ' + marker)
            program = program.replace(marker, value)
        cpp = output / (phase + '.cpp')
        cpp.write_text(program)
        binary = output / phase
        command = [*compiler, '-std=c++17', '-O1', '-g', '-DNDEBUG',
                   '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                   '-fno-omit-frame-pointer', str(cpp), '-o', str(binary)]
        build = subprocess.run(command, capture_output=True, text=True)
        (output / (phase + '-compile.log')).write_text(build.stdout + build.stderr)
        print(build.stdout + build.stderr, end='', flush=True)
        build.check_returncode()
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        log = output / (phase + '-run.log')
        log.write_text(run.stdout + run.stderr)
        print(run.stdout + run.stderr, end='', flush=True)
        cases, parse_errors = [], []
        first_parse_error = None
        for number, line in enumerate(run.stdout.splitlines(), 1):
            try:
                cases.append(json.loads(line))
            except json.JSONDecodeError as error:
                parse_errors.append({'line': number, 'output': line, 'error': str(error)})
                if first_parse_error is None:
                    first_parse_error = error
        results.append({'phase': phase, 'command': command, 'exit_code': run.returncode,
                        'result': 'pass' if run.returncode == 0 and not parse_errors else 'fail',
                        'log': str(log), 'cases': cases, 'parse_errors': parse_errors,
                        'stdout': run.stdout, 'stderr': run.stderr,
                        'fixture_sha256': hashlib.sha256(fixture.read_bytes()).hexdigest()})
        report.write_text(json.dumps({
            'revision': args.revision or 'working-tree', 'source_sha256': hashes,
            'results': results, 'limits': __doc__,
        }, indent=2) + '\n')
        run.check_returncode()
        if first_parse_error is not None:
            raise first_parse_error


if __name__ == '__main__':
    main()
