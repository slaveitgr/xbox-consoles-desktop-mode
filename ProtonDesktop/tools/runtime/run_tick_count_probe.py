#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute a real CPU64 ELF regression against pristine and checked mmap paths.

Requires an existing pinned Linux headless core build. All generated binaries,
fixtures and logs stay in a new private RAM directory. No Wine or ZIP executes.
The positive build uses the production Wine-session overlay, not a test shim.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import platform
import shlex
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PIN = "58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4"


def execute(command, log, *, timeout=120, env=None):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=timeout, env=env)
    log.write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); inspect {log}\n{result.stdout[-6000:]}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "external/Boxedwine64-full")
    parser.add_argument("--headless-build", type=Path, required=True)
    parser.add_argument("--ram-parent", type=Path, default=Path("/dev/shm"))
    parser.add_argument("--cc", default=shutil.which("cc"))
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine() not in ("x86_64", "amd64") or not args.cc:
        raise RuntimeError("An x86-64 Linux host and C compiler are required")
    source, build = args.source.resolve(), args.headless_build.resolve()
    pin = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if pin != PIN or subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True):
        raise RuntimeError("The exact pristine pinned source checkout is required")
    work = Path(tempfile.mkdtemp(prefix="protondesktop-tick-count-", dir=args.ram_parent))
    report = {"source_commit": pin, "status": "incomplete", "scope": "real Linux CPU64/static ELF mmap integration",
              "wine_executed": False, "uwp_executed": False, "xbox_executed": False,
              "production_modified": False, "work_directory": str(work)}
    report_path = work / "result.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    try:
        script = work / "prepare.cmake"
        script.write_text(f'''include("{ROOT}/tools/uwp-core/prepare_profile_overlay.cmake")
proton_prepare_uwp_profile_overlay("{source}" "{work}/overlay")
include("{ROOT}/tools/runtime/prepare_wine_session.cmake")
proton_prepare_wine_session("{source}" "{work}/overlay" sources)
''')
        execute(["cmake", "-P", str(script)], work / "prepare.log")
        commands = json.loads((build / "compile_commands.json").read_text())
        base = shlex.split(commands[0]["command"])
        base = base[:base.index("-o")]
        includes = [ROOT, work / "overlay/include", work / "overlay/source/sdl"]
        includes += [source / item for item in ("source/io", "source/x11", "source/x11wire",
                     "source/sdl", "source", "source/kernel", "platform/sdl")]
        compile_args = [base[0]] + ["-I" + str(path) for path in includes] + base[1:]
        compile_args += ["-DPROTONDESKTOP_OWNED_GUEST_WORKERS=1"]
        host = (source / "platform/sdl/knativesystem.cpp").read_text()
        entry = '''#ifndef __TEST
int boxedmain(int argc, const char** argv);

int main(int argc, char** argv) {
    return boxedmain(argc, (const char**)argv);
}
#endif'''
        if host.count(entry) != 1:
            raise RuntimeError("Pinned SDL main exclusion changed")
        host_source = work / "knativesystem.cpp"
        host_source.write_text(host.replace(entry, "// TickCount probe owns main; genuine SDL host retained."))
        sources = [ROOT / "tools/runtime/TickCountProbe.cpp", work / "overlay/source/kernel/kprocess.cpp",
                   work / "overlay/source/kernel/kmemory64.cpp", work / "overlay/source/kernel/ksystem.cpp",
                   work / "overlay/source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp",
                   host_source, ROOT / "backend/uwp/UwpGuestWorkers.cpp"]
        objects = []
        for index, path in enumerate(sources):
            obj = work / f"probe-{index}.o"
            execute(compile_args + ["-c", str(path), "-o", str(obj)], work / f"compile-{index}.log")
            objects.append(str(obj))
        excluded = ["source/sdl/main.cpp", "platform/sdl/knativesystem.cpp", "source/kernel/kprocess.cpp",
                    "source/kernel/kmemory64.cpp", "source/kernel/ksystem.cpp", "source/kernel/syscall64.cpp",
                    "source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp"]
        for command in commands:
            if any(command["file"].endswith(suffix) for suffix in excluded):
                continue
            parts = shlex.split(command["command"])
            obj = Path(command["directory"]) / parts[parts.index("-o") + 1]
            if not obj.is_file():
                raise RuntimeError(f"Build the actual headless core first: missing {obj}")
            objects.append(str(obj))
        elf = work / "tick-count.elf"
        execute([args.cc, "-nostdlib", "-static", "-no-pie", "-fno-stack-protector", "-ffreestanding", "-Os",
                 "-Wl,-e,_start", str(ROOT / "tools/runtime/tick_count_guest.c"), "-o", str(elf)], work / "compile-elf.log")
        data = elf.read_bytes()
        if data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<HH", data, 16) != (2, 62):
            raise RuntimeError("The self-authored fixture is not a little-endian static x86-64 ELF")
        phoff, phsize, phcount = struct.unpack_from("<Q", data, 32)[0], *struct.unpack_from("<HH", data, 54)
        if any(struct.unpack_from("<I", data, phoff + index * phsize)[0] == 3 for index in range(phcount)):
            raise RuntimeError("The fixture unexpectedly requests a dynamic interpreter")
        sdl = next(line.split("=", 1)[1] for line in (build / "CMakeCache.txt").read_text().splitlines()
                   if line.startswith("SDL2_RUNTIME:FILEPATH="))
        env = os.environ.copy()
        env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software")
        results = {}
        for label, path, expected in [("baseline", source / "source/kernel/syscall64.cpp", 10),
                                      ("production-overlay", work / "overlay/source/kernel/syscall64.cpp", 0)]:
            obj, exe = work / f"syscall-{label}.o", work / f"tick-count-{label}"
            execute(compile_args + ["-c", str(path), "-o", str(obj)], work / f"compile-syscall-{label}.log")
            execute([base[0], "-Wl,--gc-sections", "-pthread", "-o", str(exe)] + objects + [str(obj), sdl, "-lm"],
                    work / f"link-{label}.log")
            fixture = work / f"fixture-{label}"
            fixture.mkdir()
            (fixture / elf.name).write_bytes(data)
            log = execute([str(exe), str(fixture), str(expected)], work / f"probe-{label}.log", timeout=10, env=env)
            witness = f"ACTUAL_TICK_COUNT_ELF terminated=1 guest_exit={expected} expected={expected}"
            if witness not in log:
                raise RuntimeError(f"The {label} run did not report its expected real guest exit")
            results[label] = {"guest_exit": expected, "host_exit": 0, "fixture_sha256": hashlib.sha256(data).hexdigest()}
        report.update(status="passed", results=results, checked_contracts=["mmap(NULL) MAP_SHARED writer aliases fixed reader",
            "Wine8 KSYSTEM_TIME read terminates with the published value", "observed clock advances within a bound",
            "MAP_PRIVATE view stays isolated"], clock_origin_qualified=False, nanosleep_qualified=False)
    except Exception as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        report_path.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
