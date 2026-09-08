"""Check buffered TLS recovery with real OpenSSL memory BIOs.

Requires Python 3, clang++, and OpenSSL development libraries. No network socket
is opened. MFC and Winsock event delivery are substitutes. The constructor,
receive recovery, timer dispatch and socket declaration come from production.
Use --openssl-prefix for a library installation without pkg-config.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import tempfile


def run(source, output, flags):
    tests = Path(__file__).resolve().parent
    socket = (source / 'worldsock.cpp').read_text()
    first = socket.index('static bool IsLiveWorldSocket') if 'static bool IsLiveWorldSocket' in socket else socket.index('CWorldSocket::CWorldSocket')
    wrapper = socket[first:socket.index('void CWorldSocket::OnSend', first)]
    frame = (source / 'mainfrm.cpp').read_text()
    first = frame.index('void CMainFrame::OnTimer(')
    timer = frame[first:frame.index('void CMainFrame::OnUpdateStatuslineFreeze', first)]
    (output / 'worldsock_receive.inc').write_text(wrapper)
    (output / 'worldsock_timer.inc').write_text(timer)
    cpp = output / 'worldsock_tls.cpp'
    cpp.write_text((tests / 'worldsock_tls_prefix.cpp').read_text() +
                   (tests / 'worldsock_tls_main.cpp').read_text())
    results = []
    for throws in [1, 0]:
        for nested in [1, 0]:
            binary = output / ('worldsock_tls_' + str(throws) + '_' + str(nested))
            command = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                       '-O1', '-g', '-fsanitize=address,undefined',
                       '-fno-omit-frame-pointer', '-I', str(source), '-I', str(output),
                       '-DTHROW_AFTER_CALLBACK=' + str(throws),
                       '-DNESTED_READ=' + str(nested), str(cpp), '-o', str(binary)] + flags
            subprocess.run(command, check=True)
            result = subprocess.run([str(binary)], text=True, capture_output=True)
            binary.with_suffix('.log').write_text(result.stdout + result.stderr)
            print(result.stdout + result.stderr, end='')
            result.check_returncode()
            results.append({'throws': throws, 'nested_read': nested,
                            'output': result.stdout, 'status': 'pass'})
    (output / 'result.json').write_text(json.dumps({
        'source': str(source),
        'wrapper_sha256': hashlib.sha256(wrapper.encode()).hexdigest(),
        'timer_sha256': hashlib.sha256(timer.encode()).hexdigest(),
        'checks': results,
        'limitations': 'OpenSSL buffering is real. MFC and Windows event delivery are substitutes.'
    }, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path)
    parser.add_argument('--openssl-prefix', type=Path)
    args = parser.parse_args()
    if args.openssl_prefix:
        flags = ['-I' + str(args.openssl_prefix / 'include'),
                 '-L' + str(args.openssl_prefix / 'lib'), '-lssl', '-lcrypto']
    else:
        flags = shlex.split(subprocess.check_output(
            ['pkg-config', '--cflags', '--libs', 'openssl'], text=True))
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        run(args.source, args.output, flags)
    else:
        with tempfile.TemporaryDirectory(prefix='worldsock-tls-') as temp:
            run(args.source, Path(temp), flags)
