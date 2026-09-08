#!/usr/bin/env python3
"""Compile production output functions with focused MFC substitutes.

Run with Python 3 and clang++ on macOS or Linux. This check does not build MFC.
The C++ assertions check output, styles, callback delivery, and failure returns.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def run(source_root, output):
    doc = (source_root / 'doc.cpp').read_text()
    shim = (Path(__file__).parent / 'output_test_shim.h').read_text()
    body = '\n'.join([
        section(doc, 'bool CMUSHclientDoc::StartNewLine_KeepPreviousStyle',
                'COutputAppendTransaction::COutputAppendTransaction'),
        section(doc, 'bool CMUSHclientDoc::AddToLine (',
                '// called when starting a new line to get colours right'),
        section(doc, 'bool CMUSHclientDoc::StartNewLine (',
                'const bool CMUSHclientDoc::CheckScriptingAvailable'),
        section(doc, ' void CMUSHclientDoc::RemoveChunk (void)',
                'void CMUSHclientDoc::ShowStatusLine'),
    ])
    main = (Path(__file__).parent / 'output_callbacks_main.cpp').read_text()
    cpp = output / 'output_callbacks.cpp'
    cpp.write_text(shim + body + main)
    binary = output / 'output_callbacks'
    subprocess.run(['clang++', '-std=c++17', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-g', '-O1', str(cpp),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        run(args.source, args.output)
    else:
        with tempfile.TemporaryDirectory(prefix='mushclient-output-') as directory:
            run(args.source, Path(directory))
