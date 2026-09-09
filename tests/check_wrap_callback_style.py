#!/usr/bin/env python3
"""Check soft-wrap style changes made by a partial-line callback.

Compile the production append, ANSI, style, and line-transition functions with
focused MFC substitutes. The callback calls InterpretANSIcode directly, as
Simulate's ANSI parser does. This check does not run the Windows UI or Lua.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from output_callbacks import replace_once, section


def run(source, output, revision):
    def read(path):
        if revision:
            return subprocess.check_output(
                ['git', '-C', str(source), 'show', f'{revision}:{path}'], text=True)
        return (source / path).read_text()

    doc = read('doc.cpp')
    shim = (Path(__file__).parent / 'output_test_shim.h').read_text()
    shim = replace_once(shim, ' bool StartNewLine(bool,int,bool=true,bool * =nullptr);', '''
 int m_phase=0;
 bool m_bAlternativeInverse=false;
 COLORREF m_boldcolour[16]={},m_normalcolour[16]={},m_customtext[16]={},m_customback[16]={};
 CStyle* AddStyle(unsigned short,COLORREF,COLORREF,int,CAction*,CLine* =nullptr);
 void InterpretANSIcode(int);
 void RememberStyle(const CStyle*);
 bool StartNewLine(bool,int,bool=true,bool * =nullptr);''',
                        'output_test_shim.h ANSI declarations')
    defines = '\n'.join(line for line in read('stdafx.h').splitlines() if line.startswith('#define ANSI_'))
    defines += r"""
#define RED 1
#define GREEN 2
#define YELLOW 3
#define BLUE 4
#define MAGENTA 5
#define CYAN 6
#define ACTIONTYPE 0x0C00
#define STRIKEOUT 0x0020
#define MAX_CUSTOM 16
#define HAVE_FOREGROUND_256_START 100
#define HAVE_BACKGROUND_256_START 101
"""
    body = section(doc, 'bool CMUSHclientDoc::StartNewLine_KeepPreviousStyle',
                   'COutputAppendTransaction::COutputAppendTransaction')
    body += section(doc, 'bool CMUSHclientDoc::AddToLine (',
                    '// called when starting a new line to get colours right')
    body += section(doc, 'bool CMUSHclientDoc::StartNewLine (',
                    'const bool CMUSHclientDoc::CheckScriptingAvailable')
    body += section(doc, ' void CMUSHclientDoc::RemoveChunk (void)',
                    'void CMUSHclientDoc::ShowStatusLine')
    style_end = doc.index('void CMUSHclientDoc::RefreshMXPMissingTagAnchors')
    style_start = doc.rindex('CStyle * CMUSHclientDoc::AddStyle', 0, style_end)
    body += doc[style_start:style_end]
    body += section(doc, 'void CMUSHclientDoc::RememberStyle',
                    'void CMUSHclientDoc::OnDebugWorldInput')
    body += section(read('ansi.cpp'), 'void CMUSHclientDoc::InterpretANSIcode',
                    'void CMUSHclientDoc::Interpret256ANSIcode')
    fixture = Path(__file__).with_suffix('.cpp.in').read_text()
    cpp = output / 'wrap_callback_style.cpp'
    cpp.write_text(defines + shim + body + fixture)
    binary = output / 'wrap_callback_style'
    subprocess.run(['clang++', '-std=c++17', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-g', '-O1', str(cpp),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--revision', help='Read production functions from this Git revision')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        run(args.source, args.output, args.revision)
    else:
        with tempfile.TemporaryDirectory(prefix='mushclient-wrap-style-') as directory:
            run(args.source, Path(directory), args.revision)
