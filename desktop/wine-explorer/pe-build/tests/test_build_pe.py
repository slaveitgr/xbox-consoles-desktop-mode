"""SPDX-License-Identifier: MIT. No compiler or guest program is invoked."""
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from build_pe import BuildError, LOCK, ensure_alias, extract_source, sha256, target_for_library, verify


class BuildInputTests(unittest.TestCase):
    def test_hash_mismatch_and_missing_input_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "input"
            with self.assertRaises(BuildError):
                verify(path, "0" * 64)
            path.write_bytes(b"self-authored input")
            with self.assertRaises(BuildError):
                verify(path, "0" * 64)
            verify(path, sha256(path))

    def test_alias_does_not_replace_another_directory_or_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "target"; target.mkdir()
            alias = root / "alias"
            ensure_alias(alias, target)
            ensure_alias(alias, target)
            other = root / "other"; other.mkdir()
            with self.assertRaises(BuildError):
                ensure_alias(alias, other)
            with self.assertRaises(BuildError):
                ensure_alias(other, target)

    def test_module_lookup_confines_names_and_checks_declared_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            shell = root / "dlls/shell32"; shell.mkdir(parents=True)
            (shell / "Makefile.in").write_text("MODULE = shell32.dll\n")
            self.assertEqual(target_for_library(root, "SHELL32.DLL"), "dlls/shell32/x86_64-windows/shell32.dll")
            self.assertIsNone(target_for_library(root, "../shell32.dll"))
            self.assertIsNone(target_for_library(root, "missing.dll"))
            (shell / "Makefile.in").write_text("MODULE = another.dll\n")
            self.assertIsNone(target_for_library(root, "shell32.dll"))

    def archive(self, destination, relative, link=None):
        archive = destination / "source.tar.gz"
        with tarfile.open(archive, "w:gz") as output:
            entry = tarfile.TarInfo("wine-" + LOCK["wine"]["commit"] + "/" + relative)
            if link is None:
                entry.size = 7
                output.addfile(entry, io.BytesIO(b"fixture"))
            else:
                entry.type = tarfile.SYMTYPE; entry.linkname = link
                output.addfile(entry)
        return archive

    def test_archive_paths_and_nonlocal_symlinks_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); work = root / "work"; work.mkdir()
            with self.assertRaises(BuildError):
                extract_source(self.archive(root, "../escape"), work)
            with self.assertRaises(BuildError):
                extract_source(self.archive(root, "include/other", "/etc/passwd"), work)

    def test_regular_files_and_the_pinned_local_symlink_are_extracted(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); work = root / "work"; work.mkdir()
            extract_source(self.archive(root, "include/fixture.h"), work)
            self.assertEqual((work / "include/fixture.h").read_bytes(), b"fixture")
            extract_source(self.archive(root, "tools/gdbinit.py.in", "gdbinit.py"), work)
            self.assertTrue((work / "tools/gdbinit.py.in").is_symlink())
            self.assertEqual((work / "tools/gdbinit.py.in").readlink(), Path("gdbinit.py"))


if __name__ == "__main__":
    unittest.main()
