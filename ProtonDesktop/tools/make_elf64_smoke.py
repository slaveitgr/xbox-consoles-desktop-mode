#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate our static Linux x86-64 diagnostic without an assembler.

Only self-authored instructions/data are emitted. The fixture must be passed to
Boxedwine64's ELF loader, never directly executed as a host application.
"""
from pathlib import Path
import argparse
import struct

BASE = 0x400000
ENTRY_OFFSET = 0x1000
SCRATCH = BASE + 0x2000
MESSAGE = b'ProtonDesktop guest-x64 interpreter: PASS\n'


def make_elf(negative=False):
    code = bytearray()
    labels = {}
    relocations = []

    def emit(data):
        code.extend(bytes.fromhex(data))

    def rel(label, size):
        relocations.append((len(code), size, label))
        code.extend(b'\0' * size)

    def jump_failure():
        emit('75')  # JNE rel8
        rel('failure', 1)

    emit('b8 07 00 00 00')            # mov eax,7
    emit('48 6b c0 06')               # imul rax,rax,6
    emit('48 83 f8 ' + ('2b' if negative else '2a'))
    jump_failure()
    emit('48 8d 1d')                  # lea rbx,[rip+scratch]
    rel('scratch', 4)
    emit('48 89 03 31 c0 4c 8b 03')   # store rax; clear eax; load r8
    emit('49 83 f8 2a')               # cmp r8,42
    jump_failure()
    emit('b8 01 00 00 00 bf 01 00 00 00')  # write syscall, stdout
    emit('48 8d 35')                  # lea rsi,[rip+message]
    rel('message', 4)
    emit('ba')
    code.extend(struct.pack('<I', len(MESSAGE)))
    emit('0f 05 48 83 f8')            # syscall; cmp rax,message length
    code.append(len(MESSAGE))
    jump_failure()
    emit('b8 3c 00 00 00 31 ff 0f 05')  # exit(0)
    labels['failure'] = BASE + ENTRY_OFFSET + len(code)
    emit('b8 3c 00 00 00 bf 09 00 00 00 0f 05')  # exit(9)
    labels['message'] = BASE + ENTRY_OFFSET + len(code)
    labels['scratch'] = SCRATCH
    code.extend(MESSAGE)
    for offset, size, label in relocations:
        displacement = labels[label] - (BASE + ENTRY_OFFSET + offset + size)
        code[offset:offset + size] = displacement.to_bytes(size, 'little', signed=True)
    if ENTRY_OFFSET + len(code) >= 0x2000:
        raise ValueError('Diagnostic exceeded its code segment')

    ident = b'\x7fELF\x02\x01\x01' + b'\0' * 9
    header = struct.pack('<16sHHIQQQIHHHHHH', ident, 2, 62, 1,
                         BASE + ENTRY_OFFSET, 64, 0, 0, 64, 56, 2, 0, 0, 0)
    code_size = ENTRY_OFFSET + len(code)
    # Executable code/constant data and a separate writable scratch page.
    segments = struct.pack('<IIQQQQQQ', 1, 5, 0, BASE, BASE, code_size, code_size, 0x1000)
    segments += struct.pack('<IIQQQQQQ', 1, 6, 0x2000, SCRATCH, SCRATCH, 8, 0x1000, 0x1000)
    image = bytearray(0x2008)
    image[:64] = header
    image[64:64 + len(segments)] = segments
    image[ENTRY_OFFSET:code_size] = code
    return bytes(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for name, negative in [('guest64-smoke.elf', False), ('guest64-negative.elf', True)]:
        path = args.output_dir / name
        path.write_bytes(make_elf(negative))
        print(path)


if __name__ == '__main__':
    main()
