"""Run extracted XML conversion code with the Windows API.

Use a MinGW C++ compiler. On a non-Windows host, pass --runner with a Wine
executable path and set WINEPREFIX to a test prefix. MFC storage, file access,
exceptions, and ProcessNode use substitutes; text conversion calls WideCharToMultiByte.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(args, output):
    root = Path(__file__).resolve().parents[1]
    def read(path):
        if args.source_ref:
            return subprocess.check_output(['git', 'show', args.source_ref + ':' + path], cwd=root, text=True)
        return (root / path).read_text()
    serializer = read('xml/xml_serialize.cpp')
    parser = read('xml/xmlparse.cpp')
    probe = serializer[serializer.index('#define COMPARE_MEMORY'):serializer.index('void SeeIfBase64')]
    start = parser.index('void CXMLparser::BuildStructure (')
    build = parser[start:parser.index('// end of CXMLparser::BuildStructure', start)]
    template = (root / 'tests/xml_unicode.cpp.in').read_text()
    cpp = output / 'xml_unicode.cpp'
    cpp.write_text(template.replace('// PRODUCTION_PROBE', probe).replace('// PRODUCTION_BUILD', build))
    binary = output / 'xml_unicode.exe'
    subprocess.run([args.compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-O1', '-static', '-D_WIN32_WINNT=0x0501', '-DWINVER=0x0501', str(cpp), '-o', str(binary)], check=True)
    command = ([args.runner] if args.runner else []) + [str(binary), args.case]
    subprocess.run(command, check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default='x86_64-w64-mingw32-g++')
    parser.add_argument('--runner')
    parser.add_argument('--source-ref')
    parser.add_argument('--case', choices=['all', 'odd', 'surrogates', 'preservation'], default='all')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        run(args, args.output)
    else:
        with tempfile.TemporaryDirectory(prefix='xml-unicode-') as path:
            run(args, Path(path))
