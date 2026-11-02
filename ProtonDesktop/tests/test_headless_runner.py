# SPDX-License-Identifier: GPL-2.0-only
"""Result-validation fixtures; do not build or execute third-party code."""
from pathlib import Path
import importlib.util
import json
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('headless_runner', Path(__file__).resolve().parents[1] / 'tools/run_headless_smoke.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

def sample(status, marker=False):
    return ((tool.EXPECTED + '\n' if marker else '')
            + f'CPU64: exit syscall, status={status}\n'
            + '--x64-run-elf: stopped after 7 instructions (yield=1 RIP=0x401059 RAX=0x0)\n'
            + '--x64-run-elf: exit reached cleanly\n')

class ResultValidationTests(unittest.TestCase):
    def test_exact_guest_status_and_success_marker(self):
        self.assertTrue(tool.verify_guest_log(sample(0, True), 0, True))
        self.assertTrue(tool.verify_guest_log(sample(9), 9, False))
        self.assertFalse(tool.verify_guest_log(sample(90), 9, False))
        self.assertFalse(tool.verify_guest_log(sample(0), 0, True))
        self.assertFalse(tool.verify_guest_log(sample(9, True), 9, False))

    def test_ambiguous_exit_and_missing_stop_are_rejected(self):
        self.assertFalse(tool.verify_guest_log(sample(9) + 'CPU64: exit syscall, status=9\n', 9, False))
        self.assertFalse(tool.verify_guest_log(sample(9).replace('yield=1', 'yield=0'), 9, False))
        self.assertFalse(tool.verify_guest_log(sample(9).replace('exit reached cleanly', 'instruction cap hit'), 9, False))

    def test_new_run_invalidates_previous_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'result.json'
            path.write_text(json.dumps({'status': 'passed', 'positive_guest_smoke': True}))
            tool.begin_report(path)
            state = json.loads(path.read_text())
            self.assertEqual(state['status'], 'incomplete')
            self.assertNotIn('positive_guest_smoke', state)

if __name__ == '__main__':
    unittest.main()
