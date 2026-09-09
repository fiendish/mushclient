#!/usr/bin/env python3
"""Check complete production output transactions and count retained-style visits."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
from output_callbacks import section, replace_once


def run(source, output, revision):
    def read(path):
        if revision:
            return subprocess.check_output(
                ['git', '-C', str(source), 'show', revision + ':' + path], text=True)
        return (source / path).read_text()
    doc = read('doc.cpp')
    header = read('doc.h')
    shim_path = Path(__file__).parent / 'output_test_shim.h'
    shim = shim_path.read_text()

    def patch(expected, replacement):
        nonlocal shim
        shim = replace_once(shim, expected, replacement, shim_path)
    begin = shim.index('struct COutputAppendTransaction {')
    end = shim.index('\nstruct CView', begin)
    shim = shim[:begin] + 'class COutputAppendTransaction;\n' + shim[end:]
    patch('#include <cassert>', '#include <cassert>\n#include <type_traits>')
    patch('template<class T> struct List', '''
struct CLine;struct CStyle;
long long lineReads=0,styleReads=0;
template<class T> void countRead() {
 if constexpr (is_same<T,CLine*>::value)++lineReads;
 if constexpr (is_same<T,CStyle*>::value)++styleReads;
}
template<class T> struct List''')
    patch('T GetNext(POSITION& p)const{', 'T GetNext(POSITION& p)const{countRead<T>();')
    patch('T GetPrev(POSITION& p){', 'T GetPrev(POSITION& p){countRead<T>();')
    patch('struct AppType {', 'struct AppType {long long GetUniqueNumber(){return ++seq;}')
    patch('POSITION m_pLinePositions[11]={};',
                        'POSITION positionStorage[10002]={};POSITION* m_pLinePositions=positionStorage;')
    patch(' long long m_iOutputGeneration=0;',
                        ' int m_iListMode=0,m_iListCount=0;long long m_iMXPListOwner=0;\n long long m_iOutputGeneration=0;')
    declaration = section(header, 'class COutputAppendTransaction\n',
                          '/////////////////////////////////////////////////////////////////////////////\n\nclass CAliasExecutionGuard')
    body = '\n'.join([
        section(doc, 'bool CMUSHclientDoc::StartNewLine_KeepPreviousStyle',
                '// called when starting a new line to get colours right'),
        section(doc, 'bool CMUSHclientDoc::StartNewLine (',
                'const bool CMUSHclientDoc::CheckScriptingAvailable'),
        section(doc, ' void CMUSHclientDoc::RemoveChunk (void)',
                'void CMUSHclientDoc::ShowStatusLine'),
    ])
    main = (Path(__file__).parent / 'output_transactions_main.cpp').read_text()
    cpp = output / 'output_transactions.cpp'
    cpp.write_text(('#define EXPECT_HISTORY_SCAN\n' if revision else '') + shim + declaration + body + main)
    binary = output / 'output_transactions'
    subprocess.run(['clang++', '-std=c++17', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-g', '-O1', str(cpp), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], text=True, capture_output=True)
    (output / 'run.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='')
    result.check_returncode()
    (output / 'result.json').write_text(json.dumps({
        'revision': revision or 'working tree', 'exit_code': result.returncode,
        'output': result.stdout.splitlines(),
        'method': 'Complete extracted transaction and append functions. List substitutes count GetNext/GetPrev calls. No native timing claim.'
    }, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path)
    parser.add_argument('--baseline-ref')
    args = parser.parse_args()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        run(args.source, args.output, args.baseline_ref)
    else:
        with tempfile.TemporaryDirectory(prefix='mushclient-transactions-') as temp:
            run(args.source, Path(temp), args.baseline_ref)
