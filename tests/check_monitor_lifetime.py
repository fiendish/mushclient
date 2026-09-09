"""Compile and run extracted monitor production code with Win32/MFC stubs.

Uses real C++ threads and ASan/UBSan. Does not validate native Windows or Wine.
"""
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
    raise ValueError(signature)

text = (ROOT / 'TextDocument.cpp').read_text()
start = text.index('// ------------------- file change monitoring thread')
end = text.index('// ------------------- handle change to file', start)
production = text[start:end]
frame = (ROOT / 'mainfrm.cpp').read_text()
handlers = '\n'.join(block(frame, name) for name in [
    'LRESULT CMainFrame::OnScriptFileContentsChanged',
    'LRESULT CMainFrame::OnTextFileContentsChanged'])
notification = block((ROOT / 'stdafx.h').read_text(), 'struct CFileChangeNotification') + ';'
template = Path(__file__).with_name('monitor_lifetime.cpp.in').read_text()
for name, value in [('PRODUCTION', production), ('HANDLERS', handlers), ('NOTIFICATION', notification)]:
    assert template.count('@' + name + '@') == 1
    template = template.replace('@' + name + '@', value)
# Verify the wiring that the extracted functions do not compile.
assert 'CollectMonitoringThreads ();' in block((ROOT / 'MUSHclient.cpp').read_text(), 'BOOL CMUSHclientApp::OnIdle')
assert 'SetTimer(TICK_TIMER_ID, 40, NULL )' in frame
for path, pending in [('TextDocument.cpp', 'm_bFileChangedPending'), ('scripting/scripting.cpp', 'm_bScriptFileChangedPending')]:
    source = (ROOT / path).read_text()
    assert 'm_eventFileChanged' not in source and 'm_eventScriptFileChanged' not in source
    assert 'StopMonitoringThread (m_iMonitorToken);\n  ' + pending + ' = false;' in source
out = Path(tempfile.mkdtemp(prefix='mushclient-monitor-'))
cpp = out / 'monitor_lifetime.cpp'
cpp.write_text(template)
for mode, flags in [('release', []), ('debug', ['-D_DEBUG'])]:
    exe = out / mode
    subprocess.run(['clang++', '-std=c++17', '-pthread', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    *flags, str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
print(f'Artifacts: {out}')

# One targeted mutation proves that stale-generation rejection is exercised.
mutated = template.replace('iMonitorToken != 0 && pDoc->m_iMonitorToken == iMonitorToken', 'true')
assert mutated != template
mutant = out / 'stale_token_mutation.cpp'
mutant.write_text(mutated)
exe = out / 'stale_token_mutation'
subprocess.run(['clang++', '-std=c++17', '-pthread', '-O1', '-g',
                '-fsanitize=address,undefined', str(mutant), '-o', str(exe)], check=True)
result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
(out / 'mutation.log').write_text(result.stdout + result.stderr)
assert result.returncode != 0, 'Stale-token mutation escaped detection'
assert '!text.m_bFileChangedPending' in result.stderr, result.stderr
print('PASS: removing the stale-token guard fails the expected assertion')
