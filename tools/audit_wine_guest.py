#!/usr/bin/env python3
"""Admission audit for stock Wine PE modules running as Win32Bridge guests.

Two modes. Neither executes guest code.

source  Scan pinned Wine sources for direct TEB/PEB field use and raw
        segment/TLS access that bypasses NtCurrentTeb():

          audit_wine_guest.py source --wine-source <wine> [--modules user32 gdi32 ...]

binary  Inspect built x86_64 PE modules (e.g. the XCDM_GUEST build):

          audit_wine_guest.py binary <dir-or-files...> [--wine-source <wine>]
                                     [--wine-build-dir <build>] [--json report.json]

        Per module it reports and gates on:
          * FAIL  any gs-segment instruction in executable sections (the
                  NtCurrentTeb redirect did not cover it; it would read or
                  write the *host* TEB);
          * FAIL  an IMAGE_DIRECTORY_ENTRY_TLS directory (implicit TLS reads
                  gs:[0x58] directly; the bridge only emulates TlsAlloc-based
                  TLS);
          * FAIL  malformed unwind metadata or undecodable executable bytes
                  (the static check is inconclusive, not a proven absence);
          * FAIL  with --wine-source: the module's sources use __TRY but no
                  unwind handler resolves to __C_specific_handler. Compiler
                  SEH is then unconfirmed: the unpreprocessed source count may
                  include code excluded from this target. It does not prove
                  that the built module uses setjmp-based SEH;
          * INFO  ntdll!NtCurrentTeb import (redirect applied), imports by
                  DLL (the size of the ntdll/win32u seam), WINE_REGISTRY
                  resources (COM/registry data for tools/wine_registry_seed.py).

        GS inspection is a sequential linear sweep of file-backed executable
        sections, not control-flow recovery. Embedded data and alternate entry
        boundaries can make it inconclusive; a clean result does not prove all
        reachable instructions safe and never authorizes guest execution.

        --wine-build-dir checks the build configuration itself: PE compiler
        is clang >= 19 in MSVC mode with -fasync-exceptions (compiler SEH)
        and XCDM_GUEST defined for PE modules.

Exit status: 0 if no FAIL, 1 otherwise, 2 on usage/input errors.
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

SEAM_DLLS = {"ntdll.dll", "win32u.dll"}

# ---------------------------------------------------------------- source mode

TEB_FIELD = re.compile(r"NtCurrentTeb\s*\(\s*\)\s*->\s*((?:Peb\s*->\s*)?[A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)?)")
TEB_VAR = re.compile(r"\b(TEB|PEB)\s*\*\s*(\w+)\s*=\s*(NtCurrentTeb\s*\(\s*\)(\s*->\s*Peb)?)")
RAW = re.compile(r"%%?gs:|__readgs|__writegs|__declspec\s*\(\s*thread\s*\)|\b__thread\b|\bthread_local\b")
TRY = re.compile(r"\b__TRY\b")


def source_files(base: Path):
    for root, dirs, files in os.walk(base):
        dirs[:] = [d for d in dirs if d not in ("tests", "unix")]
        for name in files:
            if name.endswith((".c", ".h", ".S", ".s")) and not name.startswith("unix"):
                yield Path(root, name)


def module_source_dir(wine: Path, module: str) -> Path | None:
    stem = module.lower()
    for suffix in (".dll", ".exe", ".drv"):
        if stem.endswith(suffix):
            stem = stem[: -len(suffix)]
    for candidate in (wine / "dlls" / stem, wine / "programs" / stem):
        if candidate.is_dir():
            return candidate
    return None


def audit_source_module(base: Path) -> dict:
    fields: collections.Counter = collections.Counter()
    raw: list[str] = []
    tries = 0
    for path in source_files(base):
        text = path.read_text(encoding="utf-8", errors="ignore")
        tries += len(TRY.findall(text))
        for m in TEB_FIELD.finditer(text):
            fields[re.sub(r"\s", "", m.group(1))] += 1
        for m in TEB_VAR.finditer(text):
            prefix = "Peb->" if (m.group(4) or m.group(1) == "PEB") else ""
            for m2 in re.finditer(r"\b" + re.escape(m.group(2)) + r"\s*->\s*(\w+)", text):
                fields[prefix + m2.group(1)] += 1
        for number, line in enumerate(text.splitlines(), 1):
            if RAW.search(line):
                raw.append(f"{path}:{number}: {line.strip()[:120]}")
    return {"fields": dict(fields.most_common()), "raw": raw, "try_blocks": tries}


def run_source(args) -> int:
    wine = Path(args.wine_source)
    if not (wine / "include" / "winternl.h").is_file():
        print(f"error: {wine} is not a Wine source tree", file=sys.stderr)
        return 2
    modules = args.modules or sorted(p.name for p in (wine / "dlls").iterdir() if p.is_dir())
    report = {}
    failures = 0
    for module in modules:
        base = module_source_dir(wine, module)
        if not base:
            print(f"warning: no source for {module}", file=sys.stderr)
            continue
        result = audit_source_module(base)
        report[module] = result
        if result["fields"] or result["raw"]:
            top = ", ".join(f"{k}x{v}" for k, v in list(result["fields"].items())[:12])
            print(f"{module}: {top}")
        for line in result["raw"]:
            print(f"  RAW {line}")
            failures += 1
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(f"source audit: {len(report)} modules, {failures} raw segment/TLS sites")
    return 1 if failures else 0

# ---------------------------------------------------------------- binary mode


def load_pe(path: Path):
    import pefile  # noqa: PLC0415
    pe = pefile.PE(str(path), fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_TLS"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"],
    ])
    return pe


def imports_of(pe) -> tuple[dict[str, list[str]], dict[int, str]]:
    by_dll: dict[str, list[str]] = collections.defaultdict(list)
    iat: dict[int, str] = {}
    for attr in ("DIRECTORY_ENTRY_IMPORT", "DIRECTORY_ENTRY_DELAY_IMPORT"):
        for entry in getattr(pe, attr, []) or []:
            dll = entry.dll.decode("ascii", "replace").lower()
            for imp in entry.imports:
                name = imp.name.decode("ascii", "replace") if imp.name else f"#{imp.ordinal}"
                by_dll[dll].append(name)
                if imp.address:
                    iat[imp.address - pe.OPTIONAL_HEADER.ImageBase] = f"{dll}!{name}"
    return by_dll, iat


def read_rva(pe, rva: int, size: int) -> bytes:
    """Read only fully file-backed, mapped section data; never accept truncation."""
    if rva < 0 or size < 0 or rva + size > 0x100000000:
        raise ValueError(f"invalid RVA range 0x{rva:x}+0x{size:x}")
    for section in pe.sections:
        start = section.VirtualAddress
        mapped = min(section.SizeOfRawData, section.Misc_VirtualSize or section.SizeOfRawData)
        if start <= rva and rva + size <= start + mapped:
            data = pe.get_data(rva, size)
            if len(data) == size:
                return data
            break
    raise ValueError(f"unmapped or truncated RVA range 0x{rva:x}+0x{size:x}")


def unwind_handlers(pe, iat: dict[int, str]) -> tuple[collections.Counter, list[str]]:
    """Resolve imported/local exported handlers, validating bounded unwind chains."""
    handlers: collections.Counter = collections.Counter()
    problems: list[str] = []
    directory = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]  # IMAGE_DIRECTORY_ENTRY_EXCEPTION
    if not directory.VirtualAddress and not directory.Size:
        return handlers, problems
    if not directory.VirtualAddress or not directory.Size or directory.Size % 12:
        return handlers, ["malformed exception directory address/size"]
    exports: dict[int, list[str]] = collections.defaultdict(list)
    for symbol in getattr(getattr(pe, "DIRECTORY_ENTRY_EXPORT", None), "symbols", []) or []:
        if symbol.name and not symbol.forwarder:
            exports[symbol.address].append(symbol.name.decode("ascii", "replace"))
    seen: dict[int, str] = {}

    def handler_name(rva: int) -> str:
        if rva in seen:
            return seen[rva]
        read_rva(pe, rva, 1)  # an export name alone does not validate its RVA
        name = iat.get(rva, f"local:0x{rva:x}")
        names = exports.get(rva, [])
        if names:
            # Aliases can share an RVA; do not hide the relevant SEH export.
            exported = "__C_specific_handler" if "__C_specific_handler" in names else sorted(names)[0]
            name = f"local-export!{exported}"
        else:
            code = read_rva(pe, rva, 6)
            if code[:2] == b"\xff\x25":  # jmp qword ptr [rip+disp32] import thunk
                slot = rva + 6 + struct.unpack("<i", code[2:6])[0]
                name = iat.get(slot, name)
        seen[rva] = name
        return name

    def visit(begin: int, end: int, unwind: int, chain: set[int]) -> str | None:
        if not 0 < begin < end <= pe.OPTIONAL_HEADER.SizeOfImage or not unwind:
            raise ValueError("invalid runtime-function range or missing unwind RVA")
        if unwind in chain or len(chain) >= 64:
            raise ValueError("cyclic or excessive chained unwind metadata")
        header = read_rva(pe, unwind, 4)
        version, flags, count = header[0] & 7, header[0] >> 3, header[2]
        if version not in (1, 2) or flags & ~7 or (flags & 4 and flags & 3):
            raise ValueError("invalid unwind version/flags")
        aligned = 4 + ((count + 1) & ~1) * 2
        read_rva(pe, unwind, aligned)  # includes UNWIND_CODE array and alignment
        if flags & 4:  # UNW_FLAG_CHAININFO: trailing RUNTIME_FUNCTION
            child = struct.unpack("<III", read_rva(pe, unwind + aligned, 12))
            return visit(*child, chain | {unwind})
        if flags & 3:  # UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER
            handler = struct.unpack("<I", read_rva(pe, unwind + aligned, 4))[0]
            if not handler:
                raise ValueError("zero exception-handler RVA")
            return handler_name(handler)
        return None

    try:
        data = read_rva(pe, directory.VirtualAddress, directory.Size)
    except ValueError as exc:
        return handlers, [f"malformed exception directory: {exc}"]
    for offset in range(0, len(data), 12):
        begin, end, unwind = struct.unpack_from("<III", data, offset)
        if not begin and not end and not unwind:
            continue
        try:
            name = visit(begin, end, unwind, set())
            if name:
                handlers[name] += 1
        except (ValueError, struct.error) as exc:
            problems.append(f"unwind entry +0x{offset:x}: {exc}")
    return handlers, problems


def gs_sites(pe, limit: int = 20) -> tuple[list[str], dict]:
    import capstone  # noqa: PLC0415
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.skipdata = True
    sites: list[str] = []
    scan = {
        "method": "sequential executable-section linear sweep",
        "limitations": "No control-flow recovery; embedded data and alternate instruction boundaries "
                       "are not distinguished. A clean scan is not proof that all reachable code is safe.",
        "decoded_instructions": 0, "gs_instruction_count": 0,
        "unknown_byte_count": 0, "unknown_ranges": [],
    }

    def unknown(rva: int, size: int) -> None:
        scan["unknown_byte_count"] += size
        if len(scan["unknown_ranges"]) < limit:
            scan["unknown_ranges"].append(f"0x{rva:x}+0x{size:x}")

    for section in pe.sections:
        if not section.Characteristics & 0x20000000:  # IMAGE_SCN_MEM_EXECUTE
            continue
        virtual = section.Misc_VirtualSize or section.SizeOfRawData
        expected = min(virtual, section.SizeOfRawData)
        code = section.get_data()[:expected]  # exclude file-alignment padding
        base = section.VirtualAddress
        if len(code) < expected:
            unknown(base + len(code), expected - len(code))
        if virtual > section.SizeOfRawData:
            unknown(base + section.SizeOfRawData, virtual - section.SizeOfRawData)
        # Decode each instruction once, in order. A 0x65 in a displacement or
        # immediate is part of its containing instruction, never a new start.
        covered = 0
        for address, size, mnemonic, operands in md.disasm_lite(code, base):
            covered = address - base + size
            if mnemonic == ".byte":  # capstone skip-data, not an instruction
                unknown(address, size)
                continue
            scan["decoded_instructions"] += 1
            if "gs:" in operands:
                scan["gs_instruction_count"] += 1
                if len(sites) < limit:
                    sites.append(f"0x{address:x}: {mnemonic} {operands}")
        if covered < len(code):
            unknown(base + covered, len(code) - covered)
    return sites, scan


def resource_types(pe) -> dict[str, int]:
    out: dict[str, int] = {}
    root = getattr(pe, "DIRECTORY_ENTRY_RESOURCE", None)
    for entry in getattr(root, "entries", []) or []:
        name = str(entry.name) if entry.name is not None else str(entry.id)
        out[name] = sum(len(getattr(e.directory, "entries", [])) if hasattr(e, "directory") else 1
                        for e in entry.directory.entries)
    return out


def check_build_dir(build: Path) -> list[str]:
    problems: list[str] = []
    makefile = build / "Makefile"
    if not makefile.is_file():
        return [f"{makefile} not found"]
    text = makefile.read_text(encoding="utf-8", errors="ignore")
    var = lambda n: (re.search(r"^" + n + r"\s*=\s*(.*)$", text, re.M) or [None, ""])[1]  # noqa: E731
    cc = var("x86_64_CC")
    flags = " ".join(var(n) for n in ("x86_64_CFLAGS", "x86_64_EXTRACFLAGS", "CROSSCFLAGS"))
    if "clang" not in cc:
        problems.append(f"PE compiler is '{cc}', not clang: Wine __TRY stays setjmp-based on x64")
    else:
        try:
            out = subprocess.run(cc.split() + ["--version"], capture_output=True, text=True, timeout=30).stdout
            m = re.search(r"clang version (\d+)", out)
            if not m or int(m.group(1)) < 19:
                problems.append(f"clang >= 19 required for USE_COMPILER_EXCEPTIONS on x86_64; got: {out.splitlines()[:1]}")
        except Exception as exc:  # noqa: BLE001
            problems.append(f"could not run {cc}: {exc}")
        if "windows" not in cc and "msvc" not in cc:
            problems.append(f"PE compiler '{cc}' is not an MSVC-mode target (x86_64-windows): _MSC_VER unset")
    if "-fasync-exceptions" not in flags:
        problems.append("-fasync-exceptions missing from x86_64 PE flags")
    if "-DXCDM_GUEST" not in flags:
        problems.append("-DXCDM_GUEST missing from x86_64 PE flags (NtCurrentTeb redirect not compiled in)")
    return problems


def run_binary(args) -> int:
    paths: list[Path] = []
    for item in args.inputs:
        p = Path(item)
        if p.is_dir():
            paths += sorted(q for q in p.rglob("*") if q.suffix.lower() in (".dll", ".exe", ".drv") and q.is_file())
        elif p.is_file():
            paths.append(p)
        else:
            print(f"error: {p} not found", file=sys.stderr)
            return 2
    if not paths:
        print("error: no PE modules found", file=sys.stderr)
        return 2
    wine = Path(args.wine_source) if args.wine_source else None
    report: dict = {"modules": {}, "build": None}
    failures = 0

    if args.wine_build_dir:
        problems = check_build_dir(Path(args.wine_build_dir))
        report["build"] = problems
        for problem in problems:
            print(f"FAIL build: {problem}")
        failures += len(problems)

    seam: dict[str, set[str]] = collections.defaultdict(set)
    for path in paths:
        try:
            pe = load_pe(path)
        except Exception as exc:  # noqa: BLE001
            print(f"FAIL {path.name}: not a PE image ({exc})")
            failures += 1
            continue
        if pe.FILE_HEADER.Machine != 0x8664:
            print(f"FAIL {path.name}: machine 0x{pe.FILE_HEADER.Machine:x} is not x86_64")
            failures += 1
            continue
        by_dll, iat = imports_of(pe)
        handlers, unwind_problems = unwind_handlers(pe, iat)
        sites, code_scan = gs_sites(pe)
        tls = bool(getattr(pe, "DIRECTORY_ENTRY_TLS", None))
        resources = resource_types(pe)
        module = path.name.lower()
        entry = {
            "gs_sites": sites,
            "code_scan": code_scan,
            "unwind_metadata_problems": unwind_problems,
            "tls_directory": tls,
            "imports_ntcurrentteb": "NtCurrentTeb" in by_dll.get("ntdll.dll", []),
            "unwind_handlers": dict(handlers),
            "imports": {k: sorted(set(v)) for k, v in sorted(by_dll.items())},
            "wine_registry_resources": resources.get("WINE_REGISTRY", 0),
        }
        problems = [f"malformed unwind metadata: {problem}" for problem in unwind_problems]
        if sites:
            problems.append(f"{code_scan['gs_instruction_count']} gs-segment instruction(s), first {sites[0]}")
        if code_scan["unknown_byte_count"]:
            problems.append(f"{code_scan['unknown_byte_count']} executable byte(s) could not be decoded "
                            "or read; GS scan is inconclusive")
        if tls:
            problems.append("TLS directory present (implicit TLS is not supported for guests)")
        if wine:
            base = module_source_dir(wine, module)
            if base:
                tries = audit_source_module(base)["try_blocks"]
                entry["source_try_blocks"] = tries
                entry["source_try_scope"] = "unpreprocessed sources; may include target-excluded code"
                native = sum(n for name, n in handlers.items() if name.endswith("!__C_specific_handler"))
                if tries and not native:
                    entry["compiler_seh_assessment"] = "unconfirmed"
                    problems.append(f"{tries} __TRY block(s) in unpreprocessed source but no "
                                    "__C_specific_handler unwind handler: compiler SEH is unconfirmed; "
                                    "this does not establish setjmp-based SEH in the built image")
                else:
                    entry["compiler_seh_assessment"] = "handler-present" if native else "no-source-try"
        entry["problems"] = problems
        report["modules"][module] = entry
        for dll, names in by_dll.items():
            if dll in SEAM_DLLS:
                seam[dll].update(names)
        status = "FAIL" if problems else "ok"
        print(f"{status:4} {module}: redirect={'yes' if entry['imports_ntcurrentteb'] else 'no'} "
              f"seh_handlers={sum(handlers.values())} registry_res={entry['wine_registry_resources']}")
        for problem in problems:
            print(f"     - {problem}")
        failures += len(problems)

    report["seam"] = {dll: sorted(names) for dll, names in sorted(seam.items())}
    for dll, names in report["seam"].items():
        print(f"seam {dll}: {len(names)} distinct imports")
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(f"binary audit: {len(report['modules'])} modules, {failures} failure(s)")
    return 1 if failures else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)
    s = sub.add_parser("source")
    s.add_argument("--wine-source", required=True)
    s.add_argument("--modules", nargs="*")
    s.add_argument("--json")
    b = sub.add_parser("binary")
    b.add_argument("inputs", nargs="+")
    b.add_argument("--wine-source")
    b.add_argument("--wine-build-dir")
    b.add_argument("--json")
    args = ap.parse_args()
    return run_source(args) if args.mode == "source" else run_binary(args)


if __name__ == "__main__":
    sys.exit(main())
