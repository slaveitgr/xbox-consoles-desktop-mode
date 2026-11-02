#!/usr/bin/env python3
"""Read Wine Explorer imports, including named Wine delay-load thunks.

SPDX-License-Identifier: MIT
Requires GNU objdump. Never loads or executes the inspected PE. Source-name
matches are candidates only, not proof that the bridge resolves or implements
an API. Wine builds without the relevant COFF names need a separate delay audit.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess


HERE = Path(__file__).resolve().parent
BRIDGE = HERE.parents[1] / "external/DesktopMode_uwp/Win32Bridge/Bridge"
SOURCES = {
    "advapi32.dll": ["Advapi32Shims.cpp"],
    "gdi32.dll": ["Gdi32Shims.cpp"],
    "kernel32.dll": ["Kernel32Shims.cpp"],
    "kernelbase.dll": ["Kernel32Shims.cpp"],
    "ntdll.dll": ["Kernel32Shims.cpp"],
    "msvcrt.dll": ["MsvcrtShims.cpp"],
    "user32.dll": ["User32Shims.cpp", "Win32Shims.cpp"],
    "ole32.dll": ["OleShims.cpp"],
    "oleaut32.dll": ["OleShims.cpp"],
    "comctl32.dll": ["CommonControlsShims.cpp"],
    "shell32.dll": ["Shell32Shims.cpp"],
    "shlwapi.dll": ["Win32Shims.cpp"],
}


def parse_objdump_imports(private: str) -> list[dict]:
    """Recover named and ordinal ordinary imports from GNU objdump -p."""
    imports = []
    library = None
    for line in private.splitlines():
        if "The Function Table" in line or "PE File Base Relocations" in line:
            break
        match = re.search(r"DLL Name:\s*(\S+)", line)
        if match:
            library = match[1].lower()
            continue
        match = re.fullmatch(r"\s*[0-9a-f]+\s+(\d+)\s+<none>\s+<none>\s*", line, re.I)
        if library and match:
            imports.append({"library": library, "symbol": "#" + str(int(match[1])), "kind": "ordinary"})
            continue
        match = re.match(r"^\s*[0-9a-f]+\s+(?:<none>\s+)?[0-9a-f]+\s+([A-Za-z_?][^\s]*)\s*$", line)
        if library and match:
            imports.append({"library": library, "symbol": match[1], "kind": "ordinary"})
    return imports


def inspect(binary: Path, objdump: str) -> dict:
    data = binary.read_bytes()
    if len(data) > 64 * 1024 * 1024:
        raise ValueError("This audit is limited to 64 MiB PE images")
    def dump(option: str) -> str:
        return subprocess.check_output([objdump, option, str(binary)], text=True)
    private = dump("-p")
    disassembly = dump("-d")
    image_base_match = re.search(r"^ImageBase\s+([0-9a-fA-F]+)", private, re.M)
    if not image_base_match or "file format pei-x86-64" not in private:
        raise ValueError("Expected a PE32+ x64 Wine executable")
    imports = parse_objdump_imports(private)
    recovered = set()
    pending = None
    delayed = set()
    for line in disassembly.splitlines():
        # GNU objdump annotates the Wine linker thunk's LEA of __imp_<API>,
        # followed immediately by a JMP to __wine_delay_load_<library>.
        # This also sees builds whose optional-header delay directory is zero.
        match = re.search(r"\blea\s+.*?<__imp_([^>]+)>\s*$", line)
        if match:
            pending = match[1]
            continue
        match = re.search(r"\bjmp\s+.*?<__wine_delay_load_(\w+)>\s*$", line)
        if pending and match:
            library = match[1].lower() + ".dll"
            delayed.add((library, pending))
            recovered.add(library)
        pending = None
    for library, symbol in sorted(delayed):
        imports.append({"library": library, "symbol": symbol, "kind": "wine-delay"})
    candidates = {}
    for library, files in SOURCES.items():
        text = "\n".join((BRIDGE / file).read_text(encoding="utf-8-sig") for file in files)
        candidates[library] = {name.lower() for name in re.findall(r'L"([^"\n]*)"', text)}
    for entry in imports:
        entry["source_name_candidate"] = entry["symbol"].lower() in candidates.get(entry["library"], set())
    imports.sort(key=lambda entry: (entry["library"], entry["kind"], entry["symbol"].lower()))
    libraries = {}
    for entry in imports:
        summary = libraries.setdefault(entry["library"], {"ordinary": 0, "wine-delay": 0, "source_name_candidates": 0})
        summary[entry["kind"]] += 1
        summary["source_name_candidates"] += int(entry["source_name_candidate"])
    return {
        "schema": 1,
        "binary_name": binary.name,
        "binary_size": len(data),
        "source_name_matching_is_not_resolution_or_execution": True,
        "delay_inventory_note": "Named Wine x64 linker thunks recovered; stripped/dynamic imports may need further inspection. Ordinal-backed thunks retain their COFF symbol names.",
        "recovered_wine_delay_libraries": sorted(recovered),
        "libraries": libraries,
        "imports": imports,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--objdump", default="objdump")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = json.dumps(inspect(args.binary, args.objdump), indent=2) + "\n"
    if args.output:
        args.output.write_text(report, encoding="utf-8")
    else:
        print(report, end="")


if __name__ == "__main__":
    main()
