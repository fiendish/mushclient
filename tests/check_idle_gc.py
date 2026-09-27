"""Test idle Lua GC with the packaged runtime and production scheduling code.

Run from an x86 MSVC developer prompt:
  python tests/check_idle_gc.py --lua-runtime PATH_TO_RUNTIME_DIRECTORY
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from cpp_blocks import block

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lua-runtime', type=Path, required=True)
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    app = (ROOT / 'MUSHclient.cpp').read_text()
    script = (ROOT / 'scripting/lua_scripting.cpp').read_text()
    guards = (ROOT / 'stdafx.h').read_text()
    template = (ROOT / 'tests/idle_gc.cpp.in').read_text()
    definitions = '\n\n'.join([
        block(script, 'bool CScriptEngine::CollectLuaGarbage ('),
        block(app, 'struct CIdleLuaTarget') + ';',
        block(app, 'void CMUSHclientApp::CollectIdleLuaGarbage ('),
    ])
    source = template.replace('@GUARD@', 'template <class T>\n' +
                              block(guards, 'class CValueStateGuard') + ';')
    source = source.replace('@FUNCTIONS@', definitions)
    directory = Path(tempfile.mkdtemp(prefix='mushclient-idle-gc-'))
    cpp = directory / 'test.cpp'
    cpp.write_text(source)
    exe = directory / 'test.exe'
    command = ['cl', '/nologo', '/EHsc', '/MT', '/O2', '/std:c++14',
               '/D_CRT_SECURE_NO_WARNINGS', '/I' + str(ROOT), str(cpp),
               str(ROOT / 'lua5.1.lib'), '/Fe' + str(exe),
               '/Fo' + str(directory / 'test.obj')]
    if args.asan:
        command += ['/fsanitize=address', '/Zi', '/Fd' + str(directory / 'test.pdb')]
    subprocess.run(command, cwd=directory, check=True)
    env = os.environ.copy()
    env['PATH'] = str(args.lua_runtime.resolve()) + os.pathsep + env['PATH']
    subprocess.run([str(exe)], cwd=directory, env=env, check=True)
    print('Test artifacts:', directory)

if __name__ == '__main__':
    main()
