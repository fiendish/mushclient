"""Run the font warning paths with simulated MFC and Windows font calls.

This checks warnings, plugin return codes, and repeated requests. It does not
check native Windows font enumeration or output rendering.
"""
from pathlib import Path
import subprocess
import tempfile

from cpp_blocks import block

ROOT = Path(__file__).resolve().parents[1]


def main():
    doc = (ROOT / 'doc.cpp').read_text()
    miniwindows = (ROOT / 'scripting/methods/methods_miniwindows.cpp').read_text()
    output = (ROOT / 'scripting/methods/methods_output.cpp').read_text()
    header = (ROOT / 'doc.h').read_text()
    definitions = [block(header, 'struct ci_less : binary_function<string, string, bool>') + ';',
                   'typedef set<string, ci_less> ci_set;']
    methods = [block(doc, signature) for signature in (
        'void CMUSHclientDoc::FontWarning (',
        'static int CALLBACK FontFound (',
        'void CMUSHclientDoc::WarnIfFontMissing (',
        'void CMUSHclientDoc::ChangeFont (',
        'void CMUSHclientDoc::ChangeInputFont (',
        'long CMUSHclientDoc::AddSpecialFont (',
    )]
    methods += [block(miniwindows, 'long CMUSHclientDoc::WindowFont('),
                block(output, 'long CMUSHclientDoc::AddFont(')]
    template = Path(__file__).with_suffix('.cpp.in').read_text()
    for marker, content in [('@TYPES@', '\n'.join(definitions)),
                            ('@METHODS@', '\n'.join(methods))]:
        assert template.count(marker) == 1, marker
        template = template.replace(marker, content)
    with tempfile.TemporaryDirectory(prefix='mushclient-font-warnings-') as directory:
        work = Path(directory)
        source = work / 'font_warnings.cpp'
        source.write_text(template)
        binary = work / 'font_warnings'
        subprocess.run(['clang++', '-std=c++14', '-Wall', '-Wextra', '-Werror',
                        '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
