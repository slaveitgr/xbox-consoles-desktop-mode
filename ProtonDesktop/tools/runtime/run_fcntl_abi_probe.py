#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run our static flock ELF through the actual pinned Linux CPU64/kernel.

Requires an already built tools/headless-core checkout. Recompiles production
Wine-session overlays into a fresh private RAM directory, reusing the remaining
real core objects. No rootfs, Wine, UWP, Xbox, downloads or host fixture execution.
Use --check-baseline to also demonstrate the pinned raw flock copy failing with
guest status 22; the production overlay must pass all 26 checks with status 0.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shlex
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PIN = "58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4"


def execute(command, log, timeout=120, env=None, require_success=True):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=timeout, env=env)
    log.write_text(result.stdout)
    if require_success and result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); inspect {log}\n{result.stdout[-6000:]}")
    return result


def parts(command):
    return command.get("arguments") or shlex.split(command["command"])


def object_path(command):
    tokens = parts(command)
    return Path(command["directory"]) / tokens[tokens.index("-o") + 1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "external/Boxedwine64-full")
    parser.add_argument("--headless-build", type=Path, required=True)
    parser.add_argument("--ram-parent", type=Path, default=Path("/dev/shm"),
                        help="RAM-backed parent directory for all generated files")
    parser.add_argument("--guest-cc", default="cc", help="Linux x86-64 C compiler")
    parser.add_argument("--check-baseline", action="store_true")
    args = parser.parse_args()
    source, build = args.source.resolve(), args.headless_build.resolve()
    pin = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if pin != PIN or subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True):
        raise RuntimeError("The exact pristine pinned source checkout is required")
    commands = json.loads((build / "compile_commands.json").read_text())
    work = Path(tempfile.mkdtemp(prefix="protondesktop-fcntl-abi-", dir=args.ram_parent))
    print(f"Work directory: {work}", flush=True)
    script = work / "prepare.cmake"
    script.write_text(f'''include("{ROOT}/tools/uwp-core/prepare_profile_overlay.cmake")
proton_prepare_uwp_profile_overlay("{source}" "{work}/overlay")
include("{ROOT}/tools/runtime/prepare_wine_session.cmake")
proton_prepare_wine_session("{source}" "{work}/overlay" sources)
''')
    execute(["cmake", "-P", str(script)], work / "prepare.log")
    syscall = work / "overlay/source/kernel/syscall64.cpp"
    if not syscall.is_file():
        raise RuntimeError("Production Wine-session overlay must generate syscall64.cpp")

    base = parts(commands[0])
    base = base[:base.index("-o")]
    include_dirs = [ROOT, work / "overlay/include", work / "overlay/source/sdl"]
    include_dirs += [source / value for value in ["source/io", "source/x11", "source/x11wire",
                    "source/sdl", "source", "source/kernel", "platform/sdl"]]
    compile_args = [base[0]] + ["-I" + str(path) for path in include_dirs] + base[1:]
    # Match the cached headless core's no-ZIP ABI: this fixture needs no rootfs.
    compile_args += ["-DPROTONDESKTOP_OWNED_GUEST_WORKERS=1"]
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
    host_source.write_text(host.replace(entry, "// Regression driver owns main; genuine SDL host retained."))

    fixture = work / "flock-abi.elf"
    execute([args.guest_cc, "-nostdlib", "-static", "-no-pie", "-fno-stack-protector",
             "-ffreestanding", "-Os", "-Wl,-e,_start", str(ROOT / "tools/runtime/fcntl_abi_guest.c"),
             "-o", str(fixture)], work / "compile-guest.log")
    image = fixture.read_bytes()
    if image[:7] != b"\x7fELF\x02\x01\x01" or struct.unpack_from("<H", image, 18)[0] != 62:
        raise RuntimeError("The self-authored fixture must be a little-endian x86-64 ELF")

    replacements = {
        "source/kernel/kprocess.cpp": work / "overlay/source/kernel/kprocess.cpp",
        "source/kernel/kmemory64.cpp": work / "overlay/source/kernel/kmemory64.cpp",
        "source/kernel/ksystem.cpp": work / "overlay/source/kernel/ksystem.cpp",
        "source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp":
            work / "overlay/source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp",
        "platform/sdl/knativesystem.cpp": host_source,
        "source/kernel/syscall64.cpp": syscall,
    }
    overlay_objects = {}
    for index, (relative, path) in enumerate(replacements.items()):
        obj = work / f"overlay-{index}.o"
        execute(compile_args + ["-c", str(path), "-o", str(obj)], work / f"compile-overlay-{index}.log")
        overlay_objects[relative] = str(obj)
    additional = []
    for index, path in enumerate([ROOT / "tools/runtime/FcntlAbiProbe.cpp",
                                   ROOT / "backend/uwp/UwpGuestWorkers.cpp"]):
        obj = work / f"driver-{index}.o"
        execute(compile_args + ["-c", str(path), "-o", str(obj)], work / f"compile-driver-{index}.log")
        additional.append(str(obj))
    objects = list(additional)
    replaced_seen = set()
    reused = 0
    for command in commands:
        path = Path(command["file"])
        relative = path.relative_to(source).as_posix() if path.is_relative_to(source) else ""
        if relative == "source/sdl/main.cpp":
            continue
        if relative in overlay_objects:
            objects.append(overlay_objects[relative])
            replaced_seen.add(relative)
            continue
        obj = object_path(command)
        if not obj.is_file():
            raise RuntimeError(f"Build the actual headless core first: missing {obj}")
        objects.append(str(obj))
        reused += 1
    if replaced_seen != set(replacements):
        raise RuntimeError(f"Headless core lacks required real units: {set(replacements) - replaced_seen}")
    sdl = next(line.split("=", 1)[1] for line in (build / "CMakeCache.txt").read_text().splitlines()
               if line.startswith("SDL2_RUNTIME:FILEPATH="))
    environment = os.environ.copy()
    environment.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software")
    variants = [("fixed", None)]
    if args.check_baseline:
        baseline = work / "baseline-syscall64.o"
        execute(compile_args + ["-c", str(source / "source/kernel/syscall64.cpp"), "-o", str(baseline)],
                work / "compile-baseline.log")
        variants.insert(0, ("baseline", str(baseline)))
    results = {}
    for label, alternative in variants:
        linked = list(objects)
        if alternative:
            linked[linked.index(overlay_objects["source/kernel/syscall64.cpp"])] = alternative
        executable = work / f"fcntl-abi-probe-{label}"
        execute([base[0], "-Wl,--gc-sections", "-pthread", "-o", str(executable)] + linked + [sdl, "-lm"],
                work / f"link-{label}.log")
        guest_root = work / f"root-{label}"
        guest_root.mkdir()
        shutil.copyfile(fixture, guest_root / fixture.name)
        # Only the DRIVER is executed on the host; the ELF is consumed by KProcess.
        result = execute([str(executable), str(guest_root)], work / f"run-{label}.log",
                         timeout=15, env=environment, require_success=False)
        passes = result.stdout.count("PASSFAIL_FCNTL PASS: ")
        failures = result.stdout.count("PASSFAIL_FCNTL FAIL: ")
        expected = 22 if label == "baseline" else 0
        qualified = result.returncode == expected and (
            (passes == 26 and failures == 0) if label == "fixed" else
            (failures == 2 and "FAIL: F_GETLK returns the actual nonzero parent owner PID" in result.stdout))
        results[label] = {"host_exit": result.returncode, "expected_guest_exit": expected,
                          "passed_checks": passes, "failed_checks": failures,
                          "qualified": qualified, "log": str(work / f"run-{label}.log")}
        print(f"{label}: exit={result.returncode}, passes={passes}, failures={failures}", flush=True)
    report = {"source_commit": pin, "scope": "real pinned Linux CPU64/kernel/syscall/owned workers",
              "work_directory": str(work), "results": results, "reused_real_objects": reused,
              "fixture_sha256": hashlib.sha256(image).hexdigest(),
              "wine_executed": False, "uwp_tested": False, "xbox_tested": False,
              "upstream_modified": False, "passed": all(item["qualified"] for item in results.values())}
    (work / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    if not report["passed"]:
        raise RuntimeError(f"The actual fcntl ABI probe failed; inspect {work / 'result.json'}")


if __name__ == "__main__":
    main()
