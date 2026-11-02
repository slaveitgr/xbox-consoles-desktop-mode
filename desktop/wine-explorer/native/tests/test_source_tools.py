"""SPDX-License-Identifier: MIT
Meaningful failure/format checks for the native source build gates, not runtime execution.
"""
from pathlib import Path
import struct
import sys
import unittest

NATIVE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(NATIVE))
sys.path.insert(0, str(NATIVE.parent))
from validate_native import parse_imports, validate_headers  # noqa: E402
from prepare_native import adapt  # noqa: E402
from audit_imports import parse_objdump_imports  # noqa: E402


def minimal_pe() -> bytearray:
    data = bytearray(1024)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 128)
    data[128:132] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 132, 0x8664, 1, 0, 0, 0, 240, 0x22)
    struct.pack_into("<H", data, 152, 0x20b)
    struct.pack_into("<I", data, 168, 0x1000)
    struct.pack_into("<I", data, 260, 16)
    struct.pack_into("<I", data, 428, 0x60000020)
    return data


IMPORTS = """
  Section contains the following imports:
    KERNEL32.dll
                100 HeapAlloc
                101 HeapFree
                102 GetCommandLineW
    USER32.dll
                 10 CreateWindowExW
                 11 GetMessageW
    OLE32.dll
                  A CoCreateInstance
    SHELL32.dll
                  B SHGetDesktopFolder
    COMCTL32.dll
                  C InitCommonControlsEx
  Summary
        1000 .data
"""


class NativeBuildGateTests(unittest.TestCase):
    def test_pe64_format(self):
        self.assertEqual(validate_headers(minimal_pe())["machine"], "AMD64")

    def test_rejects_x86_and_dll(self):
        for offset, value in ((132, 0x14c), (152, 0x10b), (150, 0x2022)):
            data = minimal_pe()
            struct.pack_into("<H", data, offset, value)
            with self.assertRaises(ValueError): validate_headers(data)

    def test_rejects_truncated_headers_and_sections(self):
        for end in (0, 63, 151, 200, 431):
            with self.assertRaises(ValueError): validate_headers(minimal_pe()[:end])

    def test_rejects_bad_pe_offset(self):
        data = minimal_pe()
        struct.pack_into("<I", data, 60, 0xffffffff)
        with self.assertRaises(ValueError): validate_headers(data)

    def test_rejects_crt_delay_and_clr(self):
        for directory in (9, 13, 14):
            data = minimal_pe()
            struct.pack_into("<II", data, 264 + 8 * directory, 0x1000, 32)
            with self.assertRaises(ValueError): validate_headers(data)

    def test_rejects_writable_executable_section(self):
        data = minimal_pe()
        struct.pack_into("<I", data, 428, 0xe0000020)
        with self.assertRaises(ValueError): validate_headers(data)

    def test_direct_imports_and_summary(self):
        imports = parse_imports(IMPORTS)
        self.assertEqual(len(imports), 8)
        self.assertTrue(any(value == {"library": "ole32.dll", "symbol": "CoCreateInstance"} for value in imports))

    def test_rejects_unwanted_runtime(self):
        for library in ("ucrtbase.dll", "ntdll.dll", "win32u.dll", "vcruntime140.dll"):
            with self.assertRaises(ValueError): parse_imports(IMPORTS.replace("COMCTL32.dll", library))

    def test_rejects_bootstrap_and_missing_actual_wine_api(self):
        for symbol in ("__wine_unix_call", "ResolveDelayLoadedAPI", "DelayLoadFailureHook", "Missing"):
            with self.assertRaises(ValueError): parse_imports(IMPORTS.replace("CoCreateInstance", symbol))

    def test_ordinal_preserved(self):
        imports = parse_imports(IMPORTS.replace("  Summary", "               727 ordinal 727\n  Summary"))
        self.assertTrue(any(entry["symbol"] == "#727" for entry in imports))

    def test_actual_dumpbin_ordinal_layout_and_ordinal_only_library(self):
        text = IMPORTS.replace("  Summary", """
    OLEAUT32.dll
             14001A198 Import Address Table
             14001A758 Import Name Table
                     0 time date stamp
                     0 Index of first forwarder reference

                             Ordinal     9
                             Ordinal   411
    SHELL32.dll
                             Ordinal   727
  Summary""")
        imports = parse_imports(text)
        self.assertEqual(len(imports), 11)
        self.assertEqual([entry["symbol"] for entry in imports if entry["library"] == "oleaut32.dll"], ["#411", "#9"])
        self.assertIn({"library": "shell32.dll", "symbol": "#727"}, imports)

    def test_unsupported_ordinal_only_or_empty_library_rejected(self):
        for tail in ("", "                             Ordinal     9\n"):
            text = IMPORTS.replace("  Summary", "    ucrtbase.dll\n" + tail + "  Summary")
            with self.assertRaisesRegex(ValueError, "Unexpected runtime library: ucrtbase.dll"):
                parse_imports(text)

    def test_allowed_empty_library_and_invalid_ordinal_rejected(self):
        with self.assertRaisesRegex(ValueError, "no recovered imports"):
            parse_imports(IMPORTS.replace("  Summary", "    OLEAUT32.dll\n  Summary"))
        with self.assertRaisesRegex(ValueError, "ordinal exceeds"):
            parse_imports(IMPORTS.replace("  Summary", "                             Ordinal 65536\n  Summary"))

    def test_objdump_ordinary_named_and_ordinal_imports(self):
        text = """
        DLL Name: OLEAUT32.dll
        vma:     Ordinal  Hint  Member-Name  Bound-To
        0001a198      9  <none> <none>
        0001a1a0    411  <none> <none>
        DLL Name: SHELL32.dll
        0001a200  <none>  0143  SHGetDesktopFolder
        0001a258    727  <none> <none>
The Function Table (interpreted .pdata section contents)
"""
        imports = parse_objdump_imports(text)
        self.assertEqual([(entry["library"], entry["symbol"]) for entry in imports],
                         [("oleaut32.dll", "#9"), ("oleaut32.dll", "#411"),
                          ("shell32.dll", "SHGetDesktopFolder"), ("shell32.dll", "#727")])

    def test_source_remains_real_wine_and_wrong_source_rejected(self):
        path = NATIVE.parent / "staged/wine/programs/explorer/explorer.c"
        if not path.is_file(): self.skipTest("Run prepare_sources.py before the pinned-source test")
        original = path.read_text(encoding="utf-8")
        generated = adapt(original)
        self.assertIn("Copyright 2004 CodeWeavers, Mike Hearn", generated)
        self.assertIn("IExplorerBrowser_BrowseToObject(info->browser", generated)
        self.assertIn("IShellWindows *sw = NULL", generated)
        self.assertIn("IExplorerBrowserEvents_AddRef(iface)", generated)
        with self.assertRaises(ValueError): adapt("not the pinned Wine source")


if __name__ == "__main__":
    unittest.main()
