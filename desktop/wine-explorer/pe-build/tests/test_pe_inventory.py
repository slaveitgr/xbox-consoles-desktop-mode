"""Self-authored PE metadata fixtures; no executable image is loaded or run."""

import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location("pe_inventory", Path(__file__).parents[1] / "pe_inventory.py")
pe_inventory = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pe_inventory)


class Fixture:
    """One raw .rdata section followed by valid virtual zero-fill."""

    def __init__(self, image_base=0x180000000):
        self.data = bytearray(0x2200)
        self.base = image_base
        self.optional = 0x98
        self.section = self.optional + 0xf0
        self.cursor = 0x1100
        self.data[:2] = b"MZ"
        self.pack(0x3c, "<I", 0x80)
        self.data[0x80:0x84] = b"PE\0\0"
        self.pack(0x84, "<HHIIIHH", 0x8664, 1, 0, 0, 0, 0xf0, 0x2022)
        self.pack(self.optional, "<H", 0x20b)
        self.pack(self.optional + 24, "<Q", self.base)
        self.pack(self.optional + 32, "<II", 0x1000, 0x200)
        self.pack(self.optional + 56, "<II", 0x4000, 0x200)
        self.pack(self.optional + 108, "<I", 16)
        self.data[self.section:self.section + 8] = b".rdata\0\0"
        self.pack(self.section + 8, "<IIII", 0x2800, 0x1000, 0x2000, 0x200)

    def pack(self, offset, fmt, *values):
        struct.pack_into(fmt, self.data, offset, *values)

    def offset(self, rva):
        return rva - 0x1000 + 0x200

    def put(self, rva, payload):
        offset = self.offset(rva)
        self.data[offset:offset + len(payload)] = payload

    def alloc(self, payload, alignment=8):
        rva = (self.cursor + alignment - 1) // alignment * alignment
        self.put(rva, payload)
        self.cursor = rva + len(payload)
        return rva

    def string(self, value):
        return self.alloc(value.encode("ascii") + b"\0", 1)

    def directory(self, index, rva, size):
        self.pack(self.optional + 112 + index * 8, "<II", rva, size)

    def imports(self, fallback=False):
        dll = self.string("KERNEL32.dll")
        by_name = self.alloc(struct.pack("<H", 17) + b"GetVersionExA\0")
        table = self.alloc(struct.pack("<3Q", by_name, (1 << 63) | 42, 0))
        iat = self.alloc(struct.pack("<3Q", by_name, (1 << 63) | 42, 0))
        desc = self.alloc(struct.pack("<5I", 0 if fallback else table, 0, 0, dll, iat) + bytes(20))
        self.directory(1, desc, 40)
        return {"desc": desc, "table": table, "iat": iat, "by_name": by_name}

    def delay(self, va=False):
        dll = self.string("VERSION.dll")
        by_name = self.alloc(struct.pack("<H", 3) + b"GetFileVersionInfoW\0")
        encode = (lambda value: self.base + value) if va else (lambda value: value)
        table = self.alloc(struct.pack("<3Q", encode(by_name), (1 << 63) | 9, 0))
        iat = self.alloc(bytes(24))
        module = self.alloc(bytes(8))
        desc = self.alloc(struct.pack("<8I", 0 if va else 1, encode(dll), encode(module), encode(iat), encode(table), 0, 0, 0) + bytes(32))
        self.directory(13, desc, 64)
        return {"desc": desc, "table": table, "iat": iat, "by_name": by_name}

    def exports(self):
        block = self.alloc(bytes(0x300))
        dll = block + 0x40
        forward_name = block + 0x60
        forward_ordinal = block + 0x90
        functions = block + 0xc0
        names = block + 0xe0
        ordinals = block + 0xf0
        strings = [block + 0x110, block + 0x130, block + 0x150, block + 0x170]
        self.put(dll, b"sample.dll\0")
        self.put(forward_name, b"KERNELBASE.GetVersionExA\0")
        self.put(forward_ordinal, b"NTDLL.dll.#123\0")
        self.put(functions, struct.pack("<4I", 0x3100, forward_name, forward_ordinal, 0))
        self.put(names, struct.pack("<4I", *strings))
        self.put(ordinals, struct.pack("<4H", 0, 0, 1, 2))
        for rva, value in zip(strings, (b"Native", b"NativeAlias", b"ForwardName", b"ForwardOrdinal")):
            self.put(rva, value + b"\0")
        self.put(block, struct.pack("<IIHHIIIIIII", 0, 0, 0, 0, dll, 7, 4, 4, functions, names, ordinals))
        self.directory(0, block, 0x300)
        return {"block": block, "functions": functions, "names": names, "ordinals": ordinals, "forward_name": forward_name}


class InventoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "fixture.dll"

    def inspect(self, fixture):
        self.path.write_bytes(fixture.data)
        return pe_inventory.inspect_pe(self.path)

    def assert_error(self, fixture, fragment):
        result = self.inspect(fixture)
        self.assertEqual("error", result["status"], result)
        self.assertIn(fragment, result["error"])
        for key in ("imports", "delay_imports", "exports", "dependency_libraries"):
            self.assertIsNone(result[key], key)
        return result

    def test_names_ordinals_delays_forwarders_and_hash(self):
        fixture = Fixture()
        fixture.imports()
        fixture.delay()
        fixture.exports()
        result = self.inspect(fixture)
        self.assertEqual("ok", result["status"], result)
        self.assertEqual(len(fixture.data), result["bytes"])
        self.assertEqual(hashlib.sha256(fixture.data).hexdigest(), result["sha256"])
        self.assertEqual("sample.dll", result["parsedDLL"])
        self.assertEqual([{"name": "GetVersionExA", "hint": 17}, {"ordinal": 42}], result["imports"][0]["symbols"])
        self.assertEqual([{"name": "GetFileVersionInfoW", "hint": 3}, {"ordinal": 9}], result["delay_imports"][0]["symbols"])
        self.assertEqual("rva", result["delay_imports"][0]["address_mode"])
        self.assertEqual(["Native", "NativeAlias"], result["exports"][0]["names"])
        self.assertEqual([7, 8, 9], [item["ordinal"] for item in result["exports"]])
        self.assertEqual("GetVersionExA", result["exports"][1]["forwarder_name"])
        self.assertEqual("ntdll.dll", result["exports"][2]["forwarder_module"])
        self.assertEqual(123, result["exports"][2]["forwarder_ordinal"])
        self.assertEqual(["kernel32.dll", "kernelbase.dll", "ntdll.dll", "version.dll"], result["dependency_libraries"])
        self.assertEqual("unknown", result["dynamic_load_closure"]["status"])
        self.assertEqual("NOT SAFE TO EXECUTE", result["safety"])

    def test_legacy_va_delay_descriptor_and_thunks(self):
        fixture = Fixture(image_base=0x400000)
        fixture.delay(va=True)
        result = self.inspect(fixture)
        self.assertEqual("ok", result["status"], result)
        self.assertEqual("va", result["delay_imports"][0]["address_mode"])
        self.assertEqual("GetFileVersionInfoW", result["delay_imports"][0]["symbols"][0]["name"])

    def test_missing_lookup_uses_unbound_iat(self):
        fixture = Fixture()
        fixture.imports(fallback=True)
        self.assertEqual("ok", self.inspect(fixture)["status"])

    def test_bound_iat_without_lookup_is_unknown(self):
        fixture = Fixture()
        locations = fixture.imports(fallback=True)
        fixture.pack(fixture.offset(locations["desc"]) + 4, "<I", 0xffffffff)
        self.assert_error(fixture, "bound IAT")

    def test_missing_file_is_unknown_not_empty(self):
        result = pe_inventory.inspect_pe(self.path)
        self.assertEqual("error", result["status"])
        self.assertIsNone(result["sha256"])
        self.assertIsNone(result["dependency_libraries"])

    def test_no_declarations_are_empty_with_unknown_dynamic_closure(self):
        result = self.inspect(Fixture())
        self.assertEqual("ok", result["status"], result)
        self.assertIsNone(result["parsedDLL"])
        self.assertEqual([], result["imports"])
        self.assertEqual([], result["exports"])
        self.assertEqual("unknown", result["dynamic_load_closure"]["status"])

    def test_pe32_machine_and_truncated_raw_ranges(self):
        for offset, fmt, value, message in ((0x84, "<H", 0x14c, "AMD64"), (0x98, "<H", 0x10b, "PE32+"), (0x3c, "<I", 0xffffff00, "out of bounds")):
            with self.subTest(message=message):
                fixture = Fixture()
                fixture.pack(offset, fmt, value)
                self.assert_error(fixture, message)
        fixture = Fixture()
        fixture.data = fixture.data[:-1]
        self.assert_error(fixture, "out of bounds")

    def test_directory_full_span_must_be_raw_backed(self):
        fixture = Fixture()
        locations = fixture.imports()
        fixture.directory(1, locations["desc"], 0x3100 - locations["desc"])
        self.assert_error(fixture, "virtual zero-fill")

    def test_import_descriptor_terminates_inside_declared_directory(self):
        fixture = Fixture()
        locations = fixture.imports()
        fixture.directory(1, locations["desc"], 20)
        self.assert_error(fixture, "no terminator")

    def test_import_name_and_table_reject_zero_fill_and_rva_overflow(self):
        for value, message in ((0x3100, "virtual zero-fill"), (0x5000, "exceeds image"), (1 << 40, "reserved bits")):
            with self.subTest(value=value):
                fixture = Fixture()
                locations = fixture.imports()
                fixture.pack(fixture.offset(locations["table"]), "<Q", value)
                self.assert_error(fixture, message)

    def test_import_ordinal_reserved_bits_rejected(self):
        fixture = Fixture()
        locations = fixture.imports()
        fixture.pack(fixture.offset(locations["table"]), "<Q", (1 << 63) | (1 << 16) | 42)
        self.assert_error(fixture, "ordinal thunk has reserved")

    def test_unterminated_lookup_rejected_at_raw_boundary(self):
        fixture = Fixture()
        locations = fixture.imports()
        fixture.pack(fixture.offset(locations["desc"]), "<I", 0x2ff8)
        fixture.put(0x2ff8, struct.pack("<Q", (1 << 63) | 1))
        self.assert_error(fixture, "virtual zero-fill")

    def test_delay_reserved_attrs_missing_lookup_and_bad_va(self):
        for field, value, message in ((0, 3, "reserved attributes"), (16, 0, "missing required")):
            fixture = Fixture()
            locations = fixture.delay()
            fixture.pack(fixture.offset(locations["desc"]) + field, "<I", value)
            self.assert_error(fixture, message)
        fixture = Fixture(image_base=0x400000)
        locations = fixture.delay(va=True)
        fixture.pack(fixture.offset(locations["desc"]) + 4, "<I", 0x1000)
        self.assert_error(fixture, "below image base")

    def test_delay_module_storage_validates_whole_virtual_span(self):
        fixture = Fixture()
        locations = fixture.delay()
        fixture.pack(fixture.offset(locations["desc"]) + 8, "<I", 0x37fc)
        # The original mapping ends at 0x3800. A second, zero-fill-only section
        # contains the last byte but leaves a gap inside the eight-byte slot.
        fixture.pack(0x86, "<H", 2)
        fixture.pack(fixture.section + 40 + 8, "<IIII", 0x100, 0x3803, 0, 0)
        self.assert_error(fixture, "unmapped image gap")

    def test_export_ordinal_index_and_excessive_count_rejected(self):
        fixture = Fixture()
        locations = fixture.exports()
        fixture.pack(fixture.offset(locations["ordinals"]), "<H", 4)
        self.assert_error(fixture, "ordinal index")
        fixture = Fixture()
        locations = fixture.exports()
        fixture.pack(fixture.offset(locations["block"]) + 20, "<I", 0xffffffff)
        self.assert_error(fixture, "symbol count")

    def test_forwarder_cannot_read_past_export_directory(self):
        fixture = Fixture()
        locations = fixture.exports()
        fixture.directory(0, locations["block"], locations["forward_name"] + 3 - locations["block"])
        self.assert_error(fixture, "unterminated")

    def test_forwarder_syntax_and_ordinal_range(self):
        for value, message in ((b"missing_dot\0", "syntax"), (b"NTDLL.#65536\0", "16 bits"), (b"NTDLL.#abc\0", "invalid forwarded")):
            fixture = Fixture()
            locations = fixture.exports()
            fixture.put(locations["forward_name"], value)
            self.assert_error(fixture, message)

    def test_section_header_and_directory_budgets(self):
        fixture = Fixture()
        fixture.pack(0x86, "<H", 97)
        self.assert_error(fixture, "section count")
        fixture = Fixture()
        fixture.pack(fixture.optional + 108, "<I", 129)
        self.assert_error(fixture, "directory count")
        fixture = Fixture()
        fixture.pack(fixture.section + 12, "<I", 0x100)
        self.assert_error(fixture, "overlapping virtual")

    def test_file_and_symbol_budgets(self):
        fixture = Fixture()
        with mock.patch.object(pe_inventory, "MAX_FILE_BYTES", 128):
            self.assert_error(fixture, "byte budget")
        fixture = Fixture()
        fixture.imports()
        with mock.patch.object(pe_inventory, "MAX_SYMBOLS", 1):
            self.assert_error(fixture, "symbol count")

    def test_inventory_partial_and_cli(self):
        self.path.write_bytes(Fixture().data)
        missing = Path(self.temp.name) / "missing.dll"
        report = pe_inventory.inventory([self.path, missing])
        self.assertEqual("partial", report["status"])
        self.assertEqual({"ok": 1, "errors": 1}, report["counts"])
        self.assertFalse(report["dependency_union_complete_for_inputs"])
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = pe_inventory.main(["--binary", str(self.path), "--binary", str(missing), "--output", "-"])
        self.assertEqual(1, code)
        self.assertEqual("partial", json.loads(output.getvalue())["status"])
        target = Path(self.temp.name) / "report.json"
        self.assertEqual(0, pe_inventory.main(["--binary", str(self.path), "--output", str(target)]))
        self.assertEqual("ok", json.loads(target.read_text())["status"])


if __name__ == "__main__":
    unittest.main()
