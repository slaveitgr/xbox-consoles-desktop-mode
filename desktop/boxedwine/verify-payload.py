#!/usr/bin/env python3
"""Inspect a built Wasm payload and its hashes without executing its code."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def leb(data: bytes, position: int, limit: int) -> tuple[int, int]:
    value = 0
    for shift in range(0, 35, 7):
        if position >= limit:
            raise ValueError("Truncated unsigned LEB")
        byte = data[position]
        position += 1
        value |= (byte & 127) << shift
        if not byte & 128:
            if value > 0xFFFFFFFF:
                raise ValueError("Oversized unsigned LEB")
            return value, position
    raise ValueError("Unterminated unsigned LEB")


def inspect_wasm(data: bytes) -> dict:
    if data[:8] != b"\0asm\x01\0\0\0":
        raise ValueError("Invalid Wasm header")
    position, memories = 8, []
    while position < len(data):
        kind = data[position]
        size, body = leb(data, position + 1, len(data))
        end = body + size
        if end > len(data):
            raise ValueError("Truncated Wasm section")
        if kind == 5:
            count, cursor = leb(data, body, end)
            if count != 1 or memories:
                raise ValueError("Expected one Wasm memory")
            flags, cursor = leb(data, cursor, end)
            if flags != 1:
                raise ValueError("Wasm memory must be bounded, unshared, 32-bit")
            minimum, cursor = leb(data, cursor, end)
            maximum, cursor = leb(data, cursor, end)
            if cursor != end or not 1 <= minimum <= maximum <= 16384:
                raise ValueError("Wasm memory limits exceed the 1 GiB interpreter profile")
            memories.append({"minimum_pages": minimum, "maximum_pages": maximum, "shared": False})
        position = end
    if len(memories) != 1:
        raise ValueError("Missing internal Wasm memory")
    return memories[0]


def verify(directory: Path) -> dict:
    info = json.loads((directory / "build-info.json").read_text())
    if info["engine"]["cpu"] != "single-threaded interpreter" or info["engine"]["target"] != "release":
        raise ValueError("Unexpected runtime profile")
    for name, expected in info["files"].items():
        path = (directory / name).resolve()
        if directory.resolve() not in path.parents:
            raise ValueError("Manifest path escapes payload")
        if path.stat().st_size != expected["size"] or hashlib.sha256(path.read_bytes()).hexdigest() != expected["sha256"]:
            raise ValueError("Payload hash mismatch: " + name)
    return inspect_wasm((directory / "boxedwine.wasm").read_bytes())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    options = parser.parse_args()
    print(json.dumps({"static_verification": "passed", "guest_executed": False,
                      "memory": verify(options.directory)}, indent=2))


if __name__ == "__main__":
    main()
