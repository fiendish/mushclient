"""Check property edits when script lookup changes the live item.

The complete ChangeOneItem method and EnableAlias API are extracted from
production. MFC controls and script lookup use substitutes. Run with Python 3
and a C++ compiler. These checks do not exercise a native Windows dialog.
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
out = args.output or Path(tempfile.mkdtemp(prefix='mushclient-property-edit-'))
out.mkdir(parents=True, exist_ok=True)


def block(path, marker):
    text = (ROOT / path).read_text()
    start = text.index(marker)
    brace = text.index('{', start)
    depth = 0
    for end in range(brace, len(text)):
        depth += (text[end] == '{') - (text[end] == '}')
        if not depth:
            return text[start:end + 1]
    raise ValueError(f'{path}: unterminated block {marker!r}')


editor = 'dialogs/world_prefs/genpropertypage.cpp'
parts = [block('scripting/methods/methods_aliases.cpp',
               'long CMUSHclientDoc::EnableAlias('),
         block('doc.cpp', 'void CMUSHclientDoc::RetireAlias (')]
parts += [block(editor, marker) for marker in (
    'static void CopyPropertyRuntimeState (',
    'static __int64 GetPropertyCreationNumber (',
    'static void SetPropertyCreationNumber (',
    'static void RetirePropertyObject (',
    'bool CGenPropertyPage::ChangeOneItem (')]
fixture = Path(__file__).with_name('property_edit_reentry.cpp.in').read_text()
if fixture.count('@EXACT@') != 1:
    raise ValueError('Expected one @EXACT@ fixture marker')
cpp = out / 'property_edit_reentry.cpp'
cpp.write_text(fixture.replace('@EXACT@', '\n'.join(parts)))
command = shlex.split(os.environ.get('CXX', '')) if 'CXX' in os.environ else [
    shutil.which('clang++') or shutil.which('c++')]
if not command or not command[0] or not shutil.which(command[0]):
    raise SystemExit('No C++ compiler found. Set CXX or install clang++ or c++.')
exe = out / 'property_edit_reentry'
subprocess.run([*command, '-std=c++17', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-fno-omit-frame-pointer', str(cpp), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
