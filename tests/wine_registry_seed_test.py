#!/usr/bin/env python3
"""Tests for tools/wine_registry_seed.py (rgs registrar, INF AddReg, PE resources).

Pure-Python cases always run. The PE resource case needs mingw-w64 windres
and gcc plus pefile; it is skipped individually when they are missing.
Set XCDM_WINE_SOURCE to a Wine checkout to also replay its shell32.rgs.
"""
from __future__ import annotations

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "wine_registry_seed.py"
spec = importlib.util.spec_from_file_location("wine_registry_seed", TOOL)
seed = importlib.util.module_from_spec(spec)
sys.modules["wine_registry_seed"] = seed
spec.loader.exec_module(seed)

failures = 0


def check(cond: bool, label: str, detail: object = "") -> None:
    global failures
    if not cond:
        failures += 1
        print(f"FAIL: {label}\n{detail}")


def value(reg, root, path, name=""):
    key = reg.open(root, path, create=False)
    entry = key.values.get(name.lower()) if key else None
    return (seed.TYPE_NAMES[entry[1]], entry[2]) if entry else None


# The exact shape widl emits for an apartment coclass (tools/widl/register.c).
WIDL_CLASSES = """HKCR
{
    NoRemove CLSID
    {
        '{71F96385-DDD6-48D3-A0C1-AE06E8B055FB}' = s 'Explorer Browser'
        {
            InprocServer32 = s '%MODULE%' { val ThreadingModel = s 'Apartment' }
        }
        '{F3364BA0-65B9-11CE-A9BA-00AA004AE837}' = s 'Shell File System Folder'
        {
            InprocServer32 = s '%MODULE%' { val ThreadingModel = s 'Apartment' }
        }
    }
    'Shell.Application' = s 'Shell Application' { CLSID = s '{13709620-C279-11CE-A49E-444553540000}' }
}
"""


def test_rgs_grammar():
    reg = seed.Registry()
    script = r"""
HKLM
{
    NoRemove Software
    {
        ForceRemove 'Xcdm Test' = s 'it''s %% done'
        {
            val Count = d '42'
            val Blob = b '0aFF10'
            val Path = s '%MODULE%'
            Sub { val Deep = s '%SystemRoot%\fonts' }
            Gone { val X = s 'y' }
            Delete Gone
        }
    }
}
HKCU { Volatile = s 'v' }
"""
    seed.apply_rgs(reg, script, {"MODULE": r"C:\windows\system32\x.dll", "SystemRoot": r"C:\windows"})
    check(value(reg, "HKLM", r"Software\Xcdm Test") == ("REG_SZ", "it's % done"), "quoted escape and %%")
    check(value(reg, "HKLM", r"Software\Xcdm Test", "Count") == ("REG_DWORD", 42), "d value")
    check(value(reg, "HKLM", r"Software\Xcdm Test", "Blob") == ("REG_BINARY", "0aff10"), "b value")
    check(value(reg, "HKLM", r"software\xcdm test", "path") == ("REG_SZ", r"C:\windows\system32\x.dll"),
          "%MODULE% replacement and case-insensitive lookup")
    check(value(reg, "HKLM", r"Software\Xcdm Test\Sub", "Deep") == ("REG_SZ", r"C:\windows\fonts"), "nested val")
    check(reg.open("HKLM", r"Software\Xcdm Test\Gone", create=False) is None, "Delete removes subtree")
    check(value(reg, "HKCU", "Volatile") == ("REG_SZ", "v"), "second root block")

    # ForceRemove replaces an existing subtree on re-registration.
    seed.apply_rgs(reg, "HKLM { NoRemove Software { ForceRemove 'Xcdm Test' { val Fresh = s '1' } } }", {})
    check(value(reg, "HKLM", r"Software\Xcdm Test", "Count") is None, "ForceRemove dropped old values")
    check(value(reg, "HKLM", r"Software\Xcdm Test", "Fresh") == ("REG_SZ", "1"), "ForceRemove recreated key")

    # HKCR maps to HKLM\Software\Classes.
    seed.apply_rgs(reg, WIDL_CLASSES, {"MODULE": r"C:\windows\system32\shell32.dll"})
    check(value(reg, "HKLM", r"Software\Classes\CLSID\{71F96385-DDD6-48D3-A0C1-AE06E8B055FB}\InprocServer32")
          == ("REG_SZ", r"C:\windows\system32\shell32.dll"), "HKCR CLSID lands in HKLM\\Software\\Classes")
    check(value(reg, "HKCR", r"CLSID\{71f96385-ddd6-48d3-a0c1-ae06e8b055fb}\InprocServer32", "ThreadingModel")
          == ("REG_SZ", "Apartment"), "ThreadingModel")
    check(value(reg, "HKCR", r"Shell.Application\CLSID") == ("REG_SZ", "{13709620-C279-11CE-A49E-444553540000}"),
          "ProgId mapping")

    for bad, why in (("HKXX { }", "bad root"), ("HKLM { A = s 'x", "unterminated quote"),
                     ("HKLM { val V }", "val without value"), ("HKLM { A = q 'x' }", "unknown type"),
                     ("HKLM { A = s '%NOPE%' }", "missing replacement"), ("HKLM A", "missing brace"),
                     ("HKLM { A { B = s 'x' }", "unterminated block"), ("HKLM { A = b 'abc' }", "odd binary")):
        try:
            seed.apply_rgs(seed.Registry(), bad, {})
            check(False, f"rgs error not raised: {why}")
        except seed.ScriptError:
            pass


