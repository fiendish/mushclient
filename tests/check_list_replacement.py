"""Check actual list insertion and replacement code with portable UI substitutes.

Run with Python 3 and clang++. --revision reads a pinned Git revision.
The fixture checks ownership and operation counts, not native UI latency.
"""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--revision')
args = parser.parse_args()
OUT = ROOT / '.test-output' / ('list-' + (args.revision or 'current'))
OUT.mkdir(parents=True, exist_ok=True)
path = 'dialogs/world_prefs/genpropertypage.cpp'
source = (subprocess.check_output(['git', '-C', str(ROOT), 'show', args.revision + ':' + path], text=True)
          if args.revision else (ROOT / path).read_text())

def block(text, start):
    begin = text.index(start)
    brace = text.index('{', begin)
    depth = 0
    for end in range(brace, len(text)):
        depth += (text[end] == '{') - (text[end] == '}')
        if depth == 0:
            return text[begin:end + 1]
    raise ValueError(start)

load = block(source, 'void CGenPropertyPage::LoadList')
start = load.index('  CString strObjectName;')
end = load.index('  // sort filtered items', start)
replacement = load[start:end]
# Count comparisons with the same pointer order and search behavior.
replacement = replacement.replace('std::less<CString *> ()', 'CountLess ()')
replacement = replacement.replace('if (find (', 'if (counted_find (')
insert = block(source, 'int CGenPropertyPage::add_list_item')
fixture = Path(__file__).with_name('list_replacement.cpp.in').read_text()
assert fixture.count('@INSERT@') == fixture.count('@REPLACE@') == 1
fixture = fixture.replace('@INSERT@', insert).replace('@REPLACE@', replacement)
cpp = OUT / 'list_replacement.cpp'
cpp.write_text(fixture)
exe = OUT / 'list_replacement'
subprocess.run(['clang++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer', str(cpp), '-o', str(exe)], check=True)
result = subprocess.run([str(exe)], capture_output=True, text=True)
(OUT / 'result.log').write_text(result.stdout + result.stderr)
print(result.stdout + result.stderr, end='')
result.check_returncode()
