#!/usr/bin/env python3
"""Fixture tests for tools/audit_wine_guest.py.

Builds tiny x86_64 PE DLLs with clang/lld-link (MSVC mode) and mingw-w64,
then checks the audit's verdicts. Skips (exit 77, CTest SKIP_RETURN_CODE)
when the cross toolchain or pefile/capstone is unavailable.
"""
from __future__ import annotations

import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "audit_wine_guest.py"
SKIP = 77

REDIRECTED = r"""
__declspec(dllimport) void * __stdcall NtCurrentTeb(void);
void __cdecl helper(void);
__declspec(dllexport) unsigned long last_error(void) { return *(unsigned long *)((char *)NtCurrentTeb() + 0x68); }
__declspec(dllexport) int guarded(volatile int *p) {
    int r = 0;
    __try { r = *p; } __except (1) { r = -1; }
    return r;
}
"""
RAW_GS = r"""
unsigned __int64 __readgsqword(unsigned long);
#pragma intrinsic(__readgsqword)
__declspec(dllexport) unsigned long last_error(void) { return *(unsigned long *)(__readgsqword(0x30) + 0x68); }
"""
NO_SEH = r"""
__declspec(dllimport) void * __stdcall NtCurrentTeb(void);
__declspec(dllexport) void * peb(void) { return *(void **)((char *)NtCurrentTeb() + 0x60); }
"""
LOCAL_HANDLER = r"""
/* A linked, exported handler body is enough for this metadata-only fixture. */
__declspec(dllexport) int __C_specific_handler(void *a, void *b, void *c, void *d) { return 0; }
"""
MINGW_TLS = r"""
__declspec(dllexport) int value(void) { return 1; }
"""
NTDLL_DEF = "LIBRARY ntdll.dll\nEXPORTS\n  NtCurrentTeb\n  __C_specific_handler\n"


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)


def build_msvc(work: Path, name: str, source: str, ntdll_lib: Path) -> Path:
    c = work / f"{name}.c"
    c.write_text(source)
    obj = work / f"{name}.obj"
    run(["clang", "--target=x86_64-pc-windows-msvc", "-O1", "-fms-extensions", "-fasync-exceptions", "-c", str(c), "-o", str(obj)])
    dll = work / f"{name}.dll"
    run(["lld-link", "/dll", "/noentry", "/nodefaultlib", f"/out:{dll}", str(obj), str(ntdll_lib)])
    return dll


def build_assembly(work: Path, name: str, text: str, exports=("entry",)) -> Path:
    source = work / f"{name}.s"
    source.write_text(text)
    obj = work / f"{name}.obj"
    run(["clang", "--target=x86_64-pc-windows-msvc", "-c", str(source), "-o", str(obj)])
    dll = work / f"{name}.dll"
    run(["lld-link", "/dll", "/noentry", "/nodefaultlib", f"/out:{dll}",
         *[f"/export:{export}" for export in exports], str(obj)])
    return dll


def code_fixture(work: Path, name: str, body: str) -> Path:
    return build_assembly(work, name, ".text\n.globl entry\nentry:\n" + body + "\n")


def mutate_unwind(work: Path, original: Path, name: str, kind: str) -> Path:
    import pefile  # noqa: PLC0415
    pe = pefile.PE(str(original))
    directory = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    entry = pe.get_offset_from_rva(directory.VirtualAddress)
    begin, end, unwind = struct.unpack_from("<III", pe.__data__, entry)
    data = bytearray(pe.__data__)
    if kind == "directory-size":
        struct.pack_into("<I", data, directory.get_field_absolute_offset("Size"), directory.Size - 1)
    elif kind == "truncated":
        struct.pack_into("<I", data, entry + 8, pe.OPTIONAL_HEADER.SizeOfImage - 2)
    elif kind == "cycle":
        offset = pe.get_offset_from_rva(unwind)
        count = data[offset + 2]
        tail = offset + 4 + ((count + 1) & ~1) * 2
        data[offset] = 1 | (4 << 3)  # valid CHAININFO header, self-reference
        struct.pack_into("<III", data, tail, begin, end, unwind)
    else:
        raise ValueError(kind)
    output = work / f"{name}.dll"
    output.write_bytes(data)
    pe.close()
    return output


