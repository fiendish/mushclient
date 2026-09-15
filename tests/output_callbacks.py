#!/usr/bin/env python3
"""Compile production output functions with focused MFC substitutes.

Run with Python 3 and clang++ on macOS or Linux. This check does not build MFC.
The C++ assertions check output, styles, callback delivery, and failure returns.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def section(source, start, end, context='section'):
    begin = source.find(start)
    if begin < 0:
        raise ValueError(f'{context}: start anchor not found: {start!r}')
    finish = source.find(end, begin)
    if finish < 0:
        raise ValueError(f'{context}: end anchor not found: {end!r}')
    return source[begin:finish]


def replace_once(source, expected, replacement, context):
    count = source.count(expected)
    if count != 1:
        raise ValueError(f'{context}: expected one occurrence of {expected!r}, found {count}')
    return source.replace(expected, replacement)


def line_buffer_header(source_root, revision=None, *, memory_exception_defined=False):
    path = 'output_line_buffer.h'
    if revision:
        names = subprocess.check_output(
            ['git', '-C', str(source_root), 'ls-tree', '--name-only', revision, '--', path],
            text=True).splitlines()
        if not names:
            return ''
        contents = subprocess.check_output(
            ['git', '-C', str(source_root), 'show', revision + ':' + path], text=True)
    else:
        contents = (source_root / path).read_text()
    if not memory_exception_defined:
        contents = 'void AfxThrowMemoryException(){throw new CMemoryException;}\n' + contents
    return contents + '\n'


def run(source_root, output, append_only):
    doc = (source_root / 'doc.cpp').read_text()
    trigger = (source_root / 'ProcessPreviousLine.cpp').read_text()
    shim_path = Path(__file__).parent / 'output_test_shim.h'
    shim = shim_path.read_text()
    header = (source_root / 'doc.h').read_text()
    if not append_only:
        snapshot_type = section(header, '  struct CTriggerLineSnapshot',
                                '  void ProcessOneTriggerSequence', 'doc.h')
        shim = replace_once(shim, ' struct CTriggerLineSnapshot {long long iCreationNumber;int iColumn,iLength;};',
                            snapshot_type, shim_path)
    shim = replace_once(shim, 'template<class T> struct List',
                        'size_t listReadCount=0;\ntemplate<class T> struct List', shim_path)
    shim = replace_once(shim, 'T GetNext(POSITION& p)const{',
                        'T GetNext(POSITION& p)const{++listReadCount;', shim_path)
    shim = replace_once(shim, 'T GetPrev(POSITION& p){',
                        'T GetPrev(POSITION& p){++listReadCount;', shim_path)
    shim = replace_once(shim, 'POSITION m_pLinePositions[11]={};',
                        'vector<POSITION> positionStorage=vector<POSITION>(200000 / JUMP_SIZE + 1);'
                        'POSITION* m_pLinePositions=positionStorage.data();', shim_path)
    body = '\n'.join([
        section(doc, 'bool CMUSHclientDoc::StartNewLine_KeepPreviousStyle',
                'COutputAppendTransaction::COutputAppendTransaction', 'doc.cpp'),
        section(doc, 'bool CMUSHclientDoc::AddToLine (',
                '// called when starting a new line to get colours right', 'doc.cpp'),
        section(doc, 'bool CMUSHclientDoc::StartNewLine (',
                'const bool CMUSHclientDoc::CheckScriptingAvailable', 'doc.cpp'),
        section(doc, ' void CMUSHclientDoc::RemoveChunk (void)',
                'void CMUSHclientDoc::ShowStatusLine', 'doc.cpp'),
    ])
    if not append_only:
        body += section(doc, 'bool CMUSHclientDoc::FindStyle (',
                        '// find RGB equivalents for a particular style of text', 'doc.cpp')
        body += section(trigger, 'static vector<CLine *> ResolveTriggerLines (',
                        '// here when a newline is reached', 'ProcessPreviousLine.cpp')
        # Include the production generation refresh, colour predicate, and full
        # output style-splitting loop. Pane copies and regex dispatch are outside
        # this fixture; repeated match ranges are supplied by the test.
        colour = section(trigger, '      // Pruning can remove old lines',
                         '          // cool new feature in version 4.43', 'ProcessPreviousLine.cpp')
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
    if not append_only:
        body += section(trigger, 'static inline unsigned short get_foreground',
                        '// Resolve the original paragraph', 'ProcessPreviousLine.cpp')
        # Compile the actual matching block, including its local POSITION scope.
        matching = section(trigger, '      if (trigger_item->iMatch && !trigger_item->bMultiLine)',
                           '    // copy the wildcard contents to the clipboard', 'ProcessPreviousLine.cpp')
        body += r'''
bool CMUSHclientDoc::Match(const vector<CTriggerLineSnapshot>& triggerLines,
 const CString& strCurrentLine,int iStartCol,int condition,function<void()> send) {
 struct Trigger {int iMatch;bool bMultiLine=false;};
 Trigger item{condition};auto trigger_item=&item;
 auto outputLines=ResolveTriggerLines(this,triggerLines);
 auto iOutputGeneration=m_iOutputGeneration;
 send();
''' + section(trigger, '      if (iOutputGeneration != m_iOutputGeneration)',
                '      if (trigger_item->iMatch && !trigger_item->bMultiLine)', 'ProcessPreviousLine.cpp') + \
            '\nfor(int fixture=0;fixture<1;++fixture) {\n' + matching + \
            '\nreturn true;\n}\nreturn false;\n}\n'
        # Keep the full logging and omission paths. View invalidation is outside
        # this fixture. Deferred note replay is observed through its staged text.
        body += r'''
void CMUSHclientDoc::Finalize(const vector<CTriggerLineSnapshot>& triggerLines,
 long long iParagraphGeneration,POSITION prevpos,bool omit,bool bNoLog,
 const CString& strCurrentLine,long iParagraphReceivedNumber) {
 POSITION pos;
 m_bLineOmittedFromOutput=omit;
''' + section(trigger, '  // Keep the original identity boundary',
                '// if we have changed the colour of this trigger', 'ProcessPreviousLine.cpp') + \
            section(trigger, '  // logging wanted?',
                    '  // display any stuff sent to output window', 'ProcessPreviousLine.cpp') + '\n}\n'
    main = (Path(__file__).parent / 'output_callbacks_main.cpp').read_text()
    if not append_only:
        main += (Path(__file__).parent / 'output_callbacks_followup.cpp').read_text()
    defines = '#define APPEND_ONLY\n' if append_only else ''
    cpp = output / 'output_callbacks.cpp'
    cpp.write_text(defines + shim + line_buffer_header(source_root) + body + main)
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
