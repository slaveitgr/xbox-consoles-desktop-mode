#!/usr/bin/env python3
"""Syntax-only comparison with actual, hash-pinned Wine x64 record types.

SPDX-License-Identifier: MIT
Only the trusted host C++ compiler runs. No object is linked and no Wine code
is executed. Wine headers are read from the verified source archive into a
fresh temporary include tree, never from a mutable build work tree.
"""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import tempfile


HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
LOCK = json.loads((HERE / "source-lock.json").read_text(encoding="utf-8"))
NAME = re.compile(r"^[A-Za-z_][A-Za-z_0-9]*(?:\.[A-Za-z_][A-Za-z_0-9]*)*$")


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def extract_headers(archive, destination):
    if sha256(archive) != LOCK["archive_sha256"]:
        raise ValueError("Wine source archive SHA-256 does not match the pinned commit")
    prefix = "wine-" + LOCK["commit"]
    total = 0
    count = 0
    with tarfile.open(archive, "r:gz") as source:
        for member in source:
            parts = PurePosixPath(member.name).parts
            if not parts or parts[0] != prefix or ".." in parts or member.name.startswith("/"):
                raise ValueError("Unexpected source archive path")
            if len(parts) < 2 or parts[1] != "include":
                continue
            if member.isdir():
                continue
            if not member.isfile():
                raise ValueError("Non-regular member in pinned include tree")
            count += 1
            total += member.size
            if count > 10000 or member.size > 32 * 1024 * 1024 or total > 256 * 1024 * 1024:
                raise ValueError("Header extraction budget exceeded")
            target = destination.joinpath(*parts[1:])
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.extractfile(member) as input_file, target.open("wb") as output_file:
                shutil.copyfileobj(input_file, output_file)
    hashes = {}
    for relative, expected in LOCK["header_sha256"].items():
        actual = sha256(destination / relative)
        if actual != expected:
            raise ValueError("Pinned header SHA-256 mismatch: " + relative)
        hashes[relative] = actual
    return {"files": count, "bytes": total, "pinned_headers": hashes}


def generate_tu(mapping, model_header):
    # Mapping contains field/type identifiers only, never copied Wine layouts
    # or expected offsets. The compiler calculates both sides of each check.
    lines = [
        '#include "' + model_header.as_posix() + '"',
        "#include <winternl.h>", "#include <minwinbase.h>", "#include <ntgdi.h>",
        "#include <cstddef>", "#include <type_traits>",
        'static_assert(sizeof(void*) == 8, "x64 pointers required");',
        'static_assert(sizeof(wchar_t) == 2, "Windows WCHAR width required");',
    ]
    for item in mapping["types"]:
        model, wine = item["model"], item["wine"]
        if not NAME.fullmatch(model) or not NAME.fullmatch(wine):
            raise ValueError("Invalid type identifier in layout mapping")
        for operation in ("sizeof", "alignof"):
            lines.append(f'static_assert({operation}(xcdm::guest::{model}) == {operation}(::{wine}), "{operation} {model}");')
        lines.append(f'static_assert(std::is_standard_layout<xcdm::guest::{model}>::value && std::is_standard_layout<::{wine}>::value, "standard layout {model}");')
    for item in mapping["fields"]:
        model, field, wine, wine_field = (item[k] for k in ("model", "field", "wine", "wine_field"))
        if not all(NAME.fullmatch(value) for value in (model, field, wine, wine_field)):
            raise ValueError("Invalid field identifier in layout mapping")
        lines.append(f'static_assert(offsetof(xcdm::guest::{model}, {field}) == offsetof(::{wine}, {wine_field}), "offset {model}.{field}");')
        lines.append(f'static_assert(sizeof(((xcdm::guest::{model}*)0)->{field}) == sizeof(((::{wine}*)0)->{wine_field}), "field size {model}.{field}");')
    for item in mapping["non_addressable_bitfields"]:
        model, field, wine, previous, next_field = (item[k] for k in ("model", "field", "wine", "previous", "next"))
        if not all(NAME.fullmatch(value) for value in (model, field, wine, previous, next_field)):
            raise ValueError("Invalid bitfield boundary identifier")
        size = item["storage_bytes"]
        if size not in (1, 4):
            raise ValueError("Unexpected bitfield storage width")
        lines.append(f'static_assert(offsetof(xcdm::guest::{model}, {field}) == offsetof(::{wine}, {previous}) + sizeof(((::{wine}*)0)->{previous}), "bitfield storage start {field}");')
        lines.append(f'static_assert(sizeof(((xcdm::guest::{model}*)0)->{field}) == {size}, "bitfield storage width {field}");')
        lines.append(f'static_assert(offsetof(::{wine}, {next_field}) >= offsetof(xcdm::guest::{model}, {field}) + {size}, "bitfield storage end {field}");')
    lines += [
        'static_assert(xcdm::guest::GdiMaxHandleCount == GDI_MAX_HANDLE_COUNT, "GDI handle count");',
        'static_assert(xcdm::guest::GdiSharedMemorySize == sizeof(GDI_SHARED_MEMORY), "GDI shared table size");',
    ]
    return "\n".join(lines) + "\n"


