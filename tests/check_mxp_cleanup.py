"""Check MXP close, discard, and argument ownership with source excerpts.

Requires clang++. Portable substitutes do not validate native MFC behavior.
"""
import argparse
from pathlib import Path
import subprocess

from check_mxp_start_identities import between


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline-ref')
    parser.add_argument('--check', choices=['all', 'close', 'discard', 'attlist'], default='all')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    args.output.mkdir(parents=True, exist_ok=True)

    def source(path):
        if args.baseline_ref:
            return subprocess.check_output(
                ['git', 'show', args.baseline_ref + ':' + path], cwd=root, text=True)
        return (root / path).read_text()

    close = source('mxp/mxpClose.cpp')
    close_start = 'struct CTagToClose' if '\nstruct CTagToClose' in close else 'void CMUSHclientDoc::MXP_CloseOpenTags'
    close_end = '// end of CMUSHclientDoc::MXP_CloseAllTags'
    discard = source('mxp/mxpOnOff.cpp')
    definitions = source('mxp/mxpDefs.cpp')
    guard_end = '  };' if '\nclass CElementArgumentListGuard' in definitions[:definitions.index('void CMUSHclientDoc::MXP_Definition')] else '  } argumentListGuard (ArgumentList);'
    pieces = {
        'CLOSE': between(close, close_start, close_end, 'mxp/mxpClose.cpp') + close_end + '\n',
        'DISCARD': between(discard, 'static CStyle * FindMXPStyle', 'static void RebaseMXPResetState', 'mxp/mxpOnOff.cpp'),
        'GUARD': between(definitions, 'class CElementArgumentListGuard', guard_end, 'mxp/mxpDefs.cpp') + '  };\n',
        'ATTLIST': between(definitions, 'void CMUSHclientDoc::MXP_Attlist', '// here for <!ENTITY', 'mxp/mxpDefs.cpp'),
    }
    template = Path(__file__).with_suffix('.cpp.in').read_text()
    for name, code in pieces.items():
        marker = '@' + name + '@'
        assert template.count(marker) == 1, marker
        template = template.replace(marker, code)
    cpp = args.output / 'mxp_cleanup.cpp'
    cpp.write_text(template)
    executable = args.output / 'mxp_cleanup'
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(cpp), '-o', str(executable)], check=True)
    result = subprocess.run([str(executable), args.check], text=True, capture_output=True)
    (args.output / 'run.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='')
    result.check_returncode()


if __name__ == '__main__':
    main()