INF = r'''
[version]
signature="$CHICAGO$"

[DefaultInstall]
Needs=Base
AddReg=Generic

[DefaultInstall.ntamd64]
Needs=Base
AddReg=Amd64 ; arch-specific wins
DelReg=Drop

[Base]
AddReg=\
    Common,\
    Types

[Common]
HKLM,%CV%,"ProgramFilesDir",,"%16422%"
HKLM,%CV%,"SystemRoot",0x20000,"%SystemRoot%\system32"
HKLM,%CV%,"Keep",,"first"
HKLM,%CV%,"Keep",2,"second"
HKCR,exefile\shell\open\command,,2,"""%1"" %*"

[Types]
HKLM,Software\T,"Dw",0x10001,7
HKLM,Software\T,"DwBytes",0x10001,01,02,00,00
HKLM,Software\T,"DwHex",0x10003,"0x20"
HKLM,Software\T,"Bin",1,de,ad
HKLM,Software\T,"Multi",0x10000,"a","b"
HKLM,Software\T,"Multi",0x10008,"b","c"
HKLM,Software\T,"Link",0x60000,"\Registry\Machine\Software\X"
HKLM,Software\T,"Qword",0x000b0001,01,00,00,00,00,00,00,01
HKLM,Software\T\Empty,,0x10
HKLM,Software\T,"None",0x20001

[Generic]
HKLM,Software\Arch,"Which",,"generic"

[Amd64]
HKLM,Software\Arch,"Which",,"amd64"

[Drop]
HKLM,Software\Drop

[Strings]
CV="Software\Microsoft\Windows\CurrentVersion"
'''


def test_inf():
    warnings: list[str] = []
    inf = seed.Inf(INF, warnings)
    reg = seed.Registry()
    # setupapi runs DelReg before AddReg: it removes pre-existing data only.
    seed.set_value(reg.open("HKLM", r"Software\Drop", create=True), "Bye", seed.REG_SZ, "x")
    seed.apply_inf(reg, inf, "DefaultInstall", "ntamd64")
    cv = r"Software\Microsoft\Windows\CurrentVersion"
    check(value(reg, "HKLM", cv, "ProgramFilesDir") == ("REG_SZ", r"C:\Program Files"), "DIRID 16422", warnings)
    check(value(reg, "HKLM", cv, "SystemRoot") == ("REG_EXPAND_SZ", r"%SystemRoot%\system32"),
          "EXPAND_SZ keeps env reference")
    check(value(reg, "HKLM", cv, "Keep") == ("REG_SZ", "first"), "NOCLOBBER keeps first value")
    check(value(reg, "HKCR", r"exefile\shell\open\command") == ("REG_SZ", '"%1" %*'), "quoted %1 argument")
    check(value(reg, "HKLM", r"Software\T", "Dw") == ("REG_DWORD", 7), "single-field DWORD")
    check(value(reg, "HKLM", r"Software\T", "DwBytes") == ("REG_DWORD", 0x201), "byte-list DWORD")
    check(value(reg, "HKLM", r"Software\T", "DwHex") == ("REG_DWORD", 0x20), "hex DWORD with NOCLOBBER bit")
    check(value(reg, "HKLM", r"Software\T", "Bin") == ("REG_BINARY", "dead"), "binary")
    check(value(reg, "HKLM", r"Software\T", "Multi") == ("REG_MULTI_SZ", ["a", "b", "c"]), "MULTI_SZ append")
    check(value(reg, "HKLM", r"Software\T", "Link") == ("REG_LINK", r"\Registry\Machine\Software\X"),
          "REG_LINK from high-word type")
    check(value(reg, "HKLM", r"Software\T", "Qword") == ("REG_QWORD", 0x0100000000000001), "QWORD")
    check(reg.open("HKLM", r"Software\T\Empty", create=False) is not None, "KEYONLY creates key")
    check(value(reg, "HKLM", r"Software\T", "None") == ("REG_NONE", None), "REG_NONE")
    check(value(reg, "HKLM", r"Software\Arch", "Which") == ("REG_SZ", "amd64"), "decorated section preferred")
    check(reg.open("HKLM", r"Software\Drop", create=False) is None, "DelReg removes key")
    check(warnings == [], "no warnings", warnings)

    reg2 = seed.Registry()
    seed.apply_inf(reg2, seed.Inf(INF, []), "DefaultInstall", "ntx86")
    check(value(reg2, "HKLM", r"Software\Arch", "Which") == ("REG_SZ", "generic"), "undecorated fallback")


