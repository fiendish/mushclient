"""Check that xgettext extracts both shared MXP close warning formats."""
import argparse
from pathlib import Path
import subprocess


FORMATS = (
    'End-of-line closure of open MXP tag: <%s>',
    '<reset> closure of MXP tag: <%s>',
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--xgettext', default='xgettext')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    catalog = args.output / 'mxp_close.po'
    subprocess.run([args.xgettext, '--language=C++', '--keyword=TFormat',
                    '--output', str(catalog), str(args.source / 'mxp/mxpClose.cpp')], check=True)
    extracted = catalog.read_text()
    for message in FORMATS:
        assert 'msgid "' + message + '"' in extracted, f'{catalog}: missing format: {message!r}'
    print('PASS xgettext extracts both MXP close warning formats')


if __name__ == '__main__':
    main()
