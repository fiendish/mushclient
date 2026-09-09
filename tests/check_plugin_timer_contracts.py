"""Check plugin guard copy restrictions and timer debug output.

Compile exact guard declarations and methods, GetTimerMap, and the DebugHelper
entry and timer branch. MFC strings, maps, time, and output are substitutes.
Other DebugHelper branches and the native hyperlink UI are not exercised.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
GUARDS = ('CPluginCallGuard', 'CPluginContextGuard', 'CPluginNotesGuard')


def block(text, marker):
    start = text.index(marker)
    brace = text.index('{', start)
    depth = 0
    for end in range(brace, len(text)):
        depth += (text[end] == '{') - (text[end] == '}')
        if depth == 0:
            return text[start:end + 1]
    raise ValueError(f'Unterminated source block: {marker}')


def compiler_command():
    override = os.environ.get('CXX')
    if override is not None:
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--phase', choices=('guards', 'timers', 'all'), default='all')
    args = parser.parse_args()
    output = (args.output_dir or Path(tempfile.mkdtemp(prefix='mushclient-contracts-'))).resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = output / 'result.json'
    report.unlink(missing_ok=True)
    command = compiler_command()
    inputs = {}

    def source(path):
        data = (subprocess.check_output(['git', '-C', str(ROOT), 'show',
                                       f'{args.revision}:{path}'])
                if args.revision else (ROOT / path).read_bytes())
        inputs[path] = hashlib.sha256(data).hexdigest()
        return data.decode('utf-8').replace('\r\n', '\n')

    header = source('plugins.h')
    plugins = source('plugins.cpp')
    debug = source('world_debug.cpp')
    stdafx = source('stdafx.h')
    start = debug.index('void CMUSHclientDoc::DebugHelper (')
    stop = debug.index('  if (strAction == "triggerlist")', start)
    helper = debug[start:stop] + '  if (false) {}\n'
    helper += block(debug, '  else if (strAction == "timerlist")') + '\n}\n'
    # Include the original lookup when checking a revision before the fix.
    lookup = ('static CString FindTimerName (' in debug)
    parts = {
        '@GUARDS@': '\n'.join(block(header, 'class ' + name) + ';' for name in GUARDS),
        '@GUARD_METHODS@': '\n'.join(
            block(plugins, f'{name}::{method} (')
            for name in GUARDS[1:] for method in (name, '~' + name)),
        '@GET_TIMER_MAP@': block(source('doc.h'), 'CTimerMap & GetTimerMap (void)'),
        '@DEBUG_HELPER@': ((block(debug, 'static CString FindTimerName (') + '\n')
                           if lookup else '') + helper,
        '@CONSTANTS@': '\n'.join(next(line for line in stdafx.splitlines()
                                     if line.startswith('#define ' + name + ' '))
                                for name in ('DEBUG_PLUGIN_ID', 'PLUGIN_UNIQUE_ID_LENGTH')),
    }
    fixture = Path(__file__).with_suffix('.cpp.in')
    program = fixture.read_text()
    for marker, value in parts.items():
        if program.count(marker) != 1:
            raise ValueError(f'Expected one fixture marker: {marker}')
        program = program.replace(marker, value)
    cpp = output / 'plugin_timer_contracts.cpp'
    cpp.write_text(program)
    checks = []
    if args.phase in ('guards', 'all'):
        for name in GUARDS:
            for operation, statement in (
                    ('copy', f'{name} copied(first);'),
                    ('assignment', 'first = second;')):
                path = output / f'{name}_{operation}.cpp'
                path.write_text(program + f'\nvoid Unsafe({name}& first, '
                                f'{name}& second) {{ {statement} }}\n')
                cmd = command + ['-std=c++17', '-fsyntax-only', str(path)]
                result = subprocess.run(cmd, capture_output=True, text=True)
                log = output / f'{name}_{operation}.log'
                log.write_text(result.stdout + result.stderr)
                # A missing include or compiler failure is not a copy restriction.
                if result.returncode != 1 or name not in result.stderr or not any(
                        term in result.stderr for term in ('private', 'deleted', 'inaccessible')):
                    print(result.stdout + result.stderr, end='', flush=True)
                    raise AssertionError(f'{name} {operation} did not fail for access control.')
                print(f'{name} {operation}: rejected at compile time', flush=True)
                checks.append({'name': f'{name} {operation}', 'command': cmd,
                               'exit_code': result.returncode, 'log': str(log)})
    binary = output / 'plugin_timer_contracts'
    cmd = command + ['-std=c++17', '-O1', '-g', '-DNDEBUG',
                     '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                     '-fno-omit-frame-pointer', str(cpp), '-o', str(binary)]
    subprocess.run(cmd, check=True)
    result = subprocess.run([str(binary), args.phase], capture_output=True, text=True)
    log = output / 'runtime.log'
    log.write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='', flush=True)
    result.check_returncode()
    report.write_text(json.dumps({
        'revision': args.revision or 'working-tree', 'source_sha256': inputs,
        'fixture_sha256': hashlib.sha256(fixture.read_bytes()).hexdigest(),
        'copy_checks': checks, 'runtime_command': cmd, 'runtime_log': str(log),
        'status': 'pass',
        'limits': 'MFC strings, maps, time, and output are substitutes. '
                  'Only the DebugHelper entry and timer branch are compiled. '
                  'No native Windows UI or live Lua/COM callback is exercised.',
    }, indent=2) + '\n')
    print('Artifacts: ' + str(output), flush=True)


if __name__ == '__main__':
    main()
