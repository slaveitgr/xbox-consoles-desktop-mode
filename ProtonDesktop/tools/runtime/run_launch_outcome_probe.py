#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Link launch regression probe to an already built REAL pinned Linux core.

This does not fetch dependencies, build Wine, or run Wine. New overlay objects
and fixtures go into a private RAM directory; the pristine checkout is retained.
"""
from pathlib import Path
import argparse
import importlib.util
import json
import shlex
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PIN = "58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4"


def execute(command, log):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=120)
    log.write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); inspect {log}\n{result.stdout[-6000:]}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "external/Boxedwine64-full")
    parser.add_argument("--headless-build", type=Path, required=True)
    parser.add_argument("--ram-parent", type=Path, default=Path("/dev/shm"))
    args = parser.parse_args()
    source = args.source.resolve()
    pin = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if pin != PIN or subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True):
        raise RuntimeError("The exact pristine pinned source checkout is required")
    work = Path(tempfile.mkdtemp(prefix="protondesktop-launch-probe-", dir=args.ram_parent))
    script = work / "prepare.cmake"
    script.write_text(f'''include("{ROOT}/tools/uwp-core/prepare_profile_overlay.cmake")
proton_prepare_uwp_profile_overlay("{source}" "{work}/overlay")
include("{ROOT}/tools/runtime/prepare_wine_session.cmake")
proton_prepare_wine_session("{source}" "{work}/overlay" sources)
''')
    execute(["cmake", "-P", str(script)], work / "prepare.log")
    commands = json.loads((args.headless_build / "compile_commands.json").read_text())
    base = shlex.split(commands[0]["command"])
    base = base[:base.index("-o")]
    include_dirs = [ROOT, work / "overlay/include", work / "overlay/source/sdl"]
    include_dirs += [source / value for value in ["source/io", "source/x11", "source/x11wire",
                    "source/sdl", "source", "source/kernel", "platform/sdl"]]
    compile_args = [base[0]] + ["-I" + str(path) for path in include_dirs] + base[1:]
    compile_args += ["-DBOXEDWINE_ZLIB=1", "-DPROTONDESKTOP_OWNED_GUEST_WORKERS=1"]
    host = (source / "platform/sdl/knativesystem.cpp").read_text()
    entry = '''#ifndef __TEST
int boxedmain(int argc, const char** argv);

int main(int argc, char** argv) {
    return boxedmain(argc, (const char**)argv);
}
#endif'''
    if host.count(entry) != 1:
        raise RuntimeError("Pinned SDL host main exclusion changed")
    host_source = work / "knativesystem.cpp"
    host_source.write_text(host.replace(entry, "// Regression executable owns main; genuine SDL host retained."))
    sources = [ROOT / "tools/runtime/LaunchOutcomeProbe.cpp", work / "overlay/source/kernel/kprocess.cpp",
               work / "overlay/source/kernel/ksystem.cpp",
               work / "overlay/source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp",
               host_source, ROOT / "backend/uwp/UwpGuestWorkers.cpp"]
    objects = []
    for index, path in enumerate(sources):
        obj = work / f"probe-{index}.o"
        execute(compile_args + ["-c", str(path), "-o", str(obj)], work / f"compile-{index}.log")
        objects.append(str(obj))
    excluded = ["source/sdl/main.cpp", "platform/sdl/knativesystem.cpp", "source/kernel/kprocess.cpp",
                "source/kernel/ksystem.cpp", "source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp"]
    for command in commands:
        if any(command["file"].endswith(suffix) for suffix in excluded):
            continue
        parts = shlex.split(command["command"])
        obj = Path(command["directory"]) / parts[parts.index("-o") + 1]
        if not obj.is_file():
            raise RuntimeError(f"Build the actual headless core first: missing {obj}")
        objects.append(str(obj))
    sdl = next(line.split("=", 1)[1] for line in (args.headless_build / "CMakeCache.txt").read_text().splitlines()
               if line.startswith("SDL2_RUNTIME:FILEPATH="))
    executable = work / "launch-outcome-probe"
    execute([base[0], "-Wl,--gc-sections", "-pthread", "-o", str(executable)] + objects + [sdl, "-lm"],
            work / "link.log")
    fixture = work / "fixture-root"
    fixture.mkdir()
    spec = importlib.util.spec_from_file_location("smoke", ROOT / "tools/make_elf64_smoke.py")
    smoke = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(smoke)
    valid = smoke.make_elf()
    (fixture / "not-elf").write_bytes(b"not ELF\n")
    (fixture / "valid-static.elf").write_bytes(valid)
    (fixture / "negative-static.elf").write_bytes(smoke.make_elf(negative=True))
    bad = bytearray(valid)
    struct.pack_into("<H", bad, 56, 3)
    interp = b"/missing-loader\0"
    bad.extend(b"\0" * (0x3000 + len(interp) - len(bad)))
    bad[0x3000:0x3000 + len(interp)] = interp
    bad[64 + 2 * 56:64 + 3 * 56] = struct.pack("<IIQQQQQQ", 3, 4, 0x3000, 0, 0, len(interp), len(interp), 1)
    (fixture / "missing-interpreter.elf").write_bytes(bad)
    log = execute([str(executable), str(fixture)], work / "probe.log")
    checks = sum(line.startswith("PASS:") for line in log.splitlines())
    if checks != 24 or "launch-outcome probe: 0 failed" not in log:
        raise RuntimeError("The actual launch probe did not report all 24 expected checks")
    report = {"source_commit": pin, "scope": "real Linux pinned kernel/ELF loader/owned worker integration",
              "passed": checks, "failed": 0, "wine_executed": False,
              "self_authored_guest_exit_controls": [0, 9], "work_directory": str(work)}
    (work / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
