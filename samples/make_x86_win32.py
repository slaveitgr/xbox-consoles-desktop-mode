#!/usr/bin/env python3
"""Generate a deterministic, self-authored PE32 Win32 host-call smoke fixture.

The normal output is C++ source, not an executable on disk. An executable may
be written explicitly with --output for inspection by an external PE tool.
There is no compiler, assembler, CRT, third-party binary, or network dependency.

Program (32-bit stdcall, no CRT):
    SetLastError(0x1234)
    if GetLastError() != 0x1234: ExitProcess(1)
    OutputDebugStringA("x86 Win32 host calls passed")
    ExitProcess(0)

ExitProcess must stop the guest. A self-loop after either call makes an
incorrectly returning ExitProcess hit the engine's instruction budget instead
of falling through into a successful branch.
"""

import argparse
import hashlib
from pathlib import Path
import struct


IMAGE_BASE = 0x400000
TEXT_RVA = 0x1000
IDATA_RVA = 0x2000
RDATA_RVA = 0x3000
FILE_ALIGNMENT = 0x200
IMAGE_SIZE = 0x4000
MESSAGE = b"x86 Win32 host calls passed\0"
IMPORTS = (
    ("SetLastError", 0xA0),
    ("GetLastError", 0xB0),
    ("OutputDebugStringA", 0xC0),
    ("ExitProcess", 0xE0),
)
LOOKUP_OFFSET = 0x40
IAT_OFFSET = 0x60
DLL_OFFSET = 0x80


def make_code() -> bytes:
    """Hand-encode ordinary protected-mode x86 integer/stack instructions."""
    code = bytearray()

    def emit(data: bytes) -> None:
        code.extend(data)

    def call_import(index: int) -> None:
        # ff 15 <absolute address>: call dword ptr [IMAGE_BASE + IAT RVA].
        emit(b"\xff\x15" + struct.pack("<I", IMAGE_BASE + IDATA_RVA + IAT_OFFSET + index * 4))

    emit(b"\x68\x34\x12\x00\x00")  # push 0x1234
    call_import(0)                       # call [SetLastError]
    call_import(1)                       # call [GetLastError]
    emit(b"\x3d\x34\x12\x00\x00")  # cmp eax, 0x1234
    emit(b"\x75\x00")                  # jne failure; patch its rel8 below
    branch_displacement = len(code) - 1
    emit(b"\x68" + struct.pack("<I", IMAGE_BASE + RDATA_RVA))  # push message VA
    call_import(2)                       # call [OutputDebugStringA]
    emit(b"\x6a\x00")                  # push 0
    call_import(3)                       # call [ExitProcess]
    emit(b"\xeb\xfe")                  # jmp $; unreachable if ExitProcess works
    failure = len(code)
    emit(b"\x6a\x01")                  # failure: push 1
    call_import(3)                       # call [ExitProcess]
    emit(b"\xeb\xfe")                  # jmp $; unreachable if ExitProcess works
    displacement = failure - (branch_displacement + 1)
    if not -128 <= displacement <= 127:
        raise ValueError("The conditional branch no longer fits a signed rel8")
    code[branch_displacement] = displacement & 0xFF
    return bytes(code)


