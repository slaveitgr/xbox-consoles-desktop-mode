"""SPDX-License-Identifier: MIT. No compiler or guest program is invoked."""
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from build_pe import (BuildError, LOCK, audit_xcdm_selection, ensure_alias, extract_source, patched_file_hashes,
                      sha256, target_for_library, validate_build_identity,
                      validate_compiler_macros, verify, verify_reused_patch)


class BuildInputTests(unittest.TestCase):
    def test_audit_superset_preserves_exclusions_and_requires_complete_coverage(self):
        targets = {f"dlls/{name}/x86_64-windows/{name}.dll"
                   for name in ("ntdll", "win32u", "kernel32", "kernelbase", "shell32",
                                "bcrypt", "crypt32", "dnsapi", "netapi32", "secur32", "ws2_32")}
        candidates = {"kernel32.dll", "kernelbase.dll", "shell32.dll"}

        class StaticReportFixture:
            def __init__(self, reported):
                self.reported = reported

            def run(self, command, cwd, log_name, allowed_returncodes):
                files = command[command.index("binary") + 1:command.index("--wine-source")]
                self.inputs = {Path(name).name for name in files}
                output = Path(command[command.index("--json") + 1])
                output.write_text(json.dumps({"modules": {name: {"problems": []}
                                                         for name in self.reported}, "build": []}))
                return 0

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for reported in (candidates, candidates - {"shell32.dll"}):
                with self.subTest(reported=reported):
                    runner = StaticReportFixture(reported)
                    summary = audit_xcdm_selection(root / "source", root / "build", root,
                                                   targets, runner)
                    selection = json.loads((root / "guest-selection.json").read_text())
                    self.assertEqual({Path(p).name for p in selection["audit_candidate_pe"]}, candidates)
                    self.assertNotIn("selected_guest_pe", selection)
                    self.assertEqual(set(selection["excluded_providers"]),
                                     set(LOCK["xcdm"]["excluded_guest_providers"]))
                    self.assertEqual(runner.inputs, candidates)
                    self.assertEqual(summary["expected_module_count"], 3)
                    self.assertEqual(summary["reported_module_count"], len(reported))
                    self.assertEqual(summary["tool_gate_passed"], reported == candidates)
                    self.assertTrue(summary["all_gates_blocked"])
                    self.assertFalse(summary["guest_qualified"])
                    self.assertFalse(summary["safe_to_execute"])

    def test_variant_identity_never_reuses_stock_or_another_patch(self):
        stock = {"source_sha256": LOCK["wine"]["archive_sha256"]}
        validate_build_identity(stock, "stock", None)
        patch = LOCK["xcdm"]["patch_sha256"]
        with self.assertRaises(BuildError):
            validate_build_identity(stock, "xcdm", patch)
        xcdm = dict(stock, variant="xcdm", guest_patch_sha256=patch)
        validate_build_identity(xcdm, "xcdm", patch)
        with self.assertRaises(BuildError):
            validate_build_identity(xcdm, "stock", None)
        with self.assertRaises(BuildError):
            validate_build_identity(xcdm, "xcdm", "0" * 64)

    def test_reused_guest_patch_identity_and_files_are_rechecked(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in LOCK["xcdm"]["patched_files"]:
                path = root / name; path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("self-authored patched fixture\n")
            patch = LOCK["xcdm"]["patch_sha256"]
            marker = {"variant": "xcdm", "guest_patch_sha256": patch,
                      "patched_files_sha256": patched_file_hashes(root)}
            verify_reused_patch(root, marker, patch)
            with self.assertRaises(BuildError):
                verify_reused_patch(root, marker, "0" * 64)
            (root / LOCK["xcdm"]["patched_files"][0]).write_text("modified\n")
            with self.assertRaises(BuildError):
                verify_reused_patch(root, marker, patch)

    def test_compiler_proof_requires_effective_guest_and_native_seh_macros(self):
        macros = ("#define __clang_major__ 23\n#define _MSC_VER 1933\n"
                  "#define XCDM_GUEST 1\n#define USE_COMPILER_EXCEPTIONS\n"
                  "#define FOLLOWING_MACRO 99\n")
        proof = validate_compiler_macros(macros, ["-fasync-exceptions"])
        self.assertEqual(proof["USE_COMPILER_EXCEPTIONS"], "")
        for bad in (macros.replace("23", "18"),
                    macros.replace("#define _MSC_VER 1933\n", ""),
                    macros.replace("XCDM_GUEST 1", "XCDM_GUEST 0"),
                    macros.replace("#define USE_COMPILER_EXCEPTIONS\n", "")):
            with self.subTest(macros=bad), self.assertRaises(BuildError):
                validate_compiler_macros(bad, ["-fasync-exceptions"])
        with self.assertRaises(BuildError):
            validate_compiler_macros(macros, [])

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
