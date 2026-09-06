"""Test exact vendoring, explicit patches, and failed-update preservation."""
import difflib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import luacom_vendor as vendor


class VendorTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="luacom-vendor-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.upstream = self.root / "upstream"
        self.host = self.root / "host"
        for directory in (self.upstream, self.host):
            directory.mkdir()
            self.git(directory, "init", "--quiet")
            self.git(directory, "config", "user.name", "Vendor test")
            self.git(directory, "config", "user.email", "vendor@example.invalid")
            self.git(directory, "config", "core.autocrlf", "false")
        for name, content in {
            "src/library/example.cpp": b"first\nshared\nlast\n",
            "include/luacom.h": b"header\r\n",
            "COPYRIGHT": b"license\n",
        }.items():
            path = self.upstream / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        (self.upstream / "COPYRIGHT").chmod(0o755)
        self.commit = self.commit_upstream()
        self.directory = self.host / vendor.PIN.parent
        (self.directory / "patches").mkdir(parents=True)
        self.patch(b"first\nshared\nlast\n", b"first\nhost\nlast\n")
        self.pin = {"version": 1, "repository": vendor.REPOSITORY, "revision": self.commit,
                    "patches": ["patches/host.patch"]}
        self.save_pin()
        vendor.run(self.host, "apply", self.upstream)

    def git(self, directory, *arguments):
        return vendor.git(directory, *arguments)

    def commit_upstream(self):
        self.git(self.upstream, "add", ".")
        self.git(self.upstream, "commit", "--quiet", "-m", "Update test source")
        return self.git(self.upstream, "rev-parse", "HEAD").decode().strip()

    def save_pin(self):
        (self.host / vendor.PIN).write_text(json.dumps(self.pin))

    def patch(self, before, after):
        text = "".join(difflib.unified_diff(before.decode().splitlines(True), after.decode().splitlines(True),
                                          fromfile="a/example.cpp", tofile="b/example.cpp"))
        (self.directory / "patches/host.patch").write_bytes(text.encode())

    def check(self, index=False):
        vendor.run(self.host, "check", self.upstream, index=index)

    def snapshot(self):
        return {p.relative_to(self.host).as_posix(): p.read_bytes() for p in (self.host / "luacom").iterdir()}

    def test_exact_bytes_modes_and_repeat_import(self):
        self.assertEqual((self.host / "luacom/luacom.h").read_bytes(), b"header\r\n")
        self.assertEqual((self.host / "luacom/example.cpp").read_bytes(), b"first\nhost\nlast\n")
        self.assertTrue((self.host / "luacom/COPYRIGHT").stat().st_mode & 0o111)
        before = self.snapshot()
        vendor.run(self.host, "apply", self.upstream)
        self.assertEqual(self.snapshot(), before)
        self.git(self.host, "add", "luacom")
        self.check(index=True)

    def test_changed_bytes_fail(self):
        (self.host / "luacom/example.cpp").write_bytes(b"manual change\n")
        with self.assertRaisesRegex(ValueError, "bytes differ"):
            self.check()

    def test_line_ending_changes_fail(self):
        (self.host / "luacom/luacom.h").write_bytes(b"header\n")
        with self.assertRaisesRegex(ValueError, "bytes differ"):
            self.check()

    def test_missing_and_extra_files_fail(self):
        path = self.host / "luacom/luacom.h"
        path.rename(path.with_name("extra.h"))
        with self.assertRaisesRegex(ValueError, "inventory differs"):
            self.check()

    def test_staged_drift_fails(self):
        path = self.host / "luacom/example.cpp"
        expected = path.read_bytes()
        path.write_bytes(b"staged edit\n")
        self.git(self.host, "add", "luacom")
        path.write_bytes(expected)
        with self.assertRaisesRegex(ValueError, "Staged LuaCOM blobs"):
            self.check(index=True)

    def test_undeclared_patch_fails(self):
        (self.directory / "patches/unlisted.patch").write_bytes(b"patch")
        with self.assertRaisesRegex(ValueError, "Every LuaCOM patch"):
            self.check()

    def test_changed_patch_requires_regeneration(self):
        self.patch(b"first\nshared\nlast\n", b"first\nchanged host\nlast\n")
        with self.assertRaisesRegex(ValueError, "bytes differ"):
            self.check()
        vendor.run(self.host, "apply", self.upstream)
        self.check()

    def test_bad_revision_and_repository_fail(self):
        for field, value in (("revision", "master"), ("repository", "https://example.invalid/repository")):
            with self.subTest(field=field):
                original = self.pin[field]
                self.pin[field] = value
                self.save_pin()
                with self.assertRaises(ValueError):
                    self.check()
                self.pin[field] = original
        self.save_pin()

    def test_unavailable_revision_fails(self):
        self.pin["revision"] = "0" * 40
        self.save_pin()
        with self.assertRaises(subprocess.CalledProcessError):
            self.check()

    def test_public_header_is_required(self):
        (self.upstream / "include/luacom.h").unlink()
        self.pin["revision"] = self.commit_upstream()
        self.save_pin()
        with self.assertRaisesRegex(ValueError, "library, public header, and license"):
            self.check()

    def test_duplicate_destinations_fail(self):
        (self.upstream / "src/library/luacom.h").write_bytes(b"conflicting header\n")
        self.pin["revision"] = self.commit_upstream()
        self.save_pin()
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            self.check()

    def test_patch_conflict_keeps_old_pin_and_files(self):
        old_files = self.snapshot()
        old_pin = (self.host / vendor.PIN).read_bytes()
        (self.upstream / "src/library/example.cpp").write_bytes(b"changed upstream context\n")
        candidate = self.commit_upstream()
        with self.assertRaises(subprocess.CalledProcessError):
            vendor.run(self.host, "update", self.upstream, candidate)
        self.assertEqual(self.snapshot(), old_files)
        self.assertEqual((self.host / vendor.PIN).read_bytes(), old_pin)

    def test_update_rejects_existing_manual_edits(self):
        (self.upstream / "include/luacom.h").write_bytes(b"new header\n")
        candidate = self.commit_upstream()
        (self.host / "luacom/example.cpp").write_bytes(b"manual edit\n")
        with self.assertRaisesRegex(ValueError, "bytes differ"):
            vendor.run(self.host, "update", self.upstream, candidate)
        self.assertEqual((self.host / "luacom/example.cpp").read_bytes(), b"manual edit\n")

    def test_update_preserves_new_upstream_bytes(self):
        (self.upstream / "include/luacom.h").write_bytes(b"new header\r\n")
        candidate = self.commit_upstream()
        vendor.run(self.host, "update", self.upstream, candidate)
        self.assertEqual((self.host / "luacom/luacom.h").read_bytes(), b"new header\r\n")
        self.assertEqual(vendor.read_pin(self.host)["revision"], candidate)
        self.check()

    def test_install_failures_restore_old_pin_bytes_and_modes(self):
        (self.upstream / "include/luacom.h").write_bytes(b"new header\n")
        candidate = self.commit_upstream()
        before = self.snapshot()
        pin = (self.host / vendor.PIN).read_bytes()
        replace = os.replace
        for failed_name in ("luacom", "manifest.json"):
            with self.subTest(failed_name=failed_name):
                failed = False

                def fail_once(source, destination):
                    nonlocal failed
                    if Path(destination).name == failed_name and not failed:
                        failed = True
                        raise OSError("injected install failure")
                    return replace(source, destination)

                with patch.object(vendor.os, "replace", side_effect=fail_once):
                    with self.assertRaisesRegex(OSError, "injected"):
                        vendor.run(self.host, "update", self.upstream, candidate)
                self.assertTrue(failed)
                self.assertEqual(self.snapshot(), before)
                self.assertEqual((self.host / vendor.PIN).read_bytes(), pin)
                self.check()

    def test_staging_failure_keeps_old_pin_and_files(self):
        (self.upstream / "include/luacom.h").write_bytes(b"new header\n")
        candidate = self.commit_upstream()
        before = self.snapshot()
        pin = (self.host / vendor.PIN).read_bytes()
        write_bytes = Path.write_bytes

        def fail_stage(path, data):
            if path.name == "COPYRIGHT" and ".luacom-import-" in str(path):
                raise OSError("injected staging failure")
            return write_bytes(path, data)

        with patch.object(Path, "write_bytes", fail_stage):
            with self.assertRaisesRegex(OSError, "injected"):
                vendor.run(self.host, "update", self.upstream, candidate)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual((self.host / vendor.PIN).read_bytes(), pin)
        self.check()

    def test_patches_apply_only_in_declared_order(self):
        second = "".join(difflib.unified_diff(
            "first\nhost\nlast\n".splitlines(True), "first\nsecond host\nlast\n".splitlines(True),
            fromfile="a/example.cpp", tofile="b/example.cpp"))
        (self.directory / "patches/second.patch").write_bytes(second.encode())
        self.pin["patches"].append("patches/second.patch")
        self.save_pin()
        vendor.run(self.host, "apply", self.upstream)
        self.assertEqual((self.host / "luacom/example.cpp").read_bytes(), b"first\nsecond host\nlast\n")
        self.check()
        self.pin["patches"].reverse()
        self.save_pin()
        with self.assertRaises(subprocess.CalledProcessError):
            vendor.run(self.host, "apply", self.upstream)

    def test_upstream_inventory_additions_and_removals(self):
        (self.upstream / "src/library/old.h").write_bytes(b"old source\n")
        intermediate = self.commit_upstream()
        vendor.run(self.host, "update", self.upstream, intermediate)
        (self.upstream / "src/library/old.h").unlink()
        (self.upstream / "src/library/new.h").write_bytes(b"new source\r\n")
        candidate = self.commit_upstream()
        vendor.run(self.host, "update", self.upstream, candidate)
        self.assertFalse((self.host / "luacom/old.h").exists())
        self.assertEqual((self.host / "luacom/new.h").read_bytes(), b"new source\r\n")
        self.check()

    def test_symlinks_are_not_followed(self):
        path = self.host / "luacom/example.cpp"
        path.unlink()
        path.symlink_to(self.upstream / "src/library/example.cpp")
        with self.assertRaisesRegex(ValueError, "bytes differ"):
            self.check()
        with self.assertRaisesRegex(ValueError, "non-regular"):
            vendor.run(self.host, "apply", self.upstream)

    def test_non_directory_target_is_not_overwritten(self):
        directory = self.host / "luacom"
        directory.rename(self.host / "saved-luacom")
        directory.write_bytes(b"unmanaged file\n")
        with self.assertRaisesRegex(ValueError, "non-regular"):
            vendor.run(self.host, "apply", self.upstream)
        self.assertEqual(directory.read_bytes(), b"unmanaged file\n")


if __name__ == "__main__":
    unittest.main()
