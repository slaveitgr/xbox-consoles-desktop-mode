#!/usr/bin/env python3
"""Read-only Wine guest-environment source and optional PE64 audit.

SPDX-License-Identifier: MIT
Uses Python's standard library. It never downloads, builds, loads or executes
the selected source/PEs. Optional objdump disassembles a file without running it.
Lexical findings are review evidence, not a proof of compiled code or fixes.
"""
from __future__ import annotations

import argparse
from bisect import bisect_right
from collections import Counter, deque
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

DEFAULT_PIN = "1c78596625336a73dbfc035dbbcf0dbdd95b06eb"
DEFAULT_MODULES = (
    "programs/explorer", "dlls/shell32", "dlls/comctl32", "dlls/ole32",
    "dlls/shlwapi", "dlls/user32", "dlls/gdi32", "dlls/kernel32",
    "dlls/kernelbase", "dlls/ntdll", "dlls/win32u",
)
SOURCE_SUFFIXES = {".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".s", ".asm", ".inc", ".inl", ".idl", ".spec", ".def"}
MAX_PE_BYTES = 64 * 1024 * 1024
LIMITS = [
    "Source matching is lexical; preprocessor, architecture and build-option conditions are not evaluated.",
    "Comments and strings are retained and labeled. Inline/global assembly often lives inside strings; documentation may be a false positive.",
    "Comment/quote context labeling is lightweight, not a complete C/C++/assembly lexer; macro-generated tokens and unusual dialects require review.",
    "Makefile dependency expansion covers local literal IMPORTS, DELAYIMPORTS and PARENTSRC, not generated imports, configure variables, forwarders or runtime LoadLibrary closure.",
    "Unix-side files and shared headers are evidence scopes, not established PE blockers. Module tests are excluded by default.",
    "Compiler TLS macros and compiler-generated segment accesses may be absent from source matches. Dynamic TlsAlloc/FlsAlloc calls are reported separately.",
    "TLS metadata and objdump findings cover supplied binaries only. No supplied full DLL closure means full binary coverage remains unknown.",
    "Disassembly is not execution or a complete code-reachability analysis; it can interpret data as code and miss generated, packed or dynamically loaded code.",
    "Counts are evidence records, not numbers of fixes or proof that a guest TEB accessor patch is sufficient.",
]

PATTERNS = (
    ("segment_intrinsic", re.compile(r"\b__(?:read|write|inc|add)(?:gs|fs)[A-Za-z0-9_]*\b")),
    ("segment_base_intrinsic", re.compile(r"\b_+(?:read|write)(?:gs|fs)base_u(?:32|64)\b")),
    ("segment_base_builtin", re.compile(r"\b__builtin_ia32_(?:rd|wr)(?:gs|fs)base(?:32|64)\b")),
    ("segment_memory", re.compile(r"%{1,2}(?:gs|fs)\s*:|\b(?:gs|fs)\s*:\s*(?:\[|(?:0x)?[0-9a-fA-F]+)", re.I)),
    ("segment_register_instruction", re.compile(r"\b(?:swapgs|rdfsbase|rdgsbase|wrfsbase|wrgsbase|lfs|lgs)\b|%(?:gs|fs)\b|\b(?:mov|push|pop)\s+(?:[A-Za-z0-9_%]+\s*,\s*)?(?:gs|fs)\b", re.I)),
    ("segment_address_space", re.compile(r"\b__seg_(?:gs|fs)\b|\baddress_space\s*\(\s*(?:256|257)\s*\)")),
    ("nt_current_teb_reference", re.compile(r"\bNtCurrentTeb\b")),
    ("rtl_get_current_peb_reference", re.compile(r"\bRtlGetCurrentPeb\b")),
    ("nt_current_teb_definition", re.compile(r"(?:#\s*define\s+NtCurrentTeb\b|\bNtCurrentTeb\s*\(\s*(?:void)?\s*\)\s*\{|__ASM_[A-Z_]*FUNC\s*\(\s*NtCurrentTeb\b)")),
    ("compiler_tls_declaration", re.compile(r"\b__declspec\s*\(\s*thread\s*\)|\b(?:_Thread_local|thread_local|__thread)\b")),
    ("compiler_tls_macro", re.compile(r"\b(?:DECLSPEC_THREAD|DECLSPEC_TLS|_THREAD_LOCAL|__WINE_THREAD_LOCAL)\b")),
    ("static_tls_annotation", re.compile(r"\b(?:tls_model|thread_local_storage)\b|\b(?:section|allocate)\s*\(\s*[\"']\.tls(?:\$|[.\"'])|#\s*pragma\s+section\s*\(\s*[\"']\.tls", re.I)),
    ("static_tls_symbol_reference", re.compile(r"\b(?:_tls_used|_tls_index|__tls_index|_tls_start|_tls_end|__tls_get_addr|__emutls_[A-Za-z0-9_]+)\b")),
    ("thread_pointer_builtin", re.compile(r"\b__builtin_thread_pointer\b")),
    ("dynamic_tls_api_reference", re.compile(r"\b(?:TlsAlloc|TlsFree|TlsGetValue|TlsSetValue|FlsAlloc|FlsFree|FlsGetValue|FlsSetValue)\b")),
)


def parse_makefile(text: str) -> dict[str, str]:
    """Conservative local-variable expansion; does not execute make/shell code."""
    joined = re.sub(r"\\\r?\n", " ", text)
    values = {}
    for line in joined.splitlines():
        line = line.split("#", 1)[0]
        match = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(\+=|:=|\?=|=)\s*(.*)$", line)
        if not match:
            continue
        key, operation, value = match.groups()
        if operation == "+=": values[key] = (values.get(key, "") + " " + value).strip()
        elif operation != "?=" or key not in values: values[key] = value.strip()
    def expand(value: str, depth: int = 0) -> str:
        if depth >= 8: return value
        return re.sub(r"\$\(([A-Za-z_][A-Za-z0-9_]*)\)",
                      lambda match: expand(values[match[1]], depth + 1) if match[1] in values else match[0], value)
    return {key: expand(value) for key, value in values.items()}


def make_tokens(value: str) -> list[str]:
    return re.findall(r"\$\([^)]*\)|\$\{[^}]*\}|[^\s]+", value)


def relative_module(root: Path, name: str) -> str | None:
    for candidate in (root / name, root / "dlls" / name, root / "programs" / name, root / "libs" / name):
        if candidate.resolve().is_relative_to(root.resolve()) and (candidate / "Makefile.in").is_file():
            return candidate.resolve().relative_to(root.resolve()).as_posix()
    return None


def module_closure(root: Path, seeds: list[str]) -> dict:
    nodes, edges, missing, unresolved = {}, [], [], []
    work = deque()
    for seed in seeds:
        module = relative_module(root, seed)
        if module: work.append((module, "eager"))
        else: missing.append({"seed": seed, "reason": "No source Makefile.in found"})
    visited = set()
    while work:
        module, phase = work.popleft()
        if (module, phase) in visited: continue
        visited.add((module, phase))
        if module not in nodes:
            variables = parse_makefile((root / module / "Makefile.in").read_text(encoding="utf-8", errors="replace"))
            node = {"path": module, "module_target": variables.get("MODULE"), "unix_library": variables.get("UNIXLIB"),
                    "reachability": [], "listed_sources": make_tokens(variables.get("SOURCES", "")), "dependencies": []}
            nodes[module] = node
            for variable, kind in (("IMPORTS", "eager"), ("DELAYIMPORTS", "delayed"), ("PARENTSRC", "parent_source")):
                for token in make_tokens(variables.get(variable, "")):
                    if "$" in token:
                        unresolved.append({"from": module, "kind": kind, "token": token, "reason": "Unexpanded build variable/function"})
                        continue
                    if kind == "parent_source":
                        target_path = (root / module / token).resolve()
                        target = relative_module(root, target_path.relative_to(root.resolve()).as_posix()) if target_path.is_relative_to(root.resolve()) else None
                    else: target = relative_module(root, token)
                    edge = {"from": module, "kind": kind, "declared": token, "to": target}
                    edges.append(edge); node["dependencies"].append(edge)
                    if target is None:
                        unresolved.append({"from": module, "kind": kind, "token": token, "reason": "No selected source module found (may be a static/external library)"})
        node = nodes[module]
        if phase not in node["reachability"]: node["reachability"].append(phase)
        for edge in node["dependencies"]:
            if edge["to"]:
                work.append((edge["to"], "delayed" if edge["kind"] == "delayed" else phase))
    for node in nodes.values(): node["reachability"].sort()
    return {"seeds": seeds, "nodes": sorted(nodes.values(), key=lambda value: value["path"]), "edges": edges,
            "missing_seeds": missing, "unresolved_dependencies": unresolved,
            "closure_complete_proven": False, "conditionals_evaluated": False}


def token_contexts(text: str) -> tuple[list[int], list[tuple[int, int, str]]]:
    ranges, index, length = [], 0, len(text)
    while index < length:
        start = index
        if text.startswith("//", index):
            end = text.find("\n", index + 2)
            index = length if end < 0 else end
            ranges.append((start, index, "comment"))
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            index = length if end < 0 else end + 2
            ranges.append((start, index, "comment"))
        elif text[index] in ("\"", "'"):
            quote = text[index]; index += 1
            while index < length:
                if text[index] == "\\": index += 2
                elif text[index] == quote: index += 1; break
                else: index += 1
            ranges.append((start, min(index, length), "string" if quote == '"' else "character"))
        else: index += 1
    return [value[0] for value in ranges], ranges


def context_at(offset: int, starts: list[int], ranges: list[tuple[int, int, str]]) -> str:
    index = bisect_right(starts, offset) - 1
    return ranges[index][2] if index >= 0 and offset < ranges[index][1] else "code"


def scan_text(text: str, file: str, role: str, module: str | None) -> list[dict]:
    line_starts = [0] + [match.end() for match in re.finditer("\n", text)]
    lines = text.splitlines()
    starts, ranges = token_contexts(text)
    findings = []
    for category, pattern in PATTERNS:
        for match in pattern.finditer(text):
            line = bisect_right(line_starts, match.start())
            context = context_at(match.start(), starts, ranges)
            findings.append({"file": file, "line": line, "column": match.start() - line_starts[line - 1] + 1,
                             "category": category, "match": match.group(0)[:200], "context": context,
                             "source_role": role, "module": module, "architecture_applicability": "not_evaluated",
                             "access": "write" if re.match(r"_+write", match.group(0)) else "read" if re.match(r"_+read", match.group(0)) else "not_inferred",
                             "snippet": lines[line - 1].strip()[:400] if line <= len(lines) else ""})
    return findings


def scan_sources(root: Path, graph: dict, include_tests: bool = False) -> dict:
    files = {}
    skipped_tests = 0
    for node in graph["nodes"]:
        module = node["path"]
        listed = set(node["listed_sources"])
        for path in sorted((root / module).rglob("*")):
            if not path.is_file() or path.is_symlink() or path.suffix.lower() not in SOURCE_SUFFIXES: continue
            relative = path.relative_to(root).as_posix()
            if not include_tests and "tests" in path.relative_to(root / module).parts:
                skipped_tests += 1; continue
            if relative not in files:
                text = path.read_text(encoding="utf-8", errors="replace")
                unix = "unix" in path.relative_to(root / module).parts or bool(re.search(r"#\s*pragma\s+makedep\s+unix\b", text))
                local = path.relative_to(root / module).as_posix()
                role = "unix_source" if unix else "module_header" if path.suffix.lower() in (".h", ".hpp", ".inc", ".inl") else "pe_source_candidate" if local in listed else "module_source_build_membership_unknown"
                files[relative] = (text, role, module)
    include = root / "include"
    if include.is_dir():
        for path in sorted(include.rglob("*")):
            if path.is_file() and not path.is_symlink() and path.suffix.lower() in SOURCE_SUFFIXES:
                files[path.relative_to(root).as_posix()] = (path.read_text(encoding="utf-8", errors="replace"), "shared_header", None)
    findings = []
    for file, (text, role, module) in sorted(files.items()): findings.extend(scan_text(text, file, role, module))
    findings.sort(key=lambda value: (value["file"], value["line"], value["column"], value["category"]))
    return {"status": "lexical_scan_complete_for_selected_files", "file_count": len(files),
            "source_suffixes": sorted(SOURCE_SUFFIXES),
            "files": [{"file": file, "source_role": value[1], "module": value[2]} for file, value in sorted(files.items())],
            "file_counts_by_role": dict(sorted(Counter(value[1] for value in files.values()).items())),
            "tests_included": include_tests, "test_files_skipped": skipped_tests,
            "findings_by_category": dict(sorted(Counter(value["category"] for value in findings).items())),
            "findings": findings, "compiled_pe_blockers_proven": False}


class PeAuditError(ValueError): pass


def inspect_pe64(data: bytes) -> dict:
    if len(data) < 64 or len(data) > MAX_PE_BYTES or data[:2] != b"MZ": raise PeAuditError("Not a bounded DOS/PE image")
    pe = struct.unpack_from("<I", data, 60)[0]
    if pe > len(data) - 24 or data[pe:pe + 4] != b"PE\0\0": raise PeAuditError("Invalid PE signature/header offset")
    machine, count, _, _, _, optional_size, flags = struct.unpack_from("<HHIIIHH", data, pe + 4)
    optional = pe + 24
    if machine != 0x8664 or optional_size < 112 or optional_size > len(data) - optional: raise PeAuditError("Expected a complete AMD64 PE32+ optional header")
    if struct.unpack_from("<H", data, optional)[0] != 0x20b: raise PeAuditError("Not PE32+")
    base = struct.unpack_from("<Q", data, optional + 24)[0]
    image_size, headers_size = struct.unpack_from("<II", data, optional + 56)
    directories = struct.unpack_from("<I", data, optional + 108)[0]
    table = optional + optional_size
    if count == 0 or count > 96 or count * 40 > len(data) - table: raise PeAuditError("Truncated/invalid section table")
    sections = []
    for index in range(count):
        at = table + index * 40
        name = data[at:at + 8].split(b"\0", 1)[0].decode("ascii", errors="replace")
        virtual_size, rva, raw_size, offset = struct.unpack_from("<IIII", data, at + 8)
        sections.append({"name": name, "rva": rva, "virtual_size": virtual_size, "raw_size": raw_size, "file_offset": offset})
    def mapped_span(rva: int, size: int) -> tuple[int, int, str]:
        if rva < 0 or size < 0 or rva > image_size or size > image_size - rva: raise PeAuditError("TLS metadata VA/RVA leaves SizeOfImage")
        if rva < headers_size and size <= headers_size - rva:
            if rva > len(data) or size > len(data) - rva: raise PeAuditError("TLS metadata leaves file-backed headers")
            return rva, size, "file"
        for section in sections:
            offset = rva - section["rva"]
            extent = max(section["virtual_size"], section["raw_size"])
            if offset < 0 or offset > extent or size > extent - offset: continue
            available = max(0, min(size, section["raw_size"] - offset))
            file_position = section["file_offset"] + offset
            if available and (file_position > len(data) or available > len(data) - file_position): raise PeAuditError("Truncated file-backed TLS section data")
            return file_position, available, "file" if available == size else "virtual_zero_fill" if available == 0 else "file_and_virtual_zero_fill"
        raise PeAuditError("TLS metadata RVA is not mapped by headers/sections")
    def read_rva(rva: int, size: int) -> tuple[bytes, str]:
        file_position, available, storage = mapped_span(rva, size)
        return data[file_position:file_position + available] + b"\0" * (size - available), storage
    def va_rva(va: int) -> int:
        if va < base or va - base >= image_size: raise PeAuditError("TLS metadata VA is outside this image")
        return va - base
    result = {"format": "PE32+", "machine": "AMD64", "is_dll": bool(flags & 0x2000), "image_base": base,
              "sections": sections, "tls_sections": [value for value in sections if value["name"].lower().startswith(".tls")],
              "tls": {"status": "metadata_inspected", "directory_present": False}, "compiler_tls_absence_proven": False}
    if directories <= 9: return result
    if optional_size < 112 + 10 * 8: raise PeAuditError("TLS directory entry leaves optional header")
    tls_rva, tls_size = struct.unpack_from("<II", data, optional + 112 + 9 * 8)
    if tls_rva == 0 and tls_size == 0: return result
    if tls_rva == 0 or tls_size < 40: raise PeAuditError("Incomplete PE64 TLS directory")
    mapped_span(tls_rva, tls_size)
    raw, storage = read_rva(tls_rva, 40)
    start, end, index_va, callbacks_va, zero_fill, characteristics = struct.unpack("<QQQQII", raw)
    if end < start: raise PeAuditError("TLS raw-template end precedes start")
    tls = {"status": "metadata_inspected", "directory_present": True, "directory_rva": tls_rva, "directory_size": tls_size,
           "directory_storage": storage, "raw_template_start_va": start, "raw_template_end_va": end, "raw_template_size": end - start,
           "size_of_zero_fill": zero_fill, "characteristics": characteristics, "address_of_index_va": index_va,
           "address_of_callbacks_va": callbacks_va, "callbacks": []}
    if start != end:
        if end - start > MAX_PE_BYTES: raise PeAuditError("TLS raw-template size exceeds audit bound")
        mapped_span(va_rva(start), end - start)
    if index_va:
        index_rva = va_rva(index_va); value, index_storage = read_rva(index_rva, 4)
        tls.update({"address_of_index_rva": index_rva, "index_initial_value": struct.unpack("<I", value)[0], "index_storage": index_storage})
    else: tls.update({"address_of_index_rva": None, "index_initial_value": None, "index_storage": "missing"})
    if callbacks_va:
        callback_rva = va_rva(callbacks_va)
        for offset in range(256):
            value, _ = read_rva(callback_rva + offset * 8, 8)
            callback = struct.unpack("<Q", value)[0]
            if callback == 0: break
            in_image = base <= callback < base + image_size
            tls["callbacks"].append({"va": callback, "rva": callback - base if in_image else None, "in_image": in_image})
        else: raise PeAuditError("TLS callback array does not terminate within 256 entries")
    result["tls"] = tls
    return result


SEGMENT_DISASSEMBLY = re.compile(r"%(?:gs|fs)\b|\b(?:gs|fs)\s*:|\b(?:swapgs|rdfsbase|rdgsbase|wrfsbase|wrgsbase|lfs|lgs)\b|\b(?:mov|push|pop)\s+(?:[A-Za-z0-9_%]+\s*,\s*)?(?:gs|fs)\b", re.I)


def disassembly_findings(lines) -> list[dict]:
    findings = []
    for line in lines:
        if SEGMENT_DISASSEMBLY.search(line):
            address = re.match(r"\s*([0-9a-fA-F]+):", line)
            if address: findings.append({"address": "0x" + address[1], "instruction": line.strip()[:500]})
    return findings


def audit_binary(path: Path, objdump: str) -> dict:
    result = {"input": str(path), "metadata_status": "unknown", "segment_disassembly": {"status": "unknown", "finding_count": None, "findings": []}}
    try:
        size = path.stat().st_size
        if size > MAX_PE_BYTES: raise PeAuditError("Binary exceeds the 64 MiB audit bound")
        data = path.read_bytes()
        result.update(inspect_pe64(data)); result["metadata_status"] = "inspected"
        result["sha256"] = hashlib.sha256(data).hexdigest(); result["bytes"] = len(data)
    except (OSError, PeAuditError, struct.error) as error:
        result["metadata_error"] = str(error)
        return result
    inspector = shutil.which(objdump)
    if inspector is None:
        result["segment_disassembly"].update({"status": "unknown_tool_unavailable", "reason": f"Cannot find {objdump}"})
        return result
    try:
        with tempfile.TemporaryFile(mode="w+t", encoding="utf-8") as stdout, tempfile.TemporaryFile(mode="w+t", encoding="utf-8") as stderr:
            process = subprocess.run([inspector, "-d", "--no-show-raw-insn", str(path)], stdout=stdout, stderr=stderr, timeout=60)
            if process.returncode:
                stderr.seek(0)
                result["segment_disassembly"].update({"status": "unknown_disassembler_error", "reason": stderr.read(1000)})
            else:
                stdout.seek(0); findings = disassembly_findings(stdout)
                result["segment_disassembly"] = {"status": "inspected_not_exhaustive", "finding_count": len(findings), "findings": findings}
    except (OSError, subprocess.TimeoutExpired) as error:
        result["segment_disassembly"].update({"status": "unknown_disassembler_error", "reason": str(error)})
    return result


def source_identity(root: Path, pin: str) -> dict:
    identity = {"requested_pin": pin, "verification": "supplied_pin_not_independently_verified_by_scanner"}
    provenance = root.parent / "source-provenance.json"
    if provenance.is_file():
        try:
            metadata = json.loads(provenance.read_text(encoding="utf-8"))
            identity["adjacent_provenance"] = {key: metadata[key] for key in ("repository", "commit", "archive_sha256", "source_code_executed") if key in metadata}
            identity["provenance_pin_matches_requested"] = metadata.get("commit") == pin
        except (OSError, ValueError) as error: identity["provenance_error"] = str(error)
    return identity


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--source-pin", default=DEFAULT_PIN)
    parser.add_argument("--module", action="append", dest="modules", help="Seed source module path or name; defaults to the requested Explorer/shell/GUI/runtime set")
    parser.add_argument("--include-tests", action="store_true")
    parser.add_argument("--binary", type=Path, action="append", default=[])
    parser.add_argument("--objdump", default="objdump")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = args.source_root.resolve()
    if not root.is_dir(): parser.error("--source-root is not a directory")
    graph = module_closure(root, args.modules or list(DEFAULT_MODULES))
    sources = scan_sources(root, graph, args.include_tests)
    binaries = [audit_binary(path, args.objdump) for path in args.binary]
    result = {"schema": 1, "purpose": "Audit evidence before any guest TEB/header/runtime implementation",
              "source_root": str(args.source_root), "source_identity": source_identity(root, args.source_pin),
              "module_scope": graph, "source_audit": sources,
              "binary_coverage": {"status": "unknown_no_binaries_supplied" if not binaries else "partial_supplied_binaries_only",
                                  "supplied": len(binaries), "metadata_inspected": sum(value["metadata_status"] == "inspected" for value in binaries),
                                  "full_selected_dll_closure_inspected": False, "absent_binaries_are_zero_proof": False},
              "binaries": binaries, "limitations": LIMITS,
              "source_or_guest_executed": False, "runtime_structs_or_headers_modified": False}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Audited {sources['file_count']} source files / {len(graph['nodes'])} source modules; {len(sources['findings'])} lexical evidence records.")
    print(f"Binary coverage: {result['binary_coverage']['status']}; output: {args.output}")


if __name__ == "__main__":
    main()
