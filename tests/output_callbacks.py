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


def run(source_root, output, append_only):
    doc = (source_root / 'doc.cpp').read_text()
    trigger = (source_root / 'ProcessPreviousLine.cpp').read_text()
    shim = (Path(__file__).parent / 'output_test_shim.h').read_text()
    header = (source_root / 'doc.h').read_text()
    if not append_only:
        snapshot_type = section(header, '  struct CTriggerLineSnapshot',
                                '  void ProcessOneTriggerSequence')
        shim = shim.replace(' struct CTriggerLineSnapshot {long long iCreationNumber;int iColumn,iLength;};', snapshot_type)
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
    if not append_only:
        body += section(doc, 'bool CMUSHclientDoc::FindStyle (',
                        '// find RGB equivalents for a particular style of text')
        body += section(trigger, 'static vector<CLine *> ResolveTriggerLines (',
                        '// here when a newline is reached')
        # Include the production generation refresh, colour predicate, and full
        # output style-splitting loop. Pane copies and regex dispatch are outside
        # this fixture; repeated match ranges are supplied by the test.
        colour = section(trigger, '      // Pruning can remove old lines',
                         '          // cool new feature in version 4.43')
        body += r'''
void CMUSHclientDoc::Colour(const vector<CTriggerLineSnapshot>& triggerLines,
 const CString& strCurrentLine,int iStartCol,int iEndCol,function<void()> send,
 int colourValue,bool multiline,int styleValue) {
 struct Trigger {int colour,iStyle;bool bMultiLine;COLORREF iOtherForeground=11,
 iOtherBackground=12;int iColourChangeType=TRIGGER_COLOUR_CHANGE_BOTH;};
 Trigger trigger{colourValue,styleValue,multiline};auto trigger_item=&trigger;
 bool bChangedColour=false;
 auto outputLines=ResolveTriggerLines(this,triggerLines);
 auto iOutputGeneration=m_iOutputGeneration;
 send();
''' + colour + '\n break;\n }\n }\n }\n'
    main = (Path(__file__).parent / 'output_callbacks_main.cpp').read_text()
    defines = '#define APPEND_ONLY\n' if append_only else ''
    cpp = output / 'output_callbacks.cpp'
    cpp.write_text(defines + shim + body + main)
    binary = output / 'output_callbacks'
    subprocess.run(['clang++', '-std=c++17', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-g', '-O1', str(cpp),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path)
    parser.add_argument('--append-only', action='store_true')
    args = parser.parse_args()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        run(args.source, args.output, args.append_only)
    else:
        with tempfile.TemporaryDirectory(prefix='mushclient-output-') as directory:
            run(args.source, Path(directory), args.append_only)
