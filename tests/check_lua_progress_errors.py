#!/usr/bin/env python3
"""Check the exact Lprogress_new body with the official Lua 5.1.4 C runtime.

Example: python3 tests/check_lua_progress_errors.py --lua-source /tmp/lua-5.1.4/src
Add --revision a72eea4 to test the faulty baseline (must return nonzero).
CC and CXX select GCC-compatible C and C++17 compilers. No download is made.
Lua sources are copied and compiled as C in the output directory. Foreign C++
exceptions can unwind through these C frames; Lua errors still use longjmp.
Checks run with NDEBUG, then with ASan/UBSan if the compilers support them.

Limits: MFC dialogs and exceptions are substitutes. The fixture supplies its own
userdata finalizer. This does not test native MFC, Windows UI, production close
handling, or Lua allocation failure. C++ allocation counters check string cleanup
even on platforms without LeakSanitizer. Lua and std::string are not substitutes.
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
SOURCE = 'scripting/lua_progressdlg.cpp'
CASES = ['success', 'create_false', 'create_exception', 'title_exception',
         'constructor_exception', 'message_failure', 'message_empty']
LUA_UNITS = '''lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes
              lparser lstate lstring ltable ltm lundump lvm lzio lauxlib lbaselib
              ldblib liolib lmathlib loslib ltablib lstrlib loadlib linit'''.split()
SANITIZERS = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']


def extract(source):
    signature = 'static int Lprogress_new(lua_State *L)'
    if source.count(signature) != 1:
        raise ValueError('Expected exactly one Lprogress_new definition')
    start = source.index(signature)
    depth = 0
    # Ignore braces in comments, ordinary strings, and character literals.
    tokens = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
    for match in re.finditer(tokens, source[start:]):
        if match.group() == '{':
            depth += 1
        elif match.group() == '}':
            depth -= 1
            if depth == 0:
                return source[start:start + match.end()]
    raise ValueError('Unclosed Lprogress_new definition')


def compiler(variable, defaults):
    command = shlex.split(os.environ[variable]) if variable in os.environ else []
    if variable in os.environ and not command:
        raise ValueError(f'{variable} must name a compiler')
    if not command:
        command = [next((name for name in defaults if shutil.which(name)), defaults[0])]
    executable = shutil.which(command[0])
    if executable is None:
        raise FileNotFoundError(f'Compiler not found: {command[0]}')
    return [executable, *command[1:]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lua-source', required=True, type=Path,
                        help='Official Lua 5.1.4 source root or its src directory')
    parser.add_argument('--revision', help='Read production code from this Git revision')
    parser.add_argument('--output-dir', type=Path, help='Default: a new local temporary directory')
    parser.add_argument('--sanitizers', choices=['auto', 'required', 'off'], default='auto')
    args = parser.parse_args()
    out = (args.output_dir or Path(tempfile.mkdtemp(prefix='mushclient-lua-progress-'))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    print(f'Artifacts: {out}', flush=True)
    report_path = out / 'lua_progress_errors.json'
    report_path.unlink(missing_ok=True)
    revision = None
    if args.revision:
        revision = subprocess.check_output(
            ['git', '-C', str(ROOT), 'rev-parse', '--verify', args.revision + '^{commit}'],
            text=True).strip()
    source = (subprocess.check_output(['git', '-C', str(ROOT), 'show', f'{revision}:{SOURCE}'])
              if revision else (ROOT / SOURCE).read_bytes())
    body = extract(source.decode())
    template = Path(__file__).with_name('lua_progress_errors.cpp.in').read_text()
    if template.count('@LPROGRESS_NEW@') != 1:
        raise ValueError('Expected exactly one fixture marker')
    cpp = out / 'lua_progress_errors.cpp'
    cpp.write_bytes(template.replace('@LPROGRESS_NEW@', body).encode())
    report = {'revision': revision or 'working tree', 'source': str(ROOT / SOURCE),
              'source_sha256': hashlib.sha256(source).hexdigest(),
              'function_sha256': hashlib.sha256(body.encode()).hexdigest(),
              'limits': __doc__, 'commands': [], 'runs': []}

    lua_source = args.lua_source.resolve()
    if not (lua_source / 'lua.h').is_file():
        lua_source = lua_source / 'src'
    if not re.search(rb'#define\s+LUA_RELEASE\s+"Lua 5\.1\.4"',
                     (lua_source / 'lua.h').read_bytes()):
        raise ValueError('This fixture requires official Lua 5.1.4 sources')
    lua_copy = out / 'lua-source'
    if lua_copy == lua_source or lua_source in lua_copy.parents:
        raise ValueError('The output directory must be outside the supplied Lua sources')
    lua_copy.mkdir(exist_ok=True)
    report['lua_source'] = str(lua_source)
    report['lua_sha256'] = {}
    for path in sorted([*lua_source.glob('*.h'), *(lua_source / (u + '.c') for u in LUA_UNITS)]):
        data = path.read_bytes()
        (lua_copy / path.name).write_bytes(data)
        report['lua_sha256'][path.name] = hashlib.sha256(data).hexdigest()
    cc = compiler('CC', ['clang', 'cc'])
    cxx = compiler('CXX', ['clang++', 'c++'])
    environment = dict(os.environ, ASAN_OPTIONS='halt_on_error=1',
                       UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')

    def command(argv, log, check=True):
        report['commands'].append(argv)
        result = subprocess.run(argv, cwd=out, env=environment, capture_output=True,
                                text=True, timeout=120)
        output = result.stdout + result.stderr
        (out / log).write_text(output)
        if output:
            print(output, end='', flush=True)
        if check:
            result.check_returncode()
        return result

    modes = [('release', [])]
    report['sanitizers'] = 'disabled by --sanitizers off'
    if args.sanitizers != 'off':
        probe_c = out / 'sanitizer_probe.c'
        probe_cpp = out / 'sanitizer_probe.cpp'
        probe_c.write_text('int probe(void) { return 0; }\n')
        probe_cpp.write_text('extern "C" int probe(void);\nint main() { return probe(); }\n')
        probe_obj = out / 'sanitizer_probe.o'
        probe_exe = out / 'sanitizer_probe'
        supported = command([*cc, '-x', 'c', '-std=c99', *SANITIZERS, '-c', str(probe_c),
                             '-o', str(probe_obj)], 'sanitizer_probe_c.log', False).returncode == 0
        if supported:
            supported = command([*cxx, '-std=c++17', *SANITIZERS, str(probe_cpp),
                                 str(probe_obj), '-o', str(probe_exe)],
                                'sanitizer_probe_link.log', False).returncode == 0
        if supported:
            # Runtime failures are errors, not reasons to skip the sanitizer run.
            command([str(probe_exe)], 'sanitizer_probe_run.log')
            modes.append(('sanitized', SANITIZERS))
            report['sanitizers'] = 'ASan and UBSan enabled'
        else:
            report['sanitizers'] = 'unsupported compiler/linker; see sanitizer_probe logs'
            print('SKIP sanitizers: ' + report['sanitizers'], flush=True)
            if args.sanitizers == 'required':
                raise RuntimeError('Required sanitizer compiler/linker probe failed')

    failures = []
    for mode, sanitizer_flags in modes:
        build = out / mode
        build.mkdir(exist_ok=True)
        common = ['-O1', '-g', '-DNDEBUG', '-fno-omit-frame-pointer',
                  '-fexceptions', '-funwind-tables', *sanitizer_flags]
        objects = []
        print(f'Build {mode}: Lua as C, fixture as C++17, NDEBUG enabled', flush=True)
        for unit in LUA_UNITS:
            obj = build / (unit + '.o')
            # -x c prevents a CXX override or driver default from changing Lua's
            # longjmp implementation into its optional C++ exception implementation.
            command([*cc, '-x', 'c', '-std=c99', *common, '-I', str(lua_copy), '-c',
                     str(lua_copy / (unit + '.c')), '-o', str(obj)], f'{mode}/{unit}.log')
            objects.append(str(obj))
        exe = build / 'lua_progress_errors'
        command([*cxx, '-std=c++17', *common, '-Wall', '-Wextra', '-I', str(lua_copy),
                 str(cpp), *objects, '-lm', '-o', str(exe)], f'{mode}/link.log')
        for case in CASES:
            print(f'Run {mode}/{case}', flush=True)
            result = command([str(exe), case], f'{mode}/{case}.log', False)
            passed = result.returncode == 0 and f'PASS: {case}\n' in result.stdout
            report['runs'].append({'mode': mode, 'case': case, 'passed': passed,
                                   'returncode': result.returncode,
                                   'log': str(build / (case + '.log'))})
            if not passed:
                failures.append(f'{mode}/{case}')
    if not revision and (ROOT / SOURCE).read_bytes() != source:
        failures.append('production source changed during the run; run again')
    report['result'] = 'fail' if failures else 'pass'
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Report: {report_path}', flush=True)
    if failures:
        raise RuntimeError('Regression failures: ' + ', '.join(failures))
    print(f'PASS: {len(CASES)} cases in each of {len(modes)} modes', flush=True)


if __name__ == '__main__':
    main()
