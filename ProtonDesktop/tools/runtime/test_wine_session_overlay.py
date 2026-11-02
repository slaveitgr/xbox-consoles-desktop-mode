#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Pin-specific regressions for generated launch/lifecycle changes, no Wine."""
from pathlib import Path
import importlib.util
import argparse
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location("wine_overlay", HERE / "prepare_wine_session.py")
overlay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(overlay)
SOURCE = ROOT / "external/Boxedwine64-full"


class OverlayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (SOURCE / "source/kernel/kprocess.cpp").is_file():
            raise unittest.SkipTest("Pinned source checkout is required")

    def source(self, path):
        return (SOURCE / path).read_text().replace("\r\n", "\n")

    def test_changed_pin_cannot_silently_skip_a_launch_fix(self):
        text = self.source("source/kernel/kprocess.cpp")
        with self.assertRaises(ValueError):
            overlay.process(text.replace("        scheduleThread(thread);", "        a_new_scheduler(thread);"))

    def test_startup_keeps_real_vfs_and_checks_both_zip_entrypoints(self):
        cpp, header = overlay.startup(self.source("source/sdl/startupArgs.cpp"),
                                      self.source("source/sdl/startupArgs.h"))
        prepare = cpp[cpp.index("bool StartUpArgs::prepareProtonSession("):cpp.index("bool StartUpArgs::apply()")]
        self.assertIn("buildVirtualFileSystem();", prepare)
        self.assertIn("x11_init();", prepare)
        self.assertIn('if (!fsZip->init(zip, B(""))) return false;', prepare)
        self.assertIn("if (!fsZip->init(info.nativePath, info.localPath)) return false;", prepare)
        self.assertNotIn("/mnt/drive_t", prepare)
        self.assertNotIn("envValues.push_back(B(\"HOME=", prepare)
        self.assertIn("prepareProtonSession", header)

    def test_all_start_and_clone_call_sites_check_admission(self):
        cpp = overlay.process(self.source("source/kernel/kprocess.cpp"))
        # The sole remaining raw call is inside the checked exception boundary.
        self.assertEqual(cpp.count("scheduleThread("), 1)
        self.assertEqual(cpp.count("if (!protonScheduleNewThread("), 6)
        initial = cpp[cpp.index("KThread* KProcess::startProcess("):cpp.index("U32 KProcess::exit(")]
        self.assertEqual(initial.count("protonRollbackUnscheduledThread(thread);"), 2)
        self.assertIn("ChangeThread bindInitialThread(thread);", initial)
        self.assertIn("protonOldParentTid", cpp)
        self.assertNotIn("return newThread->id;", cpp)

    def test_stop_split_does_not_destroy_kernel_on_controller(self):
        cpp, header = overlay.kernel(self.source("source/kernel/ksystem.cpp"),
                                     self.source("include/ksystem.h"))
        stop = cpp[cpp.index("void KSystem::protonStopGuestProcesses()"):cpp.index("void KSystem::destroy()")]
        self.assertIn("p->killAllThreads();", stop)
        self.assertNotIn("Fs::shutDown", stop)
        self.assertNotIn("shutdownRam", stop)
        self.assertIn("protonStopGuestProcesses", header)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=SOURCE)
    args, remaining = parser.parse_known_args()
    SOURCE = args.source.resolve()
    if not (SOURCE / 'source/kernel/kprocess.cpp').is_file():
        parser.error('Pass --source with the actual pinned Boxedwine64 checkout')
    unittest.main(argv=[__file__, *remaining])
