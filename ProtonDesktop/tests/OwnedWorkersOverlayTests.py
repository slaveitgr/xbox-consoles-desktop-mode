#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the checked overlay on actual pinned source; execute no guest code."""
from pathlib import Path
import argparse
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'external/Boxedwine64-full'
RELATIVE = Path('source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp')


class OwnedOverlayTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix='proton-owned-workers-')
        self.addCleanup(self.scratch.cleanup)
        self.directory = Path(self.scratch.name)
        self.profile = self.directory / 'profile'
        result = subprocess.run(['cmake', f'-DPINNED_PROFILE_ROOT={SOURCE}',
            f'-DUWP_PROFILE_OUTPUT={self.profile}', '-P',
            str(ROOT / 'tools/uwp-core/prepare_profile_overlay.cmake')], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def apply(self, source):
        output = self.directory / 'owned.cpp'
        result = subprocess.run(['cmake', f'-DPROFILED_GUEST_WORKERS_SOURCE={source}',
            f'-DOWNED_GUEST_WORKERS_OUTPUT={output}', '-P',
            str(ROOT / 'tools/runtime/prepare_owned_workers.cmake')], capture_output=True, text=True)
        return result, output

    def test_real_worker_body_and_affinity_survive(self):
        source = self.profile / RELATIVE
        result, output = self.apply(source)
        self.assertEqual(result.returncode, 0, result.stderr)
        before, after = source.read_text(), output.read_text()
        begin = 'static void platformThread(CPU* cpu) {'
        end = '\n#ifdef __TEST\nvoid initThreadForTesting()'
        body_before = before.split(begin, 1)[1].split(end, 1)[0]
        body_after = after.split(begin, 1)[1].split(end, 1)[0]
        body_after = body_after.replace(
            'if (platformThreadCount == 0 && !protondesktop::uwp::GuestWorkerSessionManaged()) {',
            'if (platformThreadCount == 0) {')
        self.assertEqual(body_before, body_after)
        self.assertIn('Platform::setCpuAffinityForThread(cpu->thread, KSystem::cpuAffinityCountForApp);', body_after)
        self.assertNotIn('.detach()', after)
        self.assertIn('SpawnGuestWorker', after)
        self.assertEqual(after.count('!protondesktop::uwp::GuestWorkerSessionManaged()'), 2)

    def test_requires_existing_profile_overlay(self):
        result, _ = self.apply(SOURCE / RELATIVE)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('expected 1 occurrences, got 0', result.stderr)

    def test_reapplication_fails_closed(self):
        result, output = self.apply(self.profile / RELATIVE)
        self.assertEqual(result.returncode, 0, result.stderr)
        copied = self.directory / 'already-owned.cpp'
        copied.write_bytes(output.read_bytes())
        result, _ = self.apply(copied)
        self.assertNotEqual(result.returncode, 0)

    def test_ambiguous_source_fails_closed(self):
        duplicated = self.directory / 'duplicated.cpp'
        original = (self.profile / RELATIVE).read_text()
        duplicated.write_text(original + original)
        result, _ = self.apply(duplicated)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('expected 1 occurrences, got 2', result.stderr)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=SOURCE)
    args, other = parser.parse_known_args()
    SOURCE = args.source.resolve()
    if not (SOURCE / RELATIVE).is_file():
        parser.error('Pass --source with the actual pinned Boxedwine64 checkout')
    unittest.main(argv=[__file__, *other])
