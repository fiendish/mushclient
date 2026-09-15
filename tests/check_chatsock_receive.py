"""Check the production chat receive wrapper with MFC and Winsock substitutes.

Run with Python 3 and clang++. Use --source to compare an earlier chatsock.cpp.
This checks exception recovery and ownership, not native Windows event delivery.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root / 'chatsock.cpp')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    source = args.source.read_text()
    first = source.index('void CChatSocket::OnReceive(')
    last = source.index('void CChatSocket::ReceiveOneNotification(', first)
    wrapper = source[first:last]
    with tempfile.TemporaryDirectory(prefix='chat-receive-') as temporary:
        output = args.output or Path(temporary)
        output.mkdir(parents=True, exist_ok=True)
        (output / 'chatsock_receive.inc').write_text(wrapper)
        binary = output / 'chatsock_receive'
        subprocess.run([
            'clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-g', '-O1', '-fsanitize=address,undefined',
            '-fno-omit-frame-pointer', '-I', str(output),
            str(root / 'tests/chatsock_receive.cpp'), '-o', str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
