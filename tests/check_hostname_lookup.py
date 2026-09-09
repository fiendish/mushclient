"""Test extracted DNS source with a stateful asynchronous resolver fixture.

Requires Python 3 and clang++. Runs with AddressSanitizer and UndefinedBehaviorSanitizer.
The fixture substitutes MFC, sockets, and Winsock. It checks pending buffer writes,
cancellation errors, deferred completion routing, and the world-open script call path.
The generation test covers an already-deferred completion with a saved generation.
It does not cover raw queued Win32 messages whose handles are reused before capture,
or a native Windows/Wine resolver.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def block(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 0
    for end in range(brace, len(text)):
        depth += (text[end] == '{') - (text[end] == '}')
        if depth == 0:
            return text[start:end + 1]
    raise ValueError(f'Unclosed source block: {signature}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision', help='Read source from a Git revision.')
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    out = args.output_dir or Path(tempfile.mkdtemp(prefix='mushclient-hostname-'))
    out.mkdir(parents=True, exist_ok=True)

    def read(path):
        if args.revision:
            return subprocess.check_output(
                ['git', '-C', str(ROOT), 'show', f'{args.revision}:{path}'], text=True)
        return (ROOT / path).read_text()

    doc = read('doc.cpp')
    methods = read('scripting/methods/methods_utilities.cpp')
    frame = read('mainfrm.cpp')
    functions = '\n\n'.join(block(doc, sig) for sig in [
        'BOOL CMUSHclientDoc::OpenSession (',
        'BOOL CMUSHclientDoc::ConnectSocket(',
        'bool CMUSHclientDoc::LookupHostName (',
        'void CMUSHclientDoc::HostNameResolved (',
        'void CMUSHclientDoc::OnConnectionConnect()',
    ])
    functions += '\n\n' + block(methods, 'long CMUSHclientDoc::Connect()')
    # Use the actual deferred-dispatch gate. The fixture supplies the document
    # already selected by its identity, as the surrounding frame code does.
    start = frame.index('else if (pDoc->m_hNameLookup == hLookup &&')
    end = frame.index(';', start) + 1
    dispatch = frame[start:end].removeprefix('else ')
    template = Path(__file__).with_name('hostname_lookup.cpp.in').read_text()
    assert template.count('@FUNCTIONS@') == template.count('@DISPATCH@') == 1
    cpp = out / 'hostname_lookup.cpp'
    cpp.write_text(template.replace('@FUNCTIONS@', functions).replace('@DISPATCH@', dispatch))
    exe = out / 'hostname_lookup'
    subprocess.run(['clang++', '-std=c++17', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(cpp), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    (out / 'result.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='')
    print(f'Artifacts: {out}')
    result.check_returncode()


if __name__ == '__main__':
    main()
