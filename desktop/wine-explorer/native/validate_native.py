#!/usr/bin/env python3
"""Validate the native Wine-source PE64 build and dumpbin import inventory.

SPDX-License-Identifier: MIT
Build validation only; it never loads or executes the inspected executable.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

ALLOWED_LIBRARIES = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "comctl32.dll",
    "ole32.dll", "oleaut32.dll", "shell32.dll", "shlwapi.dll",
}
REQUIRED_SYMBOLS = {
    "CoCreateInstance", "InitCommonControlsEx", "SHGetDesktopFolder",
    "CreateWindowExW", "GetMessageW", "GetCommandLineW", "HeapAlloc", "HeapFree",
}


def validate_headers(data: bytes) -> dict:
    if len(data) < 64 or len(data) > 64 * 1024 * 1024 or data[:2] != b"MZ":
        raise ValueError("Expected a bounded DOS/PE executable")
    pe = struct.unpack_from("<I", data, 60)[0]
    if pe > len(data) - 24 or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("PE signature/header leaves the file")
    machine, sections, _, _, _, optional_size, characteristics = struct.unpack_from("<HHIIIHH", data, pe + 4)
    optional = pe + 24
    if optional_size < 240 or optional_size > len(data) - optional:
        raise ValueError("Incomplete PE32+ optional header")
    magic = struct.unpack_from("<H", data, optional)[0]
    if machine != 0x8664 or magic != 0x20b or characteristics & 0x2000:
        raise ValueError("Native Wine Explorer must be an x64 PE32+ executable, not a DLL")
    entry = struct.unpack_from("<I", data, optional + 16)[0]
    directories = struct.unpack_from("<I", data, optional + 108)[0]
    if not entry or directories < 15:
        raise ValueError("Missing executable entry point or data directories")
    for index, label in ((9, "TLS/CRT startup"), (13, "delay imports"), (14, "CLR")):
        if struct.unpack_from("<II", data, optional + 112 + 8 * index) != (0, 0):
            raise ValueError(f"Unexpected {label} directory in the no-CRT native build")
    table = optional + optional_size
    if sections == 0 or sections > 96 or sections * 40 > len(data) - table:
        raise ValueError("Invalid or incomplete section table")
    for index in range(sections):
        flags = struct.unpack_from("<I", data, table + index * 40 + 36)[0]
        if flags & 0x20000000 and flags & 0x80000000:
            raise ValueError("Writable/executable section in native guest")
    return {"machine": "AMD64", "format": "PE32+", "entry_rva": entry,
            "section_count": sections, "delay_imports": False, "tls": False, "clr": False}


def parse_imports(text: str) -> list[dict]:
    imports = []
    library = None
    for line in text.splitlines():
        if line.strip().lower() == "summary":
            library = None
        match = re.fullmatch(r"\s+([A-Za-z0-9._-]+\.dll)\s*", line, re.I)
        if match:
            library = match[1].lower()
            continue
        if not library:
            continue
        match = re.fullmatch(r"\s+[0-9A-Fa-f]+\s+([^\s]+)\s*", line)
        if match:
            imports.append({"library": library, "symbol": match[1]})
            continue
        match = re.fullmatch(r"\s+\d+\s+ordinal\s+(\d+)\s*", line, re.I)
        if match:
            imports.append({"library": library, "symbol": "#" + match[1]})
    if not imports:
        raise ValueError("No import inventory was recovered from dumpbin output")
    for entry in imports:
        name = entry["symbol"].lower()
        if entry["library"] not in ALLOWED_LIBRARIES:
            raise ValueError(f"Unexpected runtime library: {entry['library']}")
        if "__wine" in name or name in ("resolvedelayloadedapi", "delayloadfailurehook"):
            raise ValueError(f"Wine/Unix or delay-loader bootstrap import: {entry['symbol']}")
    missing = REQUIRED_SYMBOLS - {entry["symbol"] for entry in imports}
    if missing:
        raise ValueError("Actual Wine/source adapter imports are missing: " + ", ".join(sorted(missing)))
    return sorted(imports, key=lambda entry: (entry["library"], entry["symbol"].lower()))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("dumpbin_report", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    data = args.binary.read_bytes()
    result = validate_headers(data)
    imports = parse_imports(args.dumpbin_report.read_text(encoding="utf-8-sig"))
    result.update({
        "schema": 1,
        "binary_name": args.binary.name,
        "binary_size": len(data),
        "binary_sha256": hashlib.sha256(data).hexdigest(),
        "imports": imports,
        "libraries": sorted({entry["library"] for entry in imports}),
        "scope": "actual pinned Wine Explorer file-window source, no-CRT native PE64 build",
        "windows_execution_tested": False,
        "physical_xbox_tested": False,
        "wine_shell_provider_included": False,
        "wine_desktop_session_included": False,
    })
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Validated native PE64 and {len(imports)} direct imports; no Wine Unix bootstrap/UCRT/delay directory")


if __name__ == "__main__":
    main()
