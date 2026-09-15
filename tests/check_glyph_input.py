#!/usr/bin/env python3
"""Check the glyph input gate with the official Lua 5.1.4 C runtime.

Requires clang and clang++. Pass --lua-source with a Lua 5.1.4 source directory.
No download is made. --revision selects a baseline from Git.
The exact production prefix before LoadLibrary runs with ASan and UBSan.
The fixture replaces all GDI work with a counter and the converted WORD value.
It tests input validation and conversion, not Windows font lookup or cleanup.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from cpp_blocks import block

ROOT = Path(__file__).resolve().parents[1]
LUA_UNITS = '''lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes
               lparser lstate lstring ltable ltm lundump lvm lzio lauxlib'''.split()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lua-source', required=True, type=Path)
    parser.add_argument('--revision')
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    out = (args.output_dir or Path(tempfile.mkdtemp(prefix='mushclient-glyph-'))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    lua = args.lua_source.resolve()
    if not (lua / 'lua.h').is_file():
        lua = lua / 'src'
    if not re.search(r'#define\s+LUA_RELEASE\s+"Lua 5\.1\.4"', (lua / 'lua.h').read_text()):
        raise ValueError('Official Lua 5.1.4 sources are required')
    path = 'scripting/lua_utils.cpp'
    source = (subprocess.check_output(['git', '-C', str(ROOT), 'show',
                                      f'{args.revision}:{path}'])
              if args.revision else (ROOT / path).read_bytes()).decode('latin-1')
    function = block(source, 'static int glyph_available (lua_State *L)')
    boundary = 'HMODULE hDLL = LoadLibrary ("gdi32");'
    if function.count(boundary) != 1:
        raise ValueError('Expected exactly one GDI acquisition boundary')
    prefix = function[:function.index(boundary)]
    template = Path(__file__).with_name('glyph_input.cpp.in').read_text()
    if template.count('@GLYPH_INPUT@') != 1:
        raise ValueError('Expected exactly one input fixture marker')
    cpp = out / 'glyph_input.cpp'
    cpp.write_text(template.replace('@GLYPH_INPUT@', prefix))
    flags = ['-O1', '-g', '-DNDEBUG', '-fno-omit-frame-pointer',
             '-fsanitize=address,undefined,float-cast-overflow',
             '-fno-sanitize-recover=all', '-I', str(lua)]
    objects = []
    print(f'Artifacts: {out}', flush=True)
    for unit in LUA_UNITS:
        obj = out / (unit + '.o')
        subprocess.run(['clang', '-x', 'c', '-std=c99', *flags, '-c',
                        str(lua / (unit + '.c')), '-o', str(obj)], check=True)
        objects.append(str(obj))
    exe = out / 'glyph_input'
    subprocess.run(['clang++', '-std=c++17', *flags, '-Wall', '-Wextra',
                    str(cpp), *objects, '-lm', '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    (out / 'result.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='', flush=True)
    result.check_returncode()


if __name__ == '__main__':
    main()
