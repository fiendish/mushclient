#!/usr/bin/env python3
"""Check MXP anchor results, list visits, and allocation failure during refresh.

Compile the current production function with portable list substitutes and a
full-history reference. This does not run native MFC callbacks or the UI.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def extract(doc, label):
    first = 'void CMUSHclientDoc::RefreshMXPMissingTagAnchors (void)'
    last = '  } // end of CMUSHclientDoc::RefreshMXPMissingTagAnchors'
    start = doc.find(first)
    if start < 0:
        raise ValueError(f'{label}: missing {first!r}')
    end = doc.find(last, start)
    if end < 0:
        raise ValueError(f'{label}: missing {last!r}')
    return doc[start:end + len(last)]


def run(source, output, baseline_ref):
    doc = (source / 'doc.cpp').read_text()
    body = extract(doc, source / 'doc.cpp')
    if baseline_ref:
        original = subprocess.check_output(
            ['git', '-C', str(source), 'show', baseline_ref + ':doc.cpp'], text=True)
        baseline = extract(original, baseline_ref + ':doc.cpp').replace(
            'CMUSHclientDoc::RefreshMXPMissingTagAnchors',
            'CMUSHclientDoc::BaselineRefresh')
    else:
        baseline = 'void CMUSHclientDoc::BaselineRefresh() { reference(*this); }'
    template = Path(__file__).with_suffix('.cpp.in').read_text()
    if template.count('@REFRESH@') != 1:
        raise ValueError('check_mxp_anchor_refresh.cpp.in: expected one @REFRESH@')
    cpp = output / 'mxp_anchor_refresh.cpp'
    if template.count('@BASELINE@') != 1:
        raise ValueError('check_mxp_anchor_refresh.cpp.in: expected one @BASELINE@')
    cpp.write_text(template.replace('@REFRESH@', body).replace('@BASELINE@', baseline))
    binary = output / 'mxp_anchor_refresh'
    command = shlex.split(os.environ.get('CXX', 'clang++')) + [
        '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-O1',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
        '-fno-omit-frame-pointer', str(cpp), '-o', str(binary)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(binary)], text=True, capture_output=True)
    (output / 'run.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='')
    result.check_returncode()
    rows = [json.loads(line) for line in result.stdout.splitlines()]
    (output / 'result.json').write_text(json.dumps({
        'command': command, 'baseline_ref': baseline_ref, 'results': rows,
        'method': 'Extracted production refresh, full-history reference, counted linked-list reads, failing operator new, ASan and UBSan.',
        'limits': 'Portable list substitutes. No native MFC timing or callback integration claim.'
    }, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path)
    parser.add_argument('--baseline-ref', help='Also compare the exact refresh function at this Git revision')
    args = parser.parse_args()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        run(args.source, args.output, args.baseline_ref)
    else:
        with tempfile.TemporaryDirectory(prefix='mxp-anchor-refresh-') as directory:
            run(args.source, Path(directory), args.baseline_ref)
