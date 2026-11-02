#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Checked stop-overlay preservation, include binding, and fail-closed tests."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("thread_stop", ROOT / "tools/runtime/prepare_thread_stop.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
SOURCE = ROOT / "external/Boxedwine64-full"


class OverlayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="protondesktop-stop-overlay-tests-",
                                                    dir="/dev/shm" if Path("/dev/shm").is_dir() else None)
        cls.work = Path(cls.temporary.name)
        cls.profile = cls.work / "profile"
        cmake = cls.work / "prepare.cmake"
        cmake.write_text(f'''include("{ROOT.as_posix()}/tools/uwp-core/prepare_profile_overlay.cmake")
proton_prepare_uwp_profile_overlay("{SOURCE.as_posix()}" "{cls.profile.as_posix()}")
include("{ROOT.as_posix()}/tools/runtime/prepare_wine_session.cmake")
proton_prepare_wine_session("{SOURCE.as_posix()}" "{cls.profile.as_posix()}" sources)
''')
        result = subprocess.run(["cmake", "-P", str(cmake)], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError(result.stdout)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_preserves_lifecycle_and_owned_worker_edits(self):
        output = self.work / "preservation"
        before = {name: (self.profile / name).read_bytes() for name in
                  ("source/kernel/kprocess.cpp", "source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp")}
        manifest = module.prepare(SOURCE, self.profile, output)
        for name, data in before.items():
            self.assertEqual((self.profile / name).read_bytes(), data)
        process = (output / "source/kernel/kprocess.cpp").read_text()
        normal = (output / "source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp").read_text()
        self.assertEqual(process.count("protonRollbackUnscheduledThread"),
                         before["source/kernel/kprocess.cpp"].decode().count("protonRollbackUnscheduledThread"))
        self.assertEqual(normal.count("protondesktop::uwp::SpawnGuestWorker"), 1)
        self.assertNotIn("cppThread.detach()", normal)
        self.assertFalse(manifest["data_fields_added"])
        self.assertEqual(len(list(output.rglob("*.cpp"))), len(module.SOURCE_FILES))
        self.assertEqual(len([p for p in output.rglob("*") if p.is_file()]),
                         len(module.SOURCE_FILES) + len(module.HEADER_FILES) + 1)

    def test_quoted_edges_bind_generated_headers(self):
        output = self.work / "includes"
        manifest = module.prepare(SOURCE, self.profile, output)
        umbrella = (output / "include/boxedwine.h").read_text()
        normal = (output / "source/emulation/cpu/normal/normalCPU.cpp").read_text()
        process = (output / "source/kernel/kprocess.cpp").read_text()
        self.assertIn((output / "include/kthread.h").as_posix(), umbrella)
        self.assertIn((output / "include/kprocess.h").as_posix(), umbrella)
        self.assertIn((output / "source/emulation/cpu/normal/normal_other.h").as_posix(), normal)
        self.assertIn((SOURCE / "source/io/fsfilenode.h").as_posix(), process)
        for edges in manifest["quoted_include_edges"].values():
            for edge in edges:
                if edge["resolution"] == "external-conditional":
                    self.assertEqual(edge["include"], "TargetConditionals.h")
                else:
                    self.assertTrue(Path(edge["target"]).is_file())

    def test_anchor_drift_writes_nothing(self):
        header = self.profile / "include/kthread.h"
        before = header.read_bytes()
        output = self.work / "rejected"
        try:
            header.write_text(before.decode().replace("    BOXEDWINE_MUTEX waitingCondSync;", "    // changed fixture anchor"))
            with self.assertRaisesRegex(ValueError, "Pinned stop edit"):
                module.prepare(SOURCE, self.profile, output)
            self.assertFalse(output.exists())
        finally:
            header.write_bytes(before)

    def test_new_unrecognized_flag_access_fails_closed(self):
        relative = "source/kernel/kpoll.cpp"
        changed = self.profile / relative
        original = changed.read_bytes() if changed.exists() else None
        output = self.work / "unrecognized-field"
        try:
            changed.parent.mkdir(parents=True, exist_ok=True)
            changed.write_text((SOURCE / relative).read_text() +
                               "\nstatic bool changed_fixture(KThread* other) { return other->terminating; }\n")
            with self.assertRaisesRegex(ValueError, "Unredirected stop-field"):
                module.prepare(SOURCE, self.profile, output)
            self.assertFalse(output.exists())
        finally:
            if original is None:
                changed.unlink()
            else:
                changed.write_bytes(original)

    def test_duplicate_application_rejected_without_changes(self):
        output = self.work / "duplicate"
        module.prepare(SOURCE, self.profile, output)
        before = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in output.rglob("*") if p.is_file()}
        with self.assertRaisesRegex(ValueError, "already applied"):
            module.prepare(SOURCE, output, output)
        self.assertEqual(before, {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in output.rglob("*") if p.is_file()})

    def test_in_place_profile_preserves_other_headers(self):
        profile = self.work / "in-place"
        shutil.copytree(self.profile, profile)
        before = (profile / "include/ksystem.h").read_bytes()
        result = module.prepare(SOURCE, profile, profile)
        self.assertEqual((profile / "include/ksystem.h").read_bytes(), before)
        self.assertIn((profile / "include/kthread.h").as_posix(), (profile / "include/boxedwine.h").read_text())
        self.assertTrue((profile / "source/sdl/startupArgs.cpp").is_file())
        self.assertEqual(result["required_include_first"], str(profile / "include"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=SOURCE)
    args, rest = parser.parse_known_args()
    SOURCE = args.source.resolve()
    unittest.main(argv=[__file__] + rest)
