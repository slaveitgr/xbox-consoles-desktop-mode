"""Tests use self-authored Wasm metadata; no third-party module is executed."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("verify_payload", Path(__file__).with_name("verify-payload.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def leb(value):
    out = bytearray()
    while True:
        byte = value & 127
        value >>= 7
        out.append(byte | (128 if value else 0))
        if not value:
            return bytes(out)


def memory(flags=1, minimum=8192, maximum=16384):
    section = b"\x01" + leb(flags) + leb(minimum) + leb(maximum)
    return b"\0asm\x01\0\0\0\x05" + leb(len(section)) + section


class WasmProfile(unittest.TestCase):
    def test_bounded_interpreter(self):
        self.assertEqual(module.inspect_wasm(memory()), {
            "minimum_pages": 8192, "maximum_pages": 16384, "shared": False})

    def test_shared_unbounded_and_memory64_rejected(self):
        for flags in (0, 2, 3, 4, 5):
            with self.subTest(flags=flags), self.assertRaises(ValueError):
                module.inspect_wasm(memory(flags=flags))

    def test_resource_limits_rejected(self):
        for minimum, maximum in ((0, 16384), (16385, 16385), (8192, 8191), (8192, 49152)):
            with self.subTest(minimum=minimum, maximum=maximum), self.assertRaises(ValueError):
                module.inspect_wasm(memory(minimum=minimum, maximum=maximum))

    def test_truncation_rejected(self):
        image = memory()
        for length in range(len(image)):
            with self.subTest(length=length), self.assertRaises(ValueError):
                module.inspect_wasm(image[:length])

    def test_duplicate_memory_rejected(self):
        image = memory()
        with self.assertRaises(ValueError):
            module.inspect_wasm(image + image[8:])

    def test_malformed_leb_rejected(self):
        for data in (b"\x80", b"\xff\xff\xff\xff\xff\x00", b"\xff\xff\xff\xff\x7f"):
            with self.subTest(data=data), self.assertRaises(ValueError):
                module.leb(data, 0, len(data))


if __name__ == "__main__":
    unittest.main()
