#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify real Linux CPU64 process/thread identities with our static guest ELF.

Requires a built tools/headless-core checkout. Uses production Wine-session
source overlays and reuses the actual remaining core object cache. All generated
files live in a fresh RAM directory. No rootfs, Wine, UWP or Xbox is executed.
--check-baseline reverses ONLY the checked getpid split in the production overlay:
baseline kill(getpid(), 0) must fail with guest exit 10; fixed passes 14 checks.
This qualifies PID consistency, not Linux's separate leader-TID allocation rule.
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

# Reuse the Linux probe's ordinary compiler/subprocess helpers, not its main.
from run_fcntl_abi_probe import ROOT, PIN, execute, parts, object_path


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
    work = Path(tempfile.mkdtemp(prefix="protondesktop-process-identity-", dir=args.ram_parent))
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

    fixture = work / "process-identity.elf"
    execute([args.guest_cc, "-nostdlib", "-static", "-no-pie", "-fno-stack-protector",
             "-ffreestanding", "-Os", "-Wl,-e,_start", str(ROOT / "tools/runtime/process_identity_guest.c"),
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
    for index, path in enumerate([ROOT / "tools/runtime/ProcessIdentityProbe.cpp",
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
        # Keep every other production fix: reverse only process-ID selection.
        before = """        case X64_SYS_getpid:
            ret = cpu->thread && cpu->thread->process ? cpu->thread->process->id : 1;
            break;
        case X64_SYS_gettid:
"""
        after = """        case X64_SYS_getpid:
        case X64_SYS_gettid:
"""
        current = syscall.read_text()
        if current.count(before) != 1:
            raise RuntimeError("The checked production getpid split no longer matches")
        baseline_source = work / "baseline-syscall64.cpp"
        baseline_source.write_text(current.replace(before, after, 1))
        baseline = work / "baseline-syscall64.o"
        execute(compile_args + ["-c", str(baseline_source), "-o", str(baseline)],
                work / "compile-baseline.log")
        variants.insert(0, ("baseline", str(baseline)))
    results = {}
    for label, alternative in variants:
        linked = list(objects)
        if alternative:
            linked[linked.index(overlay_objects["source/kernel/syscall64.cpp"])] = alternative
        executable = work / f"process-identity-probe-{label}"
        execute([base[0], "-Wl,--gc-sections", "-pthread", "-o", str(executable)] + linked + [sdl, "-lm"],
                work / f"link-{label}.log")
        guest_root = work / f"root-{label}"
        guest_root.mkdir()
        shutil.copyfile(fixture, guest_root / fixture.name)
        # Only the DRIVER is executed on the host; the ELF is consumed by KProcess.
        result = execute([str(executable), str(guest_root)], work / f"run-{label}.log",
                         timeout=15, env=environment, require_success=False)
        passes = result.stdout.count("PASSFAIL_ID PASS: ")
        failures = result.stdout.count("PASSFAIL_ID FAIL: ")
        expected = 10 if label == "baseline" else 0
        qualified = result.returncode == expected and (
            (passes == 14 and failures == 0) if label == "fixed" else
            (passes == 1 and failures == 1 and "FAIL: getpid returns a process recognized by kill(pid,0)" in result.stdout))
        results[label] = {"host_exit": result.returncode, "expected_guest_exit": expected,
                          "passed_checks": passes, "failed_checks": failures,
                          "qualified": qualified, "log": str(work / f"run-{label}.log")}
        print(f"{label}: exit={result.returncode}, passes={passes}, failures={failures}", flush=True)
    report = {"source_commit": pin, "scope": "real pinned Linux CPU64/kernel/syscall/owned workers",
              "work_directory": str(work), "results": results, "reused_real_objects": reused,
              "fixture_sha256": hashlib.sha256(image).hexdigest(),
              "baseline_reverses_only_getpid_split": args.check_baseline,
              "wine_executed": False, "uwp_tested": False, "xbox_tested": False,
              "upstream_modified": False, "passed": all(item["qualified"] for item in results.values())}
    (work / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    if not report["passed"]:
        raise RuntimeError(f"The actual process identity probe failed; inspect {work / 'result.json'}")


if __name__ == "__main__":
    main()