def test_pe_resource_and_cli():
    windres = shutil.which("x86_64-w64-mingw32-windres")
    gcc = shutil.which("x86_64-w64-mingw32-gcc")
    try:
        import pefile  # noqa: F401,PLC0415
    except ImportError:
        windres = None
    if not windres or not gcc:
        print("note: mingw-w64 windres/gcc or pefile missing; PE resource case skipped")
        return
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        (work / "classes.rgs").write_text(WIDL_CLASSES)
        (work / "res.rc").write_text('1 WINE_REGISTRY "classes.rgs"\n')
        subprocess.run([windres, "res.rc", "-O", "coff", "-o", "res.o"], cwd=work, check=True)
        (work / "x.c").write_text("int __stdcall DllMain(void*a,unsigned long b,void*c){return 1;}\n")
        out_dir = work / "out"
        out_dir.mkdir()
        subprocess.run([gcc, "-shared", "-nostdlib", "-o", str(out_dir / "shell32.dll"), "x.c", "res.o",
                        "-Wl,-e,DllMain"], cwd=work, check=True)
        inf = work / "wine.inf"
        inf.write_text("[DefaultInstall]\nRegisterDlls=Reg\n[Reg]\n11,,shell32.dll,1\n11,,absent.dll,1\n")
        report = work / "seed.json"
        proc = subprocess.run([sys.executable, str(TOOL), "--inf", str(inf), "--dll-dir", str(out_dir),
                               "--require-clsid", "71f96385-ddd6-48d3-a0c1-ae06e8b055fb",
                               "--output", str(report)], capture_output=True, text=True)
        check(proc.returncode == 0, "CLI with PE resource succeeds", proc.stdout + proc.stderr)
        data = json.loads(report.read_text())
        key = "HKLM\\Software\\Classes\\CLSID\\{71F96385-DDD6-48D3-A0C1-AE06E8B055FB}\\InprocServer32"
        check(data["keys"].get(key, {}).get("", {}).get("data") == r"C:\windows\system32\shell32.dll",
              "InprocServer32 from WINE_REGISTRY resource", json.dumps(data["keys"], indent=1)[:2000])
        check(data["registered"] == [{"module": "shell32.dll", "scripts": 1}], "registered list", data["registered"])
        check(any("absent.dll" in w for w in data["warnings"]), "missing RegisterDlls module warned", data["warnings"])
        proc = subprocess.run([sys.executable, str(TOOL), "--dll-dir", str(out_dir),
                               "--require-clsid", "{00021400-0000-0000-C000-000000000046}"],
                              capture_output=True, text=True)
        check(proc.returncode == 1 and "FAIL" in proc.stdout, "missing required CLSID fails", proc.stdout)


def test_real_shell32_rgs():
    wine = os.environ.get("XCDM_WINE_SOURCE")
    if not wine or not (Path(wine) / "dlls" / "shell32" / "shell32.rgs").is_file():
        return
    reg = seed.Registry()
    seed.apply_rgs(reg, (Path(wine) / "dlls/shell32/shell32.rgs").read_text(),
                   {"MODULE": r"C:\windows\system32\shell32.dll", "SystemRoot": r"C:\windows"})
    check(value(reg, "HKCR", r"CLSID\{20D04FE0-3AEA-1069-A2D8-08002B30309D}", "LocalizedString")
          == ("REG_SZ", r"@C:\windows\system32\shell32.dll,-21"), "real shell32.rgs My Computer string")


if __name__ == "__main__":
    test_rgs_grammar()
    test_inf()
    test_pe_resource_and_cli()
    test_real_shell32_rgs()
    print("wine_registry_seed:", "passed" if not failures else f"{failures} failure(s)")
    sys.exit(1 if failures else 0)