def make_fixture() -> bytes:
    image = bytearray(4 * FILE_ALIGNMENT)
    struct.pack_into("<H", image, 0, 0x5A4D)  # MZ
    struct.pack_into("<I", image, 0x3C, 0x80)  # e_lfanew
    image[0x80:0x84] = b"PE\0\0"
    # i386, three sections, zero timestamp, PE32 optional header, executable,
    # 32-bit machine, relocations stripped (this fixture has a fixed base).
    struct.pack_into("<HHIIIHH", image, 0x84, 0x14C, 3, 0, 0, 0, 224, 0x103)
    optional = 0x98
    struct.pack_into("<H", image, optional, 0x10B)
    struct.pack_into("<III", image, optional + 4, FILE_ALIGNMENT, 2 * FILE_ALIGNMENT, 0)
    struct.pack_into("<IIII", image, optional + 16, TEXT_RVA, TEXT_RVA, IDATA_RVA, IMAGE_BASE)
    struct.pack_into("<II", image, optional + 32, 0x1000, FILE_ALIGNMENT)
    struct.pack_into("<HH", image, optional + 40, 6, 0)  # OS version
    struct.pack_into("<HH", image, optional + 48, 6, 0)  # Subsystem version
    struct.pack_into("<II", image, optional + 56, IMAGE_SIZE, FILE_ALIGNMENT)
    struct.pack_into("<HH", image, optional + 68, 3, 0x100)  # Console, NX compatible
    struct.pack_into("<IIII", image, optional + 72, 0x100000, 0x1000, 0x100000, 0x1000)
    struct.pack_into("<I", image, optional + 92, 16)  # NumberOfRvaAndSizes
    # Import descriptor and explicit all-zero terminator: 2 * 20 bytes.
    struct.pack_into("<II", image, optional + 96 + 1 * 8, IDATA_RVA, 40)
    # Four 32-bit IAT entries plus the terminating zero.
    struct.pack_into("<II", image, optional + 96 + 12 * 8, IDATA_RVA + IAT_OFFSET, 20)

    code = make_code()
    sections = (
        (b".text", TEXT_RVA, FILE_ALIGNMENT, len(code), 0x60000020),  # R/X code
        (b".idata", IDATA_RVA, 2 * FILE_ALIGNMENT, 0x100, 0xC0000040),  # R/W data
        (b".rdata", RDATA_RVA, 3 * FILE_ALIGNMENT, len(MESSAGE), 0x40000040),  # R data
    )
    for index, (name, rva, raw, virtual_size, flags) in enumerate(sections):
        section = optional + 224 + index * 40
        image[section:section + 8] = name.ljust(8, b"\0")
        struct.pack_into("<IIII", image, section + 8, virtual_size, rva, FILE_ALIGNMENT, raw)
        struct.pack_into("<I", image, section + 36, flags)
    image[FILE_ALIGNMENT:FILE_ALIGNMENT + len(code)] = code
    idata = 2 * FILE_ALIGNMENT
    struct.pack_into("<IIIII", image, idata, IDATA_RVA + LOOKUP_OFFSET, 0, 0,
                     IDATA_RVA + DLL_OFFSET, IDATA_RVA + IAT_OFFSET)
    dll = b"KERNEL32.dll\0"
    image[idata + DLL_OFFSET:idata + DLL_OFFSET + len(dll)] = dll
    for index, (name, name_offset) in enumerate(IMPORTS):
        name_rva = IDATA_RVA + name_offset
        struct.pack_into("<I", image, idata + LOOKUP_OFFSET + index * 4, name_rva)
        struct.pack_into("<I", image, idata + IAT_OFFSET + index * 4, name_rva)
        encoded = b"\0\0" + name.encode("ascii") + b"\0"  # hint 0 + ASCII name
        image[idata + name_offset:idata + name_offset + len(encoded)] = encoded
    image[3 * FILE_ALIGNMENT:3 * FILE_ALIGNMENT + len(MESSAGE)] = MESSAGE
    return bytes(image)


def cpp_source(data: bytes) -> str:
    lines = [
        "// Generated by samples/make_x86_win32.py. Self-authored diagnostic bytes.",
        '// Fixed-base PE32/i386; no CRT, TLS, relocations or third-party code.',
        '#include "X86Win32Fixture.h"',
        '',
        'namespace xcdm::samples {',
        'const std::vector<std::uint8_t>& X86Win32PeBytes() noexcept {',
        '    static const std::vector<std::uint8_t> bytes{',
    ]
    for start in range(0, len(data), 16):
        lines.append('        ' + ', '.join(f'0x{value:02x}' for value in data[start:start + 16]) + ',')
    lines.extend(['    };', '    return bytes;', '}', '} // namespace xcdm::samples', ''])
    return '\n'.join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpp-output', type=Path,
                        default=Path(__file__).resolve().with_name('X86Win32Fixture.cpp'))
    parser.add_argument('--output', type=Path, help='Optionally write PE bytes for external inspection')
    parser.add_argument('--check', action='store_true', help='Compare generated C++ without writing files')
    args = parser.parse_args()
    if args.check and args.output:
        parser.error('--check cannot be combined with --output')
    data = make_fixture()
    source = cpp_source(data)
    if args.check:
        if not args.cpp_output.is_file() or args.cpp_output.read_text(encoding='utf-8') != source:
            parser.exit(1, f'Generated fixture differs from {args.cpp_output}\n')
    else:
        args.cpp_output.write_text(source, encoding='utf-8')
        if args.output:
            args.output.write_bytes(data)
    print(f'{"Verified" if args.check else "Generated"} {len(data)} PE32 bytes; SHA-256 {hashlib.sha256(data).hexdigest()}')


if __name__ == '__main__':
    main()