def qualify(archive, compiler, output):
    output.mkdir(parents=True, exist_ok=True)
    (output / "qualification.json").write_text(json.dumps({
        "schema": "xcdm-wine-guest-abi/1", "status": "in_progress",
        "guest_execution_admitted": False, "safety": "NOT SAFE TO EXECUTE",
    }, indent=2) + "\n", encoding="utf-8")
    mapping_path = HERE / "layout-map.json"
    mapping = json.loads(mapping_path.read_text(encoding="utf-8"))
    if mapping["source_commit"] != LOCK["commit"]:
        raise ValueError("Layout mapping/source pin mismatch")
    model_header = REPO / "runtime/GuestEnvironment.h"
    compiler_path = shutil.which(compiler)
    if not compiler_path:
        raise ValueError("Host compiler not found: " + compiler)
    compiler_path = Path(compiler_path).resolve()
    tu = generate_tu(mapping, model_header)
    source_path = output / "qualification.cpp"
    source_path.write_text(tu, encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="xcdm-guest-abi-") as temporary:
        extracted = Path(temporary)
        header_evidence = extract_headers(archive, extracted)
        argv = [str(compiler_path), "-std=c++17", "-m64", "-fshort-wchar",
                "-DWINE_UNIX_LIB", "-D_WIN64", "-fmax-errors=10",
                "-I", str(extracted / "include"), "-fsyntax-only", str(source_path.resolve())]
        with (output / "compiler.log").open("w", encoding="utf-8") as log:
            completed = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT, timeout=60)
        # Prove that this invocation actually enforces offset assertions. The
        # negative control changes one compiler-calculated field comparison by
        # one byte; it must fail for that unique assertion, not a tool error.
        first = mapping["fields"][0]
        comparison = f'offsetof(::{first["wine"]}, {first["wine_field"]})'
        poisoned = tu.replace(comparison, comparison + " + 1", 1)
        poisoned = poisoned.replace(f'"offset {first["model"]}.{first["field"]}"', '"XCDM_NEGATIVE_CONTROL_LAYOUT_MISMATCH"', 1)
        negative_path = output / "negative-control.cpp"
        negative_path.write_text(poisoned, encoding="utf-8")
        negative_argv = argv[:-1] + [str(negative_path.resolve())]
        negative_log = output / "negative-control.log"
        with negative_log.open("w", encoding="utf-8") as log:
            negative = subprocess.run(negative_argv, stdout=log, stderr=subprocess.STDOUT, timeout=60)
        negative_ok = negative.returncode != 0 and "XCDM_NEGATIVE_CONTROL_LAYOUT_MISMATCH" in negative_log.read_text(encoding="utf-8")
        version = subprocess.run([str(compiler_path), "--version"], check=True, capture_output=True, text=True, timeout=15).stdout.splitlines()[0]
    report = {
        "schema": "xcdm-wine-guest-abi/1",
        "status": "qualified_layout" if completed.returncode == 0 and negative_ok else "failed",
        "source_commit": LOCK["commit"], "source_archive_sha256": LOCK["archive_sha256"],
        "headers": header_evidence, "model_header_sha256": sha256(model_header),
        "qualification_tool_sha256": sha256(Path(__file__)),
        "layout_mapping_sha256": sha256(mapping_path), "translation_unit_sha256": sha256(source_path),
        "compiler": {"path": str(compiler_path), "sha256": sha256(compiler_path), "version": version, "argv": argv, "returncode": completed.returncode},
        "negative_control": {"one_byte_offset_mismatch_rejected": negative_ok, "returncode": negative.returncode},
        "checks": {"record_types_size_align_standard_layout": len(mapping["types"]),
                   "teb_peb_addressable_fields_offset_and_size": sum(item["model"] in ("Teb64", "Peb64") for item in mapping["fields"]),
                   "support_fields_offset_and_size": sum(item["model"] not in ("Teb64", "Peb64") for item in mapping["fields"]),
                   "peb_bitfield_storage_boundaries_only": len(mapping["non_addressable_bitfields"]),
                   "static_assertions": sum(line.startswith("static_assert(") for line in tu.splitlines())},
        "scope": "Compiler-calculated record layout only, using actual immutable Wine header declarations, not their comments.",
        "limitations": ["The three PEB bitfields are not addressable with offsetof; only surrounding storage boundaries/width are checked, not bit order.",
                        "Linux GCC with Wine's Windows-width types is not an MSVC/UWP compiler or Xbox hardware qualification.",
                        "No pointer ownership, heap/NLS behavior, loader, callback lifetime, syscall, TLS/FLS, SEH or guest execution admission is established."],
        "wine_code_executed": False, "linked": False, "guest_execution_admitted": False,
        "safety": "NOT SAFE TO EXECUTE",
    }
    (output / "qualification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-archive", type=Path, required=True)
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--output", type=Path, required=True, help="Directory for TU, compiler log and qualification.json")
    args = parser.parse_args(argv)
    try:
        report = qualify(args.source_archive, args.compiler, args.output)
    except (OSError, ValueError, KeyError, tarfile.TarError, subprocess.SubprocessError) as exc:
        if args.output.is_dir():
            (args.output / "qualification.json").write_text(json.dumps({
                "schema": "xcdm-wine-guest-abi/1", "status": "input_or_tool_error",
                "error": str(exc), "guest_execution_admitted": False,
                "safety": "NOT SAFE TO EXECUTE",
            }, indent=2) + "\n", encoding="utf-8")
        parser.exit(2, "ABI qualification input/tool failure: %s\n" % exc)
    print(json.dumps({"status": report["status"], "checks": report["checks"], "guest_execution_admitted": False}))
    return 0 if report["status"] == "qualified_layout" else 1


if __name__ == "__main__":
    raise SystemExit(main())
