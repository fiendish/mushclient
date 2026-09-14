#!/usr/bin/env python3
"""Check property-add reentry with exact production functions.

Extract OnAddItem, creation and sort dispatch, alias/trigger index builders,
and each page's factory and script accessors. MFC containers, UI controls,
dialog field loading, and synchronous script lookup are substitutes.
This check does not run native Windows dialogs, Lua, or scripting Add APIs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


def block(text, marker):
    start = text.index(marker)
    brace = text.index('{', start)
    depth = 0
    for end in range(brace, len(text)):
        depth += (text[end] == '{') - (text[end] == '}')
        if depth == 0:
            return text[start:end + 1]
    raise ValueError(f'Unterminated source block: {marker}')


def compiler_command():
    override = os.environ.get('CXX')
    if override:
        command = shlex.split(override)
        if not command:
            raise ValueError('CXX must name a C++ compiler')
        executable = shutil.which(command[0])
        if executable is None:
            raise ValueError(f'CXX compiler not found or not executable: {command[0]}')
        return [executable, *command[1:]]
    executable = shutil.which('clang++') or shutil.which('c++')
    if executable is None:
        raise ValueError('No C++ compiler found. Set CXX or install clang++ or c++.')
    return [executable]


def run(source, output, revision):
    def read(path):
        if revision:
            return subprocess.check_output(
                ['git', '-C', str(source), 'show', f'{revision}:{path}'], text=True)
        return (source / path).read_text()

    editor = read('dialogs/world_prefs/genpropertypage.cpp')
    prefs = read('dialogs/world_prefs/prefspropertypages.cpp')
    doc = read('doc.cpp')
    definitions = []
    for marker in ('static void SetPropertyCreationNumber (',
                   'static void SortPropertyObjects (',
                   'void CGenPropertyPage::OnAddItem('):
        definitions.append(block(editor, marker))

    for kind, plural in (('Alias', 'Aliases'), ('Trigger', 'Triggers')):
        marker = 'static int CompareAlias (' if kind == 'Alias' else 'int CompareTrigger ('
        definitions.append(block(doc, marker))
        definitions.append(block(doc, f'void CMUSHclientDoc::Build{kind}Indexes ('))
        definitions.append(block(doc, f'void  CMUSHclientDoc::Sort{plural} ('))

    declarations = []
    for page, kind in (('CPrefsP7', 'Alias'), ('CPrefsP8', 'Trigger'), ('CPrefsP16', 'Timer')):
        entry = block(prefs, f'void {page}::OnAdd{kind}()')
        if entry.count('OnAddItem (dlg);') != 1:
            raise ValueError(f'{page}::OnAdd{kind} must call the shared OnAddItem')
        declarations.append(f'''
struct {page} : CGenPropertyPage {{
  explicit {page}(CMUSHclientDoc* doc) : CGenPropertyPage(doc, {kind}) {{}}
  CObject* MakeNewObject() override;
  void SetDispatchID(CObject*, const DISPID) override;
  void SetInternalName(CObject*, const CString) override;
  CString GetScriptName(CObject*) const override;
  CString GetLabel(CObject*) const override;
}};
''')
        for result, method in (('CObject *', 'MakeNewObject'), ('void', 'SetDispatchID'),
                               ('void', 'SetInternalName'), ('CString', 'GetScriptName'),
                               ('CString', 'GetLabel')):
            definitions.append(block(prefs, f'{result} {page}::{method} ('))

    fixture = Path(__file__).with_name('property_add_reentry.cpp.in').read_text()
    marker = '// INSERT EXACT PRODUCTION FUNCTIONS HERE'
    if fixture.count(marker) != 1:
        raise ValueError('Expected one production insertion marker')
    program = fixture.replace(marker, '\n'.join(declarations + definitions))
    cpp = output / 'property_add_reentry.cpp'
    binary = output / 'property_add_reentry'
    cpp.write_text(program)
    command = [*compiler_command(), '-std=c++17', '-Wall', '-Wextra',
               '-Wno-unused-parameter', '-O0', '-g', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
               str(cpp), '-o', str(binary)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    (output / 'property_add_reentry.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='', flush=True)
    result.check_returncode()
    expected = 3 * 2 * 7
    cases = sum(line.endswith(' passed') for line in result.stdout.splitlines())
    if cases != expected:
        raise ValueError(f'Expected {expected} cases; got {cases}')
    report = {
        'result': 'pass', 'cases': cases, 'command': command,
        'source': str(source.resolve()), 'revision': revision or 'working tree',
        'on_add_item_sha256': hashlib.sha256(definitions[2].encode()).hexdigest(),
        'limits': __doc__,
    }
    (output / 'property_add_reentry.json').write_text(json.dumps(report, indent=2) + '\n')


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
        directory = Path(tempfile.mkdtemp(prefix='mushclient-property-add-'))
        try:
            run(args.source, directory, args.revision)
        except BaseException:
            print(f'Failure artifacts retained in {directory}', file=sys.stderr)
            raise
        else:
            shutil.rmtree(directory)
