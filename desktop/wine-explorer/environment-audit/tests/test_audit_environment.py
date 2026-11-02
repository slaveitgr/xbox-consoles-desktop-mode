"""SPDX-License-Identifier: MIT
Self-authored source/PE metadata fixtures; no guest source or PE is executed.
"""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

AUDIT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(AUDIT))
from audit_environment import (PeAuditError, audit_binary, disassembly_findings, inspect_pe64,
                               module_closure, parse_makefile, scan_sources, scan_text)  # noqa: E402


def pe_fixture(tls=True):
    data = bytearray(0x1200)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 0x84, 0x8664, 1, 0, 0, 0, 240, 0x22)
    optional = 0x98
    struct.pack_into("<H", data, optional, 0x20b)
    struct.pack_into("<Q", data, optional + 24, 0x140000000)
    struct.pack_into("<II", data, optional + 56, 0x4000, 0x200)
    struct.pack_into("<I", data, optional + 108, 16)
    section = optional + 240
    data[section:section + 8] = b".tls\0\0\0\0"
    struct.pack_into("<IIII", data, section + 8, 0x2000, 0x1000, 0x1000, 0x200)
    if tls:
        struct.pack_into("<II", data, optional + 112 + 9 * 8, 0x1100, 40)
        struct.pack_into("<QQQQII", data, 0x300, 0x140001400, 0x140001404, 0x140001200, 0x140001300, 16, 0)
        struct.pack_into("<I", data, 0x400, 7)
        struct.pack_into("<QQ", data, 0x500, 0x140001800, 0)
        data[0x600:0x604] = b"ABCD"
    return data


def write(root, path, text):
    file = root / path
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(text, encoding="utf-8")


