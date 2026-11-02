"""Synthetic source-gate fixtures. No third-party source is executed."""
from pathlib import Path
import importlib.util
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('inspect_backend', Path(__file__).resolve().parents[1] / 'tools/inspect_backend.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

class SourceInventoryTests(unittest.TestCase):
    def fixture(self, root, *, enabled=False, uwp=False, include_core=False):
        items = ''.join('<ClCompile Include="../../../../' + path + '" />' for path in tool.CORE_BATCH) if include_core else ''
        xml = '<Project xmlns="' + tool.NS['m'] + '"><PropertyGroup>'
        if uwp:
            xml += '<ApplicationType>Windows Store</ApplicationType><AppContainerApplication>true</AppContainerApplication>'
        xml += '</PropertyGroup><ItemDefinitionGroup><ClCompile><PreprocessorDefinitions>'
        xml += 'BOXEDWINE_GUEST_X64=1' if enabled else 'BOXEDWINE_64;BOXEDWINE_JIT_X64'
        xml += '</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup><ItemGroup>' + items + '</ItemGroup></Project>'
        names = tool.CORE_BATCH + [tool.PROJECT, 'platform/windows/platform.cpp']
        for name in names:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(xml if name == tool.PROJECT else '// self-authored source evidence fixture\n')
        return {'commit': tool.PIN, 'files': [{'path': name, 'snapshot_sha256': tool.digest(root / name)} for name in names]}

    def test_host_x64_is_not_guest_x64_or_uwp(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            report = tool.inspect_source(root, self.fixture(root))
            self.assertFalse(report['evidence_errors'])
            self.assertFalse(report['windows_enables_guest_x64'])
            self.assertFalse(report['windows_uwp_target'])
            self.assertEqual(report['missing_core_compile_entries'], tool.CORE_BATCH)
            self.assertFalse(report['safe_to_boot_wine'])

    def test_build_declarations_do_not_admit_runtime(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            report = tool.inspect_source(root, self.fixture(root, enabled=True, uwp=True, include_core=True))
            self.assertTrue(report['windows_enables_guest_x64'])
            self.assertTrue(report['windows_uwp_target'])
            self.assertEqual(report['missing_core_compile_entries'], [])
            self.assertFalse(report['backend_linked'])
            self.assertFalse(report['safe_to_boot_wine'])
            self.assertFalse(report['complete_link_closure_proven'])

    def test_changed_source_and_wrong_pin_fail_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock = self.fixture(root)
            (root / tool.CORE_BATCH[0]).write_text('modified')
            self.assertTrue(tool.inspect_source(root, lock)['evidence_errors'])
            lock = self.fixture(root)
            lock['commit'] = '0' * 40
            self.assertTrue(tool.inspect_source(root, lock)['evidence_errors'])

    def test_missing_required_evidence_and_unsafe_paths_fail(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock = self.fixture(root)
            lock['files'] = lock['files'][1:]
            self.assertIn(tool.CORE_BATCH[0], tool.inspect_source(root, lock)['missing_evidence'])
            lock['files'].append({'path': '../escape', 'snapshot_sha256': '0' * 64})
            self.assertTrue(tool.inspect_source(root, lock)['evidence_errors'])

if __name__ == '__main__':
    unittest.main()
