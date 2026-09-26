"""Compile the actual receive callback with an MFC/Winsock contract model.

Run with Python 3 and clang++. This checks callback control flow and buffer
ownership. It does not replace a native Windows MFC message-loop test.
Pass --source to check another revision of worldsock.cpp.
"""

import argparse
import pathlib
import subprocess
import tempfile


def native_path(path):
    """Strip Python's extended prefix; cl does not parse extended UNC inputs."""
    text = str(path)
    if text.startswith("\\\\?\\UNC\\"):
        return "\\\\" + text[8:]
    if text.startswith("\\\\?\\"):
        return text[4:]
    return text


def main():
    repo = pathlib.Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=pathlib.Path, default=repo / "worldsock.cpp")
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--native-windows", action="store_true",
                        help="compile with cl from an MSVC developer prompt")
    args = parser.parse_args()
    source = args.source.read_text()
    callback = source[source.index("static bool IsLiveWorldSocket") if "static bool IsLiveWorldSocket" in source else source.index("CWorldSocket::CWorldSocket"):
                      source.index("void CWorldSocket::OnSend")]
    with tempfile.TemporaryDirectory(
            prefix="worldsock-receive-",
            ignore_cleanup_errors=args.native_windows) as work:
        work = args.output or pathlib.Path(work)
        work.mkdir(parents=True, exist_ok=True)
        frame = (repo / "mainfrm.cpp").read_text()
        first = frame.index("void CMainFrame::OnTimer(")
        last = frame.index("void CMainFrame::OnUpdateStatuslineFreeze", first)
        (work / "worldsock_timer.inc").write_text(frame[first:last])
        (work / "worldsock_receive.inc").write_text(callback)
        binary = work / ("worldsock_receive.exe" if args.native_windows else "worldsock_receive")
        if args.native_windows:
            subprocess.run([
                "cl", "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                f"/I{native_path(repo)}", f"/I{native_path(work)}",
                native_path(repo / "tests/worldsock_receive.cpp"),
                f"/Fe{native_path(binary)}",
                f"/Fo{native_path(work / 'worldsock_receive.obj')}",
            ], check=True)
        else:
            subprocess.run([
                "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(repo), "-I", str(work),
                str(repo / "tests/worldsock_receive.cpp"), "-o", str(binary),
            ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
