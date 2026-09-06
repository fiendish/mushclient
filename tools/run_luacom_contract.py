"""Run the pinned upstream connection tests against the generated host source."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

import luacom_vendor as vendor


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    pin = vendor.read_pin(vendor.ROOT)
    with vendor.source_repository(pin['revision'], args.source) as source:
        with tempfile.TemporaryDirectory(prefix='luacom-contract-') as temporary:
            directory = Path(temporary)
            for name in ('run.py', 'shim.cpp', 'tests.cpp'):
                data = vendor.git(source, 'show', pin['revision'] + ':src/test/connection_lifetime/' + name)
                (directory / name).write_bytes(data)
            subprocess.run([sys.executable, str(directory / 'run.py'), '--source-dir',
                            str(vendor.ROOT / 'luacom')], check=True)


if __name__ == '__main__':
    main()
