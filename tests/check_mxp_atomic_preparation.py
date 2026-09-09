"""Check atomic MXP preparation with production source excerpts.

Requires clang++. Portable substitutes do not validate native MFC callbacks.
"""
import argparse
from pathlib import Path
import re
import subprocess


from output_callbacks import section as between


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline-ref')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    args.output.mkdir(parents=True, exist_ok=True)

    def source(path):
        if args.baseline_ref:
            return subprocess.check_output(
                ['git', 'show', args.baseline_ref + ':' + path], cwd=root, text=True)
        return (root / path).read_text()

    start = source('mxp/mxpStart.cpp')
    # The preparation excerpt ends before the custom-element-only branch.
    # The added brace closes the enclosing callback-boundary block.
    preparation = between(start, '  const __int64 iOpeningMXPGeneration =',
                          '    if (bRanOpenCallback && !pAtomicElement)') + '    }\n'
    pieces = {
        'HELPERS': between(start, 'static void SnapshotActiveTags',
                           'static bool BuildCustomAtomicArguments'),
        'ACTION_GUARD': between(start, 'class CActionReferenceGuard',
                                '// here for start tag'),
        'OPENING': between(start, 'CStyle * pStyle = m_pCurrentLine',
                           '  if (App.m_ElementMap.Lookup'),
        'PREPARATION': preparation,
        'ADD_STYLE': between(source('doc.cpp'), '// adds a new style to the current line',
                             'void CMUSHclientDoc::RefreshMXPMissingTagAnchors'),
        'GET_ENTITY': between(source('mxp/mxpEntities.cpp'),
                              'CString CMUSHclientDoc::MXP_GetEntity',
                              '// end of CMUSHclientDoc::MXP_GetEntity',
                              'mxp/mxpEntities.cpp') + '\n',
    }
    template = Path(__file__).with_suffix('.cpp.in').read_text()
    for name, code in pieces.items():
        token = '@' + name + '@'
        assert template.count(token) == 1, token
        template = template.replace(token, code)
    names = sorted(set(re.findall(r'\berrMXP_\w+\b', template)))
    template = template.replace('@ENUMS@', 'enum { ' + ', '.join(names) + ' };')
    cpp = args.output / 'mxp_atomic_preparation.cpp'
    cpp.write_text(template)
    executable = cpp.with_suffix('')
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-O1', '-g', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', str(cpp), '-o', str(executable)], check=True)
    subprocess.run([str(executable), 'baseline' if args.baseline_ref else 'fixed'], check=True)


if __name__ == '__main__':
    main()
