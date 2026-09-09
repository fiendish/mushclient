"""Check Execute state restoration with extracted production C++.

Run with Python 3 and clang++. --revision reads a pinned Git revision.
The fixture compiles the complete Execute function, its state guards, the
manual-input reset and Execute call, and the GetInfo(230) case. MFC strings,
lists, output, and script/plugin callbacks are substitutes. No native modal
session is exercised. Build errors and failed cases produce a nonzero exit.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def between(text, start, stop):
    begin = text.index(start)
    return text[begin:text.index(stop, begin)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--cxx', default='clang++')
    args = parser.parse_args()
    out = args.output_dir or Path(tempfile.mkdtemp(prefix='mushclient-execute-'))
    out.mkdir(parents=True, exist_ok=True)
    revision = None
    if args.revision:
        revision = subprocess.check_output(
            ['git', '-C', str(ROOT), 'rev-parse', args.revision + '^{commit}'],
            text=True).strip()
    inputs = {}

    def source(path):
        if revision:
            data = subprocess.check_output(
                ['git', '-C', str(ROOT), 'show', revision + ':' + path])
        else:
            data = (ROOT / path).read_bytes()
        inputs[path] = hashlib.sha256(data).hexdigest()
        return data.decode('utf-8').replace('\r\n', '\n')

    methods = source('scripting/methods/methods_commands.cpp')
    guards = source('stdafx.h')
    send = source('sendvw.cpp')
    info = source('scripting/methods/methods_info.cpp')
    header = source('doc.h')
    manual = between(send, '    } // end of auto say', '    }  // end not auto-say')
    manual = manual[manual.index('    pDoc->m_iExecutionDepth = 0;'):]
    depth_info = re.search(r'case\s+230\s*:.*?break;', info).group()
    depth_limit = re.search(r'^#define MAX_EXECUTION_DEPTH\s+\d+.*$',
                            header, re.MULTILINE).group()
    parts = {
        '@EXECUTE@': between(methods, 'long CMUSHclientDoc::Execute(',
                            'long CMUSHclientDoc::DoCommand('),
        '@GUARDS@': between(guards, 'template <class T>\nclass CValueStateGuard',
                           '// translates "send to" numbers') +
            between(guards, 'class CBoolStateGuard', '// translates connection status'),
        '@MANUAL_INPUT@': manual,
        '@GET_DEPTH@': depth_info,
        '@DEPTH_LIMIT@': depth_limit,
        '@ERROR_CODES@': source('scripting/errors.h'),
    }
    fixture_path = Path(__file__).with_suffix('.cpp.in')
    fixture = fixture_path.read_text()
    for marker, value in parts.items():
        if fixture.count(marker) != 1:
            raise ValueError('Expected one fixture marker: ' + marker)
        fixture = fixture.replace(marker, value)
    cpp = out / 'execute_state.cpp'
    cpp.write_text(fixture)
    executable = out / 'execute_state'
    command = [args.cxx, '-std=c++17', '-O1', '-g',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
               '-fno-omit-frame-pointer', str(cpp), '-o', str(executable)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True)
    log = out / 'run.log'
    log.write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='', flush=True)
    (out / 'result.json').write_text(json.dumps({
        'revision': revision or 'working-tree',
        'source_sha256': inputs,
        'fixture_sha256': hashlib.sha256(fixture_path.read_bytes()).hexdigest(),
        'command': command,
        'exit_code': result.returncode,
        'log': str(log),
        'limits': 'Extracted production code with MFC and callback substitutes. '
                  'No native Windows modal session or live Lua/COM execution.',
    }, indent=2) + '\n')
    print('Artifacts: ' + str(out), flush=True)
    result.check_returncode()


if __name__ == '__main__':
    main()