class SourceAuditTests(unittest.TestCase):
    def test_segment_forms_intrinsics_and_context(self):
        text = """
// __readgsqword(0x30) is a documentation candidate
__asm__("movq %%gs:0x30,%0");
__asm { mov rax, gs:[0x30] }
__asm { mov ax, gs }
__asm { mov gs, ax }
__asm { push fs }
value = __readfsdword(0x18);
__writegsqword(0x58, value);
value = __builtin_ia32_rdgsbase64();
__builtin_ia32_wrfsbase64(value);
"""
        hits = scan_text(text, "fixture.c", "pe_source_candidate", "dlls/test")
        self.assertTrue(any(h["category"] == "segment_intrinsic" and h["context"] == "comment" for h in hits))
        self.assertTrue(any(h["category"] == "segment_memory" and h["context"] == "string" for h in hits))
        self.assertTrue(any(h["category"] == "segment_intrinsic" and h["access"] == "write" for h in hits))
        self.assertEqual(sum(h["category"] == "segment_base_builtin" for h in hits), 2)
        for snippet in ("mov ax, gs", "mov gs, ax", "push fs"):
            self.assertTrue(any(h["category"] == "segment_register_instruction" and snippet in h["snippet"] for h in hits))
        self.assertTrue(all(h["line"] > 0 and h["file"] == "fixture.c" for h in hits))

    def test_nt_current_teb_definition_and_peb_reference(self):
        text = "static FORCEINLINE struct _TEB * WINAPI NtCurrentTeb(void)\n{ return 0; }\nvoid f() { NtCurrentTeb(); RtlGetCurrentPeb(); }\n"
        hits = scan_text(text, "winnt.h", "shared_header", None)
        self.assertEqual(sum(h["category"] == "nt_current_teb_definition" for h in hits), 1)
        self.assertEqual(sum(h["category"] == "nt_current_teb_reference" for h in hits), 2)
        self.assertEqual(sum(h["category"] == "rtl_get_current_peb_reference" for h in hits), 1)

    def test_compiler_tls_is_separate_from_dynamic_tls(self):
        text = "TlsAlloc(); TlsGetValue(1); FlsAlloc(0); _get_thread_local_invalid_parameter_handler();"
        hits = scan_text(text, "tls.c", "pe_source_candidate", "dlls/test")
        self.assertEqual({h["category"] for h in hits}, {"dynamic_tls_api_reference"})
        text = '__declspec(thread) int a; _Thread_local int b; thread_local int c; __thread int d; DECLSPEC_THREAD int e; __attribute__((tls_model("initial-exec"))) int f; __declspec(allocate(".tls$AAA")) int g;'
        hits = scan_text(text, "tls.c", "pe_source_candidate", "dlls/test")
        self.assertEqual(sum(h["category"] == "compiler_tls_declaration" for h in hits), 4)
        self.assertTrue(any(h["category"] == "compiler_tls_macro" for h in hits))
        self.assertTrue(any(h["category"] == "static_tls_annotation" for h in hits))

    def test_makefile_local_variables_continuations_and_unknowns(self):
        values = parse_makefile("COMMON = user32\nIMPORTS = $(COMMON) \\\n ntdll $(PLATFORM_LIBS)\nIMPORTS += gdi32\nDELAYIMPORTS = shell32\n")
        self.assertEqual(values["IMPORTS"].split(), ["user32", "ntdll", "$(PLATFORM_LIBS)", "gdi32"])

    def test_closure_eager_delayed_parent_and_unix_source_roles(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write(root, "programs/explorer/Makefile.in", "IMPORTS = user32 $(UNKNOWN)\nDELAYIMPORTS = shell32\nSOURCES = explorer.c\n")
            write(root, "programs/explorer/explorer.c", "NtCurrentTeb();\n")
            write(root, "programs/explorer/tests/test.c", "__readgsqword(0);\n")
            write(root, "dlls/user32/Makefile.in", "IMPORTS = ntdll\nSOURCES = main.c driver.c\n")
            write(root, "dlls/user32/main.c", "NtCurrentTeb();\n")
            write(root, "dlls/user32/driver.c", '#pragma makedep unix\n__asm__("mov %%gs:0,%rax");\n')
            write(root, "dlls/ntdll/Makefile.in", "UNIXLIB = ntdll.so\nSOURCES = unix/foo.c\n")
            write(root, "dlls/ntdll/unix/foo.c", "__thread int x;\n")
            write(root, "dlls/shell32/Makefile.in", "IMPORTS = user32\nPARENTSRC = ../common\n")
            write(root, "dlls/common/Makefile.in", "SOURCES = helper.c\n")
            write(root, "include/winnt.h", "#define NtCurrentTeb() accessor()\n")
            graph = module_closure(root, ["programs/explorer"])
            nodes = {node["path"]: node for node in graph["nodes"]}
            self.assertEqual(nodes["dlls/shell32"]["reachability"], ["delayed"])
            self.assertEqual(nodes["dlls/common"]["reachability"], ["delayed"])
            self.assertIn("eager", nodes["dlls/user32"]["reachability"])
            self.assertTrue(any(value["token"] == "$(UNKNOWN)" for value in graph["unresolved_dependencies"]))
            report = scan_sources(root, graph)
            self.assertEqual(report["test_files_skipped"], 1)
            self.assertTrue(any(hit["source_role"] == "unix_source" for hit in report["findings"]))
            self.assertTrue(any(hit["source_role"] == "shared_header" for hit in report["findings"]))
            self.assertFalse(any("/tests/" in hit["file"] for hit in report["findings"]))

    def test_cli_no_binary_coverage_is_unknown(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write(root, "dlls/ntdll/Makefile.in", "SOURCES = main.c\n")
            write(root, "dlls/ntdll/main.c", "int ordinary;\n")
            output = root / "audit.json"
            subprocess.run([sys.executable, str(AUDIT / "audit_environment.py"), "--source-root", str(root), "--module", "dlls/ntdll", "--output", str(output)], check=True, capture_output=True)
            result = json.loads(output.read_text())
            self.assertEqual(result["binary_coverage"]["status"], "unknown_no_binaries_supplied")
            self.assertFalse(result["binary_coverage"]["full_selected_dll_closure_inspected"])
            self.assertFalse(result["runtime_structs_or_headers_modified"])


class BinaryAuditTests(unittest.TestCase):
    def test_tls_va_rva_index_template_and_callback(self):
        result = inspect_pe64(pe_fixture())
        tls = result["tls"]
        self.assertEqual(tls["address_of_index_rva"], 0x1200)
        self.assertEqual(tls["index_initial_value"], 7)
        self.assertEqual(tls["raw_template_size"], 4)
        self.assertEqual(tls["size_of_zero_fill"], 16)
        self.assertEqual(tls["callbacks"], [{"va": 0x140001800, "rva": 0x1800, "in_image": True}])

    def test_zero_fill_index_is_mapped_not_fabricated_file_data(self):
        data = pe_fixture()
        struct.pack_into("<Q", data, 0x310, 0x140002200)
        tls = inspect_pe64(data)["tls"]
        self.assertEqual(tls["index_storage"], "virtual_zero_fill")
        self.assertEqual(tls["index_initial_value"], 0)

    def test_no_directory_does_not_prove_no_static_tls_or_gs(self):
        result = inspect_pe64(pe_fixture(False))
        self.assertFalse(result["tls"]["directory_present"])
        self.assertEqual(len(result["tls_sections"]), 1)
        self.assertFalse(result["compiler_tls_absence_proven"])

    def test_rejects_pe32_invalid_offsets_and_truncation(self):
        for offset, value in ((0x84, 0x14c), (0x98, 0x10b)):
            data = pe_fixture(); struct.pack_into("<H", data, offset, value)
            with self.assertRaises(PeAuditError): inspect_pe64(data)
        data = pe_fixture(); struct.pack_into("<I", data, 60, 0xffffffff)
        with self.assertRaises(PeAuditError): inspect_pe64(data)
        with self.assertRaises(PeAuditError): inspect_pe64(pe_fixture()[:0x505])

    def test_rejects_tls_va_and_declared_directory_extent_outside_image(self):
        data = pe_fixture(); struct.pack_into("<Q", data, 0x310, 0x140008000)
        with self.assertRaises(PeAuditError): inspect_pe64(data)
        data = pe_fixture(); struct.pack_into("<I", data, 0x98 + 112 + 9 * 8 + 4, 0x4000)
        with self.assertRaises(PeAuditError): inspect_pe64(data)

    def test_rejects_reversed_template_and_unterminated_callback_array(self):
        data = pe_fixture(); struct.pack_into("<Q", data, 0x308, 0x140001300)
        with self.assertRaises(PeAuditError): inspect_pe64(data)
        data = pe_fixture()
        for index in range(256): struct.pack_into("<Q", data, 0x500 + index * 8, 0x140001800)
        with self.assertRaises(PeAuditError): inspect_pe64(data)

    def test_disassembly_atnt_intel_and_segment_base(self):
        lines = ["140001000: mov %gs:0x30,%rax", "140001010: mov rax,QWORD PTR gs:[0x58]", "140001020: mov ax,gs", "140001030: push fs", "140001040: wrgsbase %rax", "140001050: mov %rax,%rbx", "140001060: lgs eax,FWORD PTR [ebx]"]
        self.assertEqual(len(disassembly_findings(lines)), 6)

    def test_unavailable_objdump_and_invalid_binary_remain_unknown(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "fixture.exe"; path.write_bytes(pe_fixture(False))
            result = audit_binary(path, "xcdm-tool-that-does-not-exist")
            self.assertEqual(result["metadata_status"], "inspected")
            self.assertEqual(result["segment_disassembly"]["status"], "unknown_tool_unavailable")
            self.assertIsNone(result["segment_disassembly"]["finding_count"])
            path.write_bytes(b"not a PE")
            result = audit_binary(path, "xcdm-tool-that-does-not-exist")
            self.assertEqual(result["metadata_status"], "unknown")
            self.assertIsNone(result["segment_disassembly"]["finding_count"])


if __name__ == "__main__": unittest.main()
