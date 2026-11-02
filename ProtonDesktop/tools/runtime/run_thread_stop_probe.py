#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Reproduce real-kernel cancellation races against a pristine pin/no-ZIP cache.

Builds only new overlay objects and a self-authored kernel-method harness in RAM.
No Wine/rootfs, console calls, downloads, source edits, or test-only API stubs.
Baseline must exhibit four rescued failures and an isolated dual-exit failure;
fixed workers must really join, including both concurrent group-exit callers.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PIN = "58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4"
CASES = ("socket-before-registration", "futex-before-registration",
         "shared-socket-targeted-kill", "guest-exitgroup-plus-host-controller")


def execute(command, log, timeout=150, env=None):
    try:
        completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired as failure:
        log.write_text((failure.stdout or b"").decode(errors="replace") if
                       isinstance(failure.stdout, bytes) else failure.stdout or "")
        raise RuntimeError(f"Bounded child process timed out; no successful join is claimed: {log}") from failure
    log.write_text(completed.stdout)
    return completed


def checked(command, log):
    result = execute(command, log)
    if result.returncode:
        raise RuntimeError(f"Build command failed ({result.returncode}): {log}\n{result.stdout[-6000:]}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "external/Boxedwine64-full")
    parser.add_argument("--headless-build", type=Path, required=True)
    parser.add_argument("--ram-parent", type=Path, default=Path("/dev/shm"))
    args = parser.parse_args()
    source, cache = args.source.resolve(), args.headless_build.resolve()
    pin = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if pin != PIN or subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True):
        raise RuntimeError("The exact pristine pinned source checkout is required")
    work = Path(tempfile.mkdtemp(prefix="protondesktop-stop-probe-", dir=args.ram_parent))
    profile, fixed = work / "profile", work / "fixed"
    script = work / "prepare.cmake"
    script.write_text(f'''include("{ROOT}/tools/uwp-core/prepare_profile_overlay.cmake")
proton_prepare_uwp_profile_overlay("{source}" "{profile}")
include("{ROOT}/tools/runtime/prepare_wine_session.cmake")
proton_prepare_wine_session("{source}" "{profile}" sources)
''')
    checked(["cmake", "-P", str(script)], work / "prepare.log")
    spec = importlib.util.spec_from_file_location("thread_stop", ROOT / "tools/runtime/prepare_thread_stop.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    manifest = module.prepare(source, profile, fixed)
    commands = json.loads((cache / "compile_commands.json").read_text())
    base = shlex.split(commands[0]["command"])
    base = base[:base.index("-o")]
    joined_flags = " ".join(base)
    if "BOXEDWINE_ZLIB" in joined_flags or "BOXEDWINE_MULTI_THREADED" not in joined_flags:
        raise RuntimeError("This runner requires the actual no-ZIP MULTI_THREADED headless cache; class layouts must match")
    includes = [ROOT, profile / "include", profile / "source/sdl"]
    includes += [source / name for name in ("source/io", "source/x11", "source/x11wire",
                 "source/sdl", "source", "source/kernel", "platform/sdl")]
    baseline_flags = [base[0]] + ["-I" + str(path) for path in includes] + base[1:]
    baseline_flags += ["-DPROTONDESKTOP_OWNED_GUEST_WORKERS=1", "-O1", "-ffunction-sections", "-fdata-sections"]
    fixed_flags = baseline_flags[:1] + ["-I" + str(fixed / "include")] + baseline_flags[1:]
    baseline_normal = (profile / "source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp").read_text()
    baseline_normal = module.replace(baseline_normal, "        thread->terminating = true;",
        "        thread->terminating = true;\n        extern void protonFixtureTerminationMarked(U32);\n"
        "        protonFixtureTerminationMarked(threadId);")
    baseline_path = work / "normal-baseline-probe.cpp"
    baseline_path.write_text(baseline_normal)
    fixed_normal = (fixed / "source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp").read_text()
    fixed_normal = module.replace(fixed_normal,
        "    const auto cond = process->protonRequestThreadTermination(threadId);",
        "    const auto cond = process->protonRequestThreadTermination(threadId);\n"
        "    extern void protonFixtureTerminationMarked(U32);\n    protonFixtureTerminationMarked(threadId);")
    fixed_path = work / "normal-fixed-probe.cpp"
    wait_anchor = "        BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(process->threadRemovedCondition);"
    wait_probe = ("        extern void protonFixtureThreadRemovalWait(U32);\n"
                  "        protonFixtureThreadRemovalWait(threadId);\n" + wait_anchor)
    baseline_path.write_text(module.replace(baseline_normal, wait_anchor, wait_probe))
    fixed_normal = module.replace(fixed_normal, wait_anchor, wait_probe)
    fixed_path.write_text(fixed_normal)
    host = (source / "platform/sdl/knativesystem.cpp").read_text()
    old_main = '''#ifndef __TEST
int boxedmain(int argc, const char** argv);

int main(int argc, char** argv) {
    return boxedmain(argc, (const char**)argv);
}
#endif'''
    host_path = work / "knativesystem.cpp"
    host_path.write_text(module.replace(host, old_main, "// Regression executable owns main; real SDL host retained."))
    baseline_sources = {
        "source/kernel/kprocess.cpp": profile / "source/kernel/kprocess.cpp",
        "source/kernel/kmemory64.cpp": profile / "source/kernel/kmemory64.cpp",
        "source/kernel/syscall64.cpp": profile / "source/kernel/syscall64.cpp",
        "source/kernel/ksystem.cpp": profile / "source/kernel/ksystem.cpp",
        "source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp": baseline_path,
        "platform/sdl/knativesystem.cpp": host_path,
    }
    compiled = {}
    for index, (relative, path) in enumerate(baseline_sources.items()):
        obj = work / f"baseline-{index}.o"
        checked(baseline_flags + ["-c", str(path), "-o", str(obj)], work / f"compile-baseline-{index}.log")
        compiled[relative] = str(obj)
    worker = work / "workers.o"
    checked(baseline_flags + ["-c", str(ROOT / "backend/uwp/UwpGuestWorkers.cpp"), "-o", str(worker)],
            work / "compile-workers.log")
    baseline_harness, fixed_harness = work / "harness-baseline.o", work / "harness-fixed.o"
    for flags, obj in ((baseline_flags, baseline_harness), (fixed_flags, fixed_harness)):
        checked(flags + ["-c", str(ROOT / "tools/runtime/ThreadStopProbe.cpp"), "-o", str(obj)],
                work / (obj.stem + ".log"))
    objects, logical_objects = [], {}
    for command in commands:
        path = Path(command["file"])
        relative = path.relative_to(source).as_posix() if path.is_relative_to(source) else ""
        if relative == "source/sdl/main.cpp":
            continue
        pieces = shlex.split(command["command"])
        obj = compiled.get(relative, str(Path(command["directory"]) / pieces[pieces.index("-o") + 1]))
        if not Path(obj).is_file():
            raise RuntimeError(f"Build the actual headless core first; missing {obj}")
        objects.append(obj)
        logical_objects[relative] = obj
    objects.append(str(worker))
    sdl = next(line.split("=", 1)[1] for line in (cache / "CMakeCache.txt").read_text().splitlines()
               if line.startswith("SDL2_RUNTIME:FILEPATH="))
    link = [base[0], "-Wl,--gc-sections", "-pthread"]
    baseline_exe, fixed_exe = work / "stop-baseline", work / "stop-fixed"
    checked(link + ["-o", str(baseline_exe), str(baseline_harness)] + objects + [sdl, "-lm"], work / "link-baseline.log")
    fixed_objects = objects[:]
    fixed_compiled = {}
    for index, relative in enumerate(manifest["source_files"]):
        if relative not in logical_objects:
            continue
        path = fixed_path if relative == "source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp" else fixed / relative
        obj = work / f"fixed-{index}.o"
        checked(fixed_flags + ["-c", str(path), "-o", str(obj)], work / f"compile-fixed-{index}.log")
        fixed_objects[fixed_objects.index(logical_objects[relative])] = str(obj)
        fixed_compiled[relative] = str(obj)
    checked(link + ["-o", str(fixed_exe), str(fixed_harness)] + fixed_objects + [sdl, "-lm"], work / "link-fixed.log")
    # Isolate the new ownership defect against the preceding synchronized-wait
    # implementation, rather than attributing it to the pristine lost wakeup.
    previous_process = work / "kprocess-before-group-ownership.cpp"
    previous_process.write_text(module.replace((fixed / "source/kernel/kprocess.cpp").read_text(),
                                               module.EXIT_GROUP_ENTRY,
                                               "U32 KProcess::exitgroup(KThread* thread, U32 code) {"))
    previous_object = work / "kprocess-before-group-ownership.o"
    checked(fixed_flags + ["-c", str(previous_process), "-o", str(previous_object)],
            work / "compile-before-group-ownership.log")
    previous_objects = fixed_objects[:]
    previous_objects[previous_objects.index(fixed_compiled["source/kernel/kprocess.cpp"])] = str(previous_object)
    previous_exe = work / "stop-before-group-ownership"
    checked(link + ["-o", str(previous_exe), str(fixed_harness)] + previous_objects + [sdl, "-lm"],
            work / "link-before-group-ownership.log")
    fixture = work / "fixture-root"
    fixture.mkdir()
    environment = os.environ.copy()
    environment.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    baseline = execute([str(baseline_exe), str(fixture)], work / "baseline.log", timeout=15, env=environment)
    corrected = execute([str(fixed_exe), str(fixture)], work / "fixed.log", timeout=15, env=environment)
    dual_baseline = execute([str(previous_exe), str(fixture), "--dual-exit-group"],
                            work / "dual-baseline.log", timeout=5, env=environment)
    dual_corrected = execute([str(fixed_exe), str(fixture), "--dual-exit-group"],
                             work / "dual-fixed.log", timeout=5, env=environment)
    if baseline.returncode != 23 or "counterexamples=4" not in baseline.stdout:
        raise RuntimeError(f"All four actual baseline failures were not reproduced: {work / 'baseline.log'}")
    if corrected.returncode or "ALL_REAL_WORKERS_JOINED counterexamples=0" not in corrected.stdout:
        raise RuntimeError(f"Fixed run did not genuinely join every worker without rescue: {work / 'fixed.log'}")
    if dual_baseline.returncode != 90 or "DUAL_EXIT_BLOCKED marks=2" not in dual_baseline.stdout:
        raise RuntimeError(f"Actual dual-exit cancellation cycle was not reproduced: {work / 'dual-baseline.log'}")
    if (dual_corrected.returncode or "DUAL_EXIT_ALL_REAL_WORKERS_JOINED" not in dual_corrected.stdout or
            "DUAL_EXIT_OWNER_WAITED_FOR_LIVE_SIBLING returned=1 remaining_threads=2 process_terminated=0" not in dual_corrected.stdout):
        raise RuntimeError(f"Dual-exit did not safely wait, finalize and join: {work / 'dual-fixed.log'}")
    for case in CASES:
        if f"CASE {case} completed_before_external_rescue=0" not in baseline.stdout:
            raise RuntimeError(f"Missing baseline counterexample {case}")
        if f"CASE {case} completed_before_external_rescue=1" not in corrected.stdout:
            raise RuntimeError(f"Missing fixed completion {case}")
    layouts = [next(line for line in result.stdout.splitlines() if line.startswith("LAYOUT "))
               for result in (baseline, corrected)]
    if layouts[0] != layouts[1]:
        raise RuntimeError("Baseline/fixed kernel layouts differ; cached-object qualification is invalid")
    report = {"source_pin": PIN, "scope": "real Linux pinned kernel methods and owned worker cancellation",
              "wine_executed": False, "uwp_tested": False, "xbox_tested": False,
              "production_modified": False, "baseline_counterexamples": 5, "fixed_cases_passed": 5,
              "dual_exit_baseline_child_exit": dual_baseline.returncode,
              "dual_exit_baseline_scope": "synchronized waits, without group-exit ownership",
              "dual_exit_baseline_join_claimed": False, "dual_exit_fixed_workers_joined": 2,
              "dual_exit_owner_waited_for_live_sibling": True,
              "fixed_external_rescue_used": False, "all_workers_really_joined": True,
              "layout": layouts[0], "full_wine_shutdown_qualified": False,
              "baseline_sha256": hashlib.sha256(baseline_exe.read_bytes()).hexdigest(),
              "dual_exit_baseline_sha256": hashlib.sha256(previous_exe.read_bytes()).hexdigest(),
              "fixed_sha256": hashlib.sha256(fixed_exe.read_bytes()).hexdigest(), "work_directory": str(work)}
    (work / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
