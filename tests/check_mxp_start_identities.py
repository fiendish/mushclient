"""Compile MXP source excerpts with portable callback and MFC substitutes.

Run: python3 tests/check_mxp_start_identities.py --output /path/to/results
Requires clang++ with AddressSanitizer. This is not a native MFC build.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess


def between(text, first, last, label):
    start = text.find(first)
    if start < 0:
        raise ValueError(f'{label}: start marker not found: {first!r}')
    stop = text.find(last, start)
    if stop < 0:
        raise ValueError(f'{label}: end marker not found: {last!r}')
    return text[start:stop]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    start = (args.source / 'mxp/mxpStart.cpp').read_text()
    atomic = (args.source / 'mxp/mxpOpenAtomic.cpp').read_text()
    doc = (args.source / 'doc.cpp').read_text()
    header = (args.source / 'doc.h').read_text()
    declaration = between(header, '  bool MXP_OpenAtomicTag', '  void MXP_CloseAtomicTag', 'doc.h')
    assert '__int64 & iResultStyleCreationNumber' in declaration
    assert start.count('transaction.ResolveStyle (iAtomicResultStyleCreationNumber)') == 2
    assert start.count('                            iAtomicResultStyleCreationNumber,') == 2
    assert 'CStyle * & pResultStyle' not in atomic
    packet_names = ['VERSION', 'AFK', 'SUPPORT', 'OPTION', 'USER', 'PASSWORD']
    packet_cases = []
    for name in packet_names:
        case = between(atomic, '    case MXP_ACTION_' + name + ':',
                       '          break;', 'mxp/mxpOpenAtomic.cpp: ' + name)
        # Keep the exact packet call and its deferred diagnostic. Packet building
        # and option/credential gates are outside this lifetime reproduction.
        match = re.search(r'            SendPacket \((\w+),.*?\);[^\n]*\n(.*?);', case, re.S)
        assert match, name
        call = match.group(0)
        variable = match.group(1)
        packet_cases.append('case MXP_ACTION_' + name + ': {\nCString ' + variable + ' = "response-' + name + '\\r\\n";\n' + call + '\nbreak; }')
    pieces = {
        'TRANSACTION': between(start, 'class CMXPStartTransaction', 'class CActionReferenceGuard', 'mxp/mxpStart.cpp'),
        'FINALIZATION': between(start, '  // atomic element?  (looked-up earlier)', '  } // end of CMUSHclientDoc::MXP_StartTag', 'mxp/mxpStart.cpp'),
        'SEND_PACKET': between(doc, 'void  CMUSHclientDoc::SendPacket (const char *', 'void  CMUSHclientDoc::SendPacket (const unsigned char *', 'doc.cpp'),
        'REMEMBER_STYLE': between(doc, 'void CMUSHclientDoc::RememberStyle (', 'void CMUSHclientDoc::OnDebugWorldInput', 'doc.cpp'),
        'PREPARE_STYLE': between(doc, 'void COutputAppendTransaction::TrackLine (', 'void COutputAppendTransaction::RecordCreatedLine ()', 'doc.cpp'),
        'RESULT_INIT': between(atomic, 'unsigned short iFlags', '// find current foreground', 'mxp/mxpOpenAtomic.cpp'),
        'PACKET_CASES': '\n'.join(packet_cases),
        'BR_CASE': between(atomic, '    case MXP_ACTION_BR:', '          // reset', 'mxp/mxpOpenAtomic.cpp'),
        'HR_CASE': between(atomic, '    case MXP_ACTION_HR:', '    case MXP_ACTION_PRE:', 'mxp/mxpOpenAtomic.cpp'),
        'LI_CASE': between(atomic, '     case MXP_ACTION_LI:', '    // pueblo tags', 'mxp/mxpOpenAtomic.cpp'),
        'IMAGE_RESULT': between(atomic, '            CStyle * pResultStyle = AddStyle', '            strAction = strOldAction;', 'mxp/mxpOpenAtomic.cpp'),
    }
    template = Path(__file__).with_name('mxp_start_identity_test.cpp.in').read_text()
    for key, value in pieces.items():
        token = '@' + key + '@'
        assert template.count(token) == 1, token
        template = template.replace(token, value)
    names = sorted(set(re.findall(r'\b(?:MXP_ACTION_\w+|infoMXP_\w+|wrnMXP_\w+)\b', template)))
    template = template.replace('@ENUMS@', 'enum { ' + ', '.join(names) + ' };')
    cpp = args.output / 'mxp_start_identity_test.cpp'
    cpp.write_text(template)
    executable = args.output / 'mxp_start_identity_test'
    command = ['clang++', '-std=c++17', '-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(cpp), '-o', str(executable)]
    subprocess.run(command, check=True)
    env = dict(os.environ)
    # Keep diagnostics visible. A failed compile or check stops this runner.
    subprocess.run([str(executable)], check=True, env=env)


if __name__ == '__main__':
    main()
