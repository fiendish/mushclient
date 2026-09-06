"""Import exact LuaCOM Git blobs and apply only the declared host patches."""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from typing import NamedTuple


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = "https://github.com/fiendish/luacom.git"
PIN = Path("third_party/luacom/manifest.json")


class VendorFile(NamedTuple):
    mode: str
    data: bytes


def git(repository, *arguments):
    return subprocess.check_output(["git", "-C", str(repository), *arguments])


def revision(value):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{40}", value):
        raise ValueError("LuaCOM revision must be a full lowercase commit ID")
    return value


def read_pin(root):
    pin = json.loads((root / PIN).read_bytes())
    if set(pin) != {"version", "repository", "revision", "patches"}:
        raise ValueError("Unexpected LuaCOM manifest fields")
    if pin["version"] != 1 or pin["repository"] != REPOSITORY:
        raise ValueError("Unsupported LuaCOM source manifest")
    revision(pin["revision"])
    names = pin["patches"]
    if not isinstance(names, list) or len(set(names)) != len(names):
        raise ValueError("Patches must be an ordered list without duplicates")
    for name in names:
        if not isinstance(name, str) or not re.fullmatch(r"patches/[A-Za-z0-9_.-]+\.patch", name):
            raise ValueError("Invalid LuaCOM patch path")
    directory = root / PIN.parent
    actual = {p.relative_to(directory).as_posix() for p in (directory / "patches").rglob("*.patch")}
    if actual != set(names):
        raise ValueError("Every LuaCOM patch must be declared in the manifest")
    return pin


@contextmanager
def source_repository(commit, source=None):
    revision(commit)
    if source is not None:
        resolved = git(source, "rev-parse", "--verify", commit + "^{commit}").decode().strip()
        if resolved != commit:
            raise ValueError("LuaCOM source revision does not match the pin")
        yield Path(source)
    else:
        with tempfile.TemporaryDirectory(prefix="luacom-source-") as temporary:
            repository = Path(temporary)
            git(repository, "init", "--bare", "--quiet")
            git(repository, "fetch", "--quiet", "--depth=1", "--no-tags", REPOSITORY, commit)
            resolved = git(repository, "rev-parse", "FETCH_HEAD").decode().strip()
            if resolved != commit:
                raise ValueError("Fetched LuaCOM revision does not match the pin")
            yield repository


def upstream_files(repository, commit):
    inventory = {}
    tree = git(repository, "ls-tree", "-r", "-z", commit, "--", "src/library", "include/luacom.h", "COPYRIGHT")
    for entry in tree.split(b"\0"):
        if not entry:
            continue
        metadata, name = entry.split(b"\t", 1)
        mode, kind, oid = metadata.split()
        if mode not in {b"100644", b"100755"} or kind != b"blob":
            raise ValueError("LuaCOM source paths must be regular files")
        inventory[name.decode()] = (mode.decode(), oid.decode())
    if not {"include/luacom.h", "COPYRIGHT"} <= set(inventory) or not any(
            name.startswith("src/library/") for name in inventory):
        raise ValueError("LuaCOM source must include the library, public header, and license")
    # The consumer owns the import layout. LuaCOM needs no export manifest or
    # other knowledge of this host. Include every library blob, without filters.
    files = {source: source.rsplit("/", 1)[-1] for source in inventory}
    if len(set(files.values())) != len(files):
        raise ValueError("Duplicate LuaCOM import destination")
    result = {}
    for source, destination in files.items():
        if not isinstance(destination, str) or not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]*", destination):
            raise ValueError("LuaCOM import destinations must be plain file names")
        mode, oid = inventory[source]
        result[destination] = VendorFile(mode, git(repository, "cat-file", "blob", oid))
    return result


def reconstruct(root, pin, repository):
    files = upstream_files(repository, pin["revision"])
    with tempfile.TemporaryDirectory(prefix="luacom-patched-") as temporary:
        directory = Path(temporary)
        for name, content in files.items():
            (directory / name).write_bytes(content.data)
            (directory / name).chmod(int(content.mode, 8) & 0o777)
        for name in pin["patches"]:
            patch = root / PIN.parent / name
            if patch.is_symlink() or not patch.is_file():
                raise ValueError("LuaCOM patches must be regular files")
            if any(line.startswith((b"old mode ", b"new mode ", b"new file mode ", b"deleted file mode ")) for line in patch.read_bytes().splitlines()):
                raise ValueError("Host patches must preserve upstream file modes and inventory")
            changes = git(directory, "apply", "--numstat", "-z", str(patch)).split(b"\0")
            changed = []
            for change in changes:
                if change:
                    added, removed, path = change.split(b"\t", 2)
                    int(added)
                    int(removed)
                    changed.append(path.decode())
            if not changed or not set(changed) <= set(files):
                raise ValueError("Host patches may change only imported LuaCOM files")
            git(directory, "apply", "--check", "--whitespace=error-all", str(patch))
            git(directory, "apply", "--whitespace=error-all", str(patch))
        if {p.name for p in directory.iterdir()} != set(files):
            raise ValueError("Host patches must preserve the imported file inventory")
        result = {}
        for name in files:
            path = directory / name
            if path.is_symlink() or not path.is_file():
                raise ValueError("Host patches must preserve regular file types")
            result[name] = VendorFile(files[name].mode, path.read_bytes())
        return result


