"""Check Load_Set exception ownership and deferred plugin notifications.

Run with Python 3 and clang++. --revision reads production source from Git.
The fixture compiles all three replacement blocks, the caller catch handlers,
notification methods, and the notification block from OnIdle without changes.
MFC types, XML loading, publication, callbacks, and snapshot allocation use
substitutes. This is a source-extracted regression, not a native MFC test.
"""
import argparse
import hashlib
import json
import os
import shlex
import shutil
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


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
    parser.add_argument('--output-dir', required=True, type=Path)
    args = parser.parse_args()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    report = out / 'validation.json'
    report.unlink(missing_ok=True)
    sources = {}

    def source(path):
        raw = (subprocess.check_output(
            ['git', '-C', str(ROOT), 'show', args.revision + ':' + path])
            if args.revision else (ROOT / path).read_bytes())
        sources[path] = hashlib.sha256(raw).hexdigest()
        return raw.decode().replace('\r\n', '\n')

    evaluate = source('evaluate.cpp')
    plugins = source('plugins.cpp')
    application = source('MUSHclient.cpp')
    load = block(evaluate, 'BOOL CMUSHclientDoc::Load_Set (')
    parts = {}
    for kind in ('TRIGGER', 'ALIAS', 'TIMER'):
        case = load[load.index('case ' + kind + ':'):]
        case = case[:case.index('break;')]
        replacement = block(case, 'else\n')
        parts['@' + kind + '@'] = replacement[replacement.index('{'):]

    start = load.index('  catch (CFileException * e)')
    end_marker = '    } // end of catching an archive exception'
    end = load.index(end_marker, start) + len(end_marker)
    parts['@CALLER_CATCHES@'] = load[start:end]
    parts['@NOTIFICATION_METHODS@'] = '\n\n'.join(block(plugins, marker) for marker in (
        'void GetPluginInstanceSnapshot (',
        'void  CMUSHclientDoc::PluginListChanged (',
        'void CMUSHclientDoc::BeginPluginListChangedDeferral (',
        'bool CMUSHclientDoc::EndPluginListChangedDeferral (',
        'void CMUSHclientDoc::SendToAllPluginCallbacks (const string & sName)'))
    idle = block(application, 'BOOL CMUSHclientApp::OnIdle(')
    parts['@IDLE_DELIVERY@'] = block(
        idle, 'if (pDoc->m_bPluginListChangedPending &&')
    fixture = Path(__file__).with_suffix('.cpp.in').read_text()
    for marker, value in parts.items():
        if fixture.count(marker) != 1:
            raise ValueError('Expected one fixture marker: ' + marker)
        fixture = fixture.replace(marker, value)
    cpp = out / 'deferred_notifications.cpp'
    cpp.write_text(fixture)
    exe = out / 'deferred_notifications'
    compiler = shlex.split(os.environ['CXX']) if 'CXX' in os.environ else [
        shutil.which('clang++') or shutil.which('c++')]
    if not compiler or not compiler[0] or not shutil.which(compiler[0]):
        raise SystemExit('No C++ compiler found. Set CXX or install clang++ or c++.')
    command = [*compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
               '-fno-omit-frame-pointer', str(cpp), '-o', str(exe)]
    compile_result = subprocess.run(command, capture_output=True, text=True)
    (out / 'compile.log').write_text(compile_result.stdout + compile_result.stderr)
    print(compile_result.stdout + compile_result.stderr, end='', flush=True)
    compile_result.check_returncode()
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    (out / 'result.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='', flush=True)
    cases = []
    parse_errors = []
    first_parse_error = None
    for number, line in enumerate(result.stdout.splitlines(), 1):
        try:
            cases.append(json.loads(line))
        except json.JSONDecodeError as error:
            parse_errors.append({'line': number, 'output': line, 'error': str(error)})
            if first_parse_error is None:
                first_parse_error = error
    report.write_text(json.dumps({
        'revision': args.revision or 'working-tree',
        'result': 'pass' if result.returncode == 0 and not parse_errors else 'fail',
        'exit_code': result.returncode, 'command': command,
        'source_sha256': sources, 'cases': cases,
        'stdout': result.stdout, 'stderr': result.stderr, 'parse_errors': parse_errors,
        'limits': 'Production replacement blocks, caller catch handlers, notification '
                  'methods, and idle delivery block with MFC, XML, publication, '
                  'callback, and snapshot allocator substitutes. No native parser, '
                  'message loop, MFC allocation failure, or full application run.'
    }, indent=2) + '\n')
    result.check_returncode()
    if first_parse_error is not None:
        raise first_parse_error


if __name__ == '__main__':
    main()
