#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Pinned overlay preservation/rejection checks; not guest runtime tests."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

PARSER = argparse.ArgumentParser()
PARSER.add_argument("--source", required=True, type=Path)
ARGS, REMAINDER = PARSER.parse_known_args()
SCRIPT = Path(__file__).resolve().parents[1] / "prepare_profile_overlay.cmake"


class ProfileOverlayTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.output = Path(self.temp.name) / "output"

    def generate(self, source=ARGS.source):
        return subprocess.run(["cmake", f"-DPINNED_PROFILE_ROOT={source}",
                               f"-DUWP_PROFILE_OUTPUT={self.output}", "-P", str(SCRIPT)],
                              capture_output=True, text=True)

    def text(self, relative):
        return (self.output / relative).read_text()

    def test_preserves_inputs_and_omits_only_unsupported_nodes(self):
        relatives = ["source/kernel/sys/sysfs.cpp", "source/sdl/startupArgs.cpp",
                     "platform/sdl/knativeaudio.cpp"]
        before = {p: hashlib.sha256((ARGS.source / p).read_bytes()).hexdigest() for p in relatives}
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(before, {p: hashlib.sha256((ARGS.source / p).read_bytes()).hexdigest()
                                  for p in relatives})
        startup = self.text("source/sdl/startupArgs.cpp")
        for path in ("/proc/stat", "/proc/cpuinfo", "/dev/dsp", "/dev/mixer", "/dev/sequencer"):
            self.assertNotIn(f'B("{path}"), open', startup)
        for path in ("/proc/meminfo", "/proc/uptime", "/dev/urandom", "/dev/fb0"):
            self.assertIn(f'B("{path}"), open', startup)
        sysfs = self.text("source/kernel/sys/sysfs.cpp")
        self.assertNotIn("Platform::getCpuFreqMHz()", sysfs)
        self.assertIn("Platform::getCpuCount()", sysfs)
        self.assertIn('B("/sys/devices/system/cpu/online")', sysfs)
        audio = self.text("platform/sdl/knativeaudio.cpp")
        self.assertNotIn("Fake Midi", audio)
        self.assertNotIn("initWindowsAudio", audio)
        self.assertNotIn("KNativeAudioSDL", audio)
        self.assertIn("Copyright (C) 2012-2025", audio)

    def test_changed_pin_anchor_fails_closed(self):
        source = Path(self.temp.name) / "changed"
        for p in (ARGS.source / "platform/sdl").glob("knativeaudio.cpp"):
            destination = source / p.relative_to(ARGS.source)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(p, destination)
        destination = source / "source/kernel/devs/devsequencer.cpp"
        destination.parent.mkdir(parents=True, exist_ok=True)
        original = (ARGS.source / destination.relative_to(source)).read_text()
        destination.write_text(original.replace("        audio->load();", "        changed_load();"))
        result = self.generate(source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no longer matches", result.stderr)

    def test_worker_affinity_and_socket_failure_boundaries(self):
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        cpu = self.text("source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp")
        worker, parent = cpu.split("void scheduleThread(KThread* thread) {", 1)
        self.assertNotIn("Platform::setCpuAffinityForThread", parent)
        self.assertLess(worker.index("KThread::setCurrentThread(cpu->thread)"),
                        worker.index("Platform::setCpuAffinityForThread(cpu->thread"))
        sockets = self.text("source/kernel/knativesocket.cpp")
        start = sockets.split("void startNativeSocketsThread() {", 1)[1].split(
            "void stopNativeSocketsThread() {", 1)[0]
        self.assertLess(start.index("Platform::nativeSocketPair(nativeSocketPipe) != 0"),
                        start.index("ioctlsocket("))
        self.assertLess(start.index("ioctlsocket("), start.index("createAndStartThread("))
        self.assertIn("if (!checkWaitingNativeSocketsThread)", start)
        self.assertIn("static std::atomic<bool> checkWaitingNativeSocketsThreadDone", sockets)
        stop = sockets.split("void stopNativeSocketsThread() {", 1)[1].split("#endif", 1)[0]
        self.assertLess(stop.index("->wait()"), stop.index("closesocket("))
        self.assertIn("nativeSocketPipe[0] = nativeSocketPipe[1] = -1", stop)
        threads = self.text("platform/sdl/knativethread.cpp")
        self.assertIn("if (!result->nativeThread)", threads)
        self.assertIn("delete result;\n        return nullptr;", threads)


if __name__ == "__main__":
    unittest.main(argv=[__file__, *REMAINDER])