def check_files(root, expected, index=False):
    directory = root / "luacom"
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError("Vendored LuaCOM must be a regular directory")
    actual = {p.relative_to(directory).as_posix() for p in directory.rglob("*")}
    if actual != set(expected):
        raise ValueError("Vendored LuaCOM inventory differs: " + str(sorted(actual ^ set(expected))))
    mismatches = []
    for name, content in expected.items():
        path = directory / name
        if path.is_symlink() or not path.is_file() or path.read_bytes() != content.data or (os.name != "nt" and bool(path.stat().st_mode & 0o111) != (content.mode == "100755")):
            mismatches.append(name)
    if mismatches:
        raise ValueError("Vendored LuaCOM bytes differ from upstream plus patches: " + ", ".join(mismatches))
    if index:
        entries = git(root, "ls-files", "--stage", "-z", "--", "luacom").split(b"\0")
        indexed = {}
        for entry in entries:
            if entry:
                metadata, name = entry.split(b"\t", 1)
                mode, oid, stage = metadata.split()
                if mode not in {b"100644", b"100755"} or stage != b"0":
                    raise ValueError("Invalid LuaCOM Git index mode or merge state")
                indexed[name.decode().removeprefix("luacom/")] = VendorFile(mode.decode(), git(root, "cat-file", "blob", oid.decode()))
        if indexed != expected:
            raise ValueError("Staged LuaCOM blobs differ from upstream plus patches")


def write_files(root, expected, previous, pin_bytes=None):
    directory = root / "luacom"
    if directory.is_symlink() or (directory.exists() and not directory.is_dir()):
        raise ValueError("Refusing to overwrite a non-regular LuaCOM path")
    actual = {p.relative_to(directory).as_posix() for p in directory.rglob("*")}
    if not actual <= set(previous) | set(expected):
        raise ValueError("Refusing to remove unmanaged files from the LuaCOM directory")
    for path in directory.rglob("*"):
        if path.is_symlink() or not path.is_file():
            raise ValueError("Refusing to overwrite a non-regular LuaCOM path")
    # Stage on the same filesystem before moving either live path. Keep the
    # backups if rollback itself fails so an I/O error cannot erase recovery data.
    temporary = Path(tempfile.mkdtemp(prefix=".luacom-import-", dir=root))
    backed_up = installed = pin_installed = False
    retain_backup = False
    try:
        staged = temporary / "new" / "luacom"
        staged.mkdir(parents=True)
        for name, content in expected.items():
            path = staged / name
            path.write_bytes(content.data)
            path.chmod(int(content.mode, 8) & 0o777)
        check_files(staged.parent, expected)
        if pin_bytes is not None:
            (temporary / "new-pin").write_bytes(pin_bytes)
            shutil.copy2(root / PIN, temporary / "old-pin")
        try:
            if directory.exists():
                os.replace(directory, temporary / "old-luacom")
                backed_up = True
            os.replace(staged, directory)
            installed = True
            if pin_bytes is not None:
                os.replace(temporary / "new-pin", root / PIN)
                pin_installed = True
            check_files(root, expected)
        except BaseException:
            try:
                if pin_installed:
                    os.replace(temporary / "old-pin", root / PIN)
                if installed:
                    os.replace(directory, temporary / "failed-luacom")
                if backed_up:
                    os.replace(temporary / "old-luacom", directory)
            except BaseException as rollback_error:
                retain_backup = True
                raise RuntimeError(f"LuaCOM rollback failed; recover backups from {temporary}") from rollback_error
            raise
    finally:
        if not retain_backup:
            shutil.rmtree(temporary)


def run(root, action, source=None, new_revision=None, index=False):
    pin = read_pin(root)
    with source_repository(pin["revision"], source) as repository:
        expected = reconstruct(root, pin, repository)
    if action == "check":
        check_files(root, expected, index=index)
    elif action == "apply":
        write_files(root, expected, expected)
    elif action == "update":
        check_files(root, expected)
        candidate_pin = dict(pin, revision=revision(new_revision))
        with source_repository(candidate_pin["revision"], source) as repository:
            candidate = reconstruct(root, candidate_pin, repository)
        write_files(root, candidate, expected, (json.dumps(candidate_pin, indent=2) + "\n").encode())
        expected = candidate
        pin = candidate_pin
    else:
        raise ValueError("Unknown LuaCOM vendor action")
    print(f"LuaCOM {action} passed: {len(expected)} files at {pin['revision']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["check", "apply", "update"])
    parser.add_argument("--source", type=Path, help="Use objects from an existing LuaCOM Git checkout")
    parser.add_argument("--revision", help="New full commit ID for update")
    parser.add_argument("--index", action="store_true", help="Also check staged Git blob bytes")
    arguments = parser.parse_args()
    if bool(arguments.revision) != (arguments.action == "update"):
        parser.error("--revision is required only for update")
    if arguments.index and arguments.action != "check":
        parser.error("--index is valid only for check")
    run(ROOT, arguments.action, arguments.source, arguments.revision, arguments.index)


if __name__ == "__main__":
    main()
