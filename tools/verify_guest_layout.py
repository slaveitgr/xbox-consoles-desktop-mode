#!/usr/bin/env python3
"""Check the modeled guest TEB/PEB offsets against Wine's own header.

Wine annotates every TEB/PEB field in include/winternl.h with its
win32/win64 offset, e.g. ``ULONG LastErrorValue; /* 034/0068 */``. This tool
parses the x64 column of ``struct _TEB`` and ``struct _PEB`` and compares it
with ``guest_environment_tests --dump-layout``.

    python3 tools/verify_guest_layout.py --winternl <wine>/include/winternl.h \
        --layout-exe build/guest_environment_tests

Exit status: 0 when every annotated Wine field is modeled at the same offset
and the model contains no field Wine lacks; 1 otherwise.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

OFFSET = re.compile(r"/\*\s*([0-9a-fA-F]{3}|\s*)\s*/\s*([0-9a-fA-F]{3,4})\b")
FIELD = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\])?\s*(?::\s*\d+)?\s*;")


# Wine annotates the first member of a union or bitfield word; the model
# names the whole word.
ALIASES = {
    ("Teb64", "ChpeV2CpuAreaInfo"): "DeallocationBStore",   # union
    ("Peb64", "ImageUsedLargePages"): "BitField",           # UCHAR bitfield at 0x003
    ("Peb64", "ProcessInJob"): "CrossProcessFlags",         # ULONG bitfield at 0x050
    ("Peb64", "HeapTracingEnabled"): "TracingFlags",        # ULONG bitfield at 0x378
}


def parse_struct(text: str, tag: str) -> dict[str, int]:
    start = re.search(r"typedef\s+struct\s+" + re.escape(tag) + r"\s*\{", text)
    if not start:
        raise SystemExit(f"struct {tag} not found")
    depth, i = 1, start.end()
    while depth and i < len(text):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    body = text[start.end():i - 1]
    fields: dict[str, int] = {}
    for line in body.splitlines():
        m = OFFSET.search(line)
        if not m:
            continue
        code = line[:m.start()]
        f = FIELD.search(code)
        if not f:
            continue
        fields[f.group(1)] = int(m.group(2), 16)
    return fields


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--winternl", required=True, type=Path)
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--layout-exe", type=Path, help="guest_environment_tests binary")
    group.add_argument("--layout-json", type=Path, help="saved --dump-layout output")
    args = ap.parse_args()

    text = args.winternl.read_text(encoding="utf-8", errors="replace")
    if args.layout_exe:
        out = subprocess.run([str(args.layout_exe), "--dump-layout"], check=True,
                             capture_output=True, text=True).stdout
    else:
        out = args.layout_json.read_text(encoding="utf-8")
    model = json.loads(out)

    failures = 0
    checked = 0
    for struct, tag in (("Teb64", "_TEB"), ("Peb64", "_PEB")):
        wine = parse_struct(text, tag)
        if len(wine) < 40:
            print(f"FAIL {tag}: parsed only {len(wine)} annotated fields", file=sys.stderr)
            return 1
        modeled = {k.split(".", 1)[1]: v for k, v in model.items()
                   if k.startswith(struct + ".") and "." not in k.split(".", 1)[1]}
        for name, offset in sorted(wine.items(), key=lambda kv: kv[1]):
            if name == "Tib":
                continue  # NT_TIB members are asserted in C++.
            name = ALIASES.get((struct, name), name)
            checked += 1
            if name not in modeled:
                print(f"FAIL {struct}.{name}: Wine x64 offset 0x{offset:x} is not modeled")
                failures += 1
            elif modeled[name] != offset:
                print(f"FAIL {struct}.{name}: model 0x{modeled[name]:x}, Wine 0x{offset:x}")
                failures += 1
        wine_names = {ALIASES.get((struct, n), n) for n in wine}
        for name in sorted(set(modeled) - wine_names):
            print(f"FAIL {struct}.{name}: modeled field has no Wine x64 annotation")
            failures += 1
    teb_size = model.get("sizeof.Teb64")
    peb_size = model.get("sizeof.Peb64")
    print(f"guest layout: {checked} Wine fields checked, {failures} mismatches; "
          f"sizeof TEB64=0x{teb_size:x} PEB64=0x{peb_size:x}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
