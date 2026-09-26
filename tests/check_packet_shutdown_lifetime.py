"""Native MFC/ASan checks of packet callbacks and world resource teardown.

Run from an x86 MSVC developer prompt:
  python tests/check_packet_shutdown_lifetime.py
Add --revision HEAD --case packet-buffer (or shutdown-sound) for a negative control.
Actual production methods are extracted; sockets, plugins and resource objects
are deterministic substitutes. This is not an end-to-end UI or audio test.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from cpp_blocks import block
from check_worldsock_receive import native_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision")
    parser.add_argument("--case", action="append", dest="cases")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]

    def source(path, baseline=True):
        if baseline and args.revision:
            return subprocess.check_output(
                ["git", "-C", native_path(repo), "show", f"{args.revision}:{path}"],
                text=True)
        return (repo / path).read_text()

    doc = source("doc.cpp")
    teardown = block(source("doc_construct.cpp"), "CMUSHclientDoc::~CMUSHclientDoc()")
    # Exercise the exact cleanup/callback order through resource release.
    # The remaining trigger/line/database cleanup is outside this fixture.
    teardown = teardown[:teardown.index("// delete triggers")] + "}\n"
    sound = source("scripting/methods/methods_sounds.cpp")
    disconnect = block(doc, "void CMUSHclientDoc::OnConnectionDisconnect()")
    lookup_cleanup = disconnect[disconnect.index("  if (m_hNameLookup)"):
                                disconnect.index("  App.m_bUpdateActivity")]
    display = block(doc, "void CMUSHclientDoc::DisplayMsg(")
    display = display[:display.index("  // at the very start we may not have a current line")]
    display += "  parsed.assign(lpszText, size);\n}\n"
    parts = {
        "GUARD": block(source("doc.h", False), "class CWorldDocumentOperationGuard") + ";",
        "METHODS": "\n\n".join([
            block(doc, "void  CMUSHclientDoc::SendPacket (const char *"),
            block(doc, "void CMUSHclientDoc::Debug_Packets ("),
            block(sound, "long CMUSHclientDoc::StopSound("),
            block(sound, "long CMUSHclientDoc::GetSoundStatus("),
            "void CMUSHclientDoc::ReleaseLookupForDisconnect() {\n" + lookup_cleanup + "}\n",
            display,
            teardown,
        ]),
    }
    template = Path(__file__).with_name("packet_shutdown_lifetime.cpp.in").read_text()
    for name, value in parts.items():
        token = "@" + name + "@"
        assert template.count(token) == 1, token
        template = template.replace(token, value)
    directory = Path(tempfile.mkdtemp(prefix="mushclient-packet-shutdown-"))
    cpp = directory / "test.cpp"
    binary = directory / "test.exe"
    cpp.write_text(template)
    print(f"Test artifacts: {directory}", flush=True)
    subprocess.run([
        "cl", "/nologo", "/EHsc", "/MD", "/D_AFXDLL", "/D_CRT_SECURE_NO_WARNINGS",
        "/Zi", "/Od", "/fsanitize=address", "/std:c++17", str(cpp),
        f"/Fe{binary}", f"/Fo{directory / 'test.obj'}", f"/Fd{directory / 'test.pdb'}",
        "/link", "/INCREMENTAL:NO", "/SUBSYSTEM:CONSOLE", "ws2_32.lib",
    ], check=True)
    cases = args.cases or [
        "packet-normal", "packet-disconnect", "packet-reconnect", "packet-reuse",
        "packet-buffer", "packet-close-world", "packet-close-handle",
        "packet-closing", "packet-nested", "packet-throw", "incoming-buffer",
        "shutdown-sound", "shutdown-replace", "shutdown-font", "shutdown-mapper",
        "shutdown-finalizer", "shutdown-lookup",
    ]
    for case in cases:
        subprocess.run([str(binary), case], check=True)


if __name__ == "__main__":
    main()