def audit(*args) -> tuple[int, dict, str]:
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as handle:
        report_path = Path(handle.name)
    proc = subprocess.run([sys.executable, str(TOOL), "binary", *map(str, args), "--json", str(report_path)],
                          capture_output=True, text=True)
    report = json.loads(report_path.read_text()) if report_path.stat().st_size else {}
    report_path.unlink()
    return proc.returncode, report, proc.stdout + proc.stderr


def main() -> int:
    try:
        import capstone  # noqa: F401,PLC0415
        import pefile  # noqa: F401,PLC0415
    except ImportError:
        print("SKIP: pefile/capstone not installed")
        return SKIP
    if not all(shutil.which(t) for t in ("clang", "lld-link", "llvm-dlltool")):
        print("SKIP: clang/lld-link/llvm-dlltool not available")
        return SKIP

    failures = 0
    checks = 0

    def check(cond: bool, label: str, detail: str = "") -> None:
        nonlocal failures, checks
        checks += 1
        if not cond:
            failures += 1
            print(f"FAIL: {label}\n{detail}")

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        (work / "ntdll.def").write_text(NTDLL_DEF)
        lib = work / "ntdll.lib"
        run(["llvm-dlltool", "-m", "i386:x86-64", "-d", str(work / "ntdll.def"), "-l", str(lib)])

        good = build_msvc(work, "good", REDIRECTED, lib)
        rawgs = build_msvc(work, "rawgs", RAW_GS, lib)
        noseh = build_msvc(work, "noseh", NO_SEH, lib)
        local = build_msvc(work, "local", REDIRECTED + LOCAL_HANDLER, lib)

        # These exact bytes reproduce the false positives from advapi32 and
        # Explorer. The 0x65 belongs to a LEA displacement / JNE immediate.
        displacement = code_fixture(work, "displacement", ".byte 0x48,0x8d,0x0d,0x65,0xff,0x01,0x00\nret")
        immediate = code_fixture(work, "immediate", ".byte 0x75,0x65\n.rept 101\nnop\n.endr\nret")
        undecodable = code_fixture(work, "undecodable", ".byte 0x0f")
        chained = build_assembly(work, "chained", r"""
.text
.globl entry
entry:
ret
entry_end:
.globl __C_specific_handler
__C_specific_handler:
ret
.section .xdata,"dr"
.p2align 2
unwind_target:
.byte 0x09,0,0,0
.rva __C_specific_handler
unwind_chain:
.byte 0x21,0,0,0
.rva entry,entry_end,unwind_target
.section .pdata,"dr"
.rva entry,entry_end,unwind_chain
""", exports=("entry", "__C_specific_handler"))

        # Fake Wine source tree: both modules contain __TRY.
        for module in ("good", "noseh", "local"):
            d = work / "wine" / "dlls" / module
            d.mkdir(parents=True)
            (d / f"{module}.c").write_text("void f(void) { __TRY { } __EXCEPT_PAGE_FAULT { } __ENDTRY }\n")
        (work / "wine" / "include").mkdir()

        code, report, out = audit(good, "--wine-source", work / "wine")
        entry = report.get("modules", {}).get("good.dll", {})
        check(code == 0, "redirected module with compiler SEH passes", out)
        check(entry.get("imports_ntcurrentteb") is True, "NtCurrentTeb import detected", out)
        check(entry.get("unwind_handlers", {}).get("ntdll.dll!__C_specific_handler", 0) >= 1,
              "__try handler resolved through import thunk", json.dumps(entry, indent=1))
        check(entry.get("gs_sites") == [] and entry.get("tls_directory") is False, "no gs/TLS findings", out)
        check(report.get("seam", {}).get("ntdll.dll") == ["NtCurrentTeb", "__C_specific_handler"],
              "seam import list", json.dumps(report.get("seam")))

        for fixture in (displacement, immediate):
            code, report, out = audit(fixture)
            entry = report.get("modules", {}).get(fixture.name, {})
            check(code == 0 and entry.get("gs_sites") == [],
                  f"0x65 operand byte is not an instruction start: {fixture.stem}", out)
            check(entry.get("code_scan", {}).get("unknown_byte_count") == 0,
                  f"operand fixture decodes completely: {fixture.stem}", out)

        code, report, out = audit(undecodable)
        entry = report.get("modules", {}).get("undecodable.dll", {})
        check(code == 1 and entry.get("code_scan", {}).get("unknown_byte_count") == 1,
              "undecodable executable byte is inconclusive, never a clean scan", out)
        check(any("inconclusive" in p for p in entry.get("problems", [])),
              "decode-gap admission reason reported", out)

        code, report, out = audit(local, "--wine-source", work / "wine")
        entry = report.get("modules", {}).get("local.dll", {})
        check(code == 0 and entry.get("unwind_handlers", {}).get("local-export!__C_specific_handler", 0) >= 1,
              "locally defined exported SEH handler is recognized", out)
        check(entry.get("compiler_seh_assessment") == "handler-present",
              "local handler satisfies the source-SEH check", out)

        code, report, out = audit(chained)
        entry = report.get("modules", {}).get("chained.dll", {})
        check(code == 0 and entry.get("unwind_handlers", {}).get("local-export!__C_specific_handler") == 1,
              "bounded chained unwind metadata resolves its handler", out)
        for kind in ("directory-size", "truncated", "cycle"):
            malformed = mutate_unwind(work, good, f"bad_{kind}", kind)
            code, report, out = audit(malformed)
            entry = report.get("modules", {}).get(malformed.name, {})
            check(code == 1 and bool(entry.get("unwind_metadata_problems")),
                  f"malformed unwind metadata fails closed: {kind}", out)

        code, report, out = audit(rawgs)
        entry = report.get("modules", {}).get("rawgs.dll", {})
        check(code == 1, "raw gs access fails the audit", out)
        check(any("gs:[0x30]" in s for s in entry.get("gs_sites", [])), "gs:[0x30] site reported", out)
        check(entry.get("imports_ntcurrentteb") is False, "no redirect import", out)

        code, report, out = audit(noseh, "--wine-source", work / "wine")
        check(code == 1, "__TRY without compiler SEH fails", out)
        entry = report.get("modules", {}).get("noseh.dll", {})
        check(entry.get("compiler_seh_assessment") == "unconfirmed" and
              any("does not establish setjmp" in p for p in entry.get("problems", [])),
              "unpreprocessed source count is not misreported as proof of setjmp SEH", out)
        code, _, out = audit(noseh)
        check(code == 0, "same module passes when no source is supplied", out)

        if shutil.which("x86_64-w64-mingw32-gcc"):
            c = work / "tls.c"
            c.write_text(MINGW_TLS)
            tls = work / "tls.dll"
            run(["x86_64-w64-mingw32-gcc", "-shared", "-O1", str(c), "-o", str(tls)])
            code, report, out = audit(tls)
            check(code == 1 and report["modules"]["tls.dll"]["tls_directory"] is True,
                  "mingw CRT TLS directory fails the audit", out)
        else:
            print("note: mingw-w64 not found, TLS directory fixture skipped")

        (work / "notpe.dll").write_bytes(b"MZ not really")
        code, _, out = audit(work / "notpe.dll")
        check(code == 1, "malformed PE fails", out)
        code, _, out = audit(work / "missing.dll")
        check(code == 2, "missing input is a usage error", out)

    print("audit_wine_guest fixtures:", f"{checks}/{checks} checks passed" if not failures
          else f"{failures}/{checks} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
