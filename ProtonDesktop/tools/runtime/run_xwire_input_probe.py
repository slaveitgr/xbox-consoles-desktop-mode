#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise real SDL input and X11 socket records through the desktop overlay.

Requires an already built tools/headless-core cache. Regenerates the production
Wine presenter and desktop/input shadows, then rebuilds ALL three XWire units
with their complete matching headers. The cache and this fixture both use the
same no-ZIP ABI. No Wine, rootfs, guest executable, UWP or Xbox is run.
--check-baseline verifies that compositor-only input loses cross-client events;
the fixed profile must pass all 33 actual wire assertions.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile

from prepare_wine_session import prepare as prepare_wine_session
from prepare_xwire_desktop import prepare_xwire_desktop
from prepare_xwire_input import prepare_xwire_input, REQUIRED_TUS
from run_fcntl_abi_probe import ROOT, PIN, execute, parts, object_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "external/Boxedwine64-full")
    parser.add_argument("--headless-build", type=Path, required=True)
    parser.add_argument("--ram-parent", type=Path, default=Path("/dev/shm"))
    parser.add_argument("--check-baseline", action="store_true")
    args = parser.parse_args()
    source, build = args.source.resolve(), args.headless_build.resolve()
    pin = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    changed = subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True)
    if pin != PIN or changed:
        raise RuntimeError("The exact pristine pinned source checkout is required")
    commands = json.loads((build / "compile_commands.json").read_text())
    if any("BOXEDWINE_ZLIB" in token for entry in commands for token in parts(entry)):
        raise RuntimeError("This rootfs-free fixture requires the no-ZIP headless cache")
    work = Path(tempfile.mkdtemp(prefix="protondesktop-xwire-input-", dir=args.ram_parent))
    print(f"Work directory: {work}", flush=True)
    overlay = work / "overlay"
    # Only the generated XWire units are used below. Kernel/header overlays are
    # deliberately not on the include path of the unmodified headless cache.
    prepare_wine_session(source, work / "unused-profile", overlay)
    desktop_info = prepare_xwire_desktop(source, overlay)
    baseline = work / "baseline"
    if args.check_baseline:
        shutil.copytree(overlay / "source/x11wire", baseline)
    input_info = prepare_xwire_input(overlay, source)
    fixed = overlay / "source/x11wire"

    base = parts(commands[0])
    base = base[:base.index("-o")]
    include_dirs = [source / value for value in ["source/io", "source/x11", "source/x11wire",
                    "source/sdl", "source", "source/kernel", "platform/sdl"]]
    common = [base[0]] + ["-I" + str(path) for path in include_dirs] + base[1:]
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
    host_source.write_text(host.replace(entry, "// Wire probe owns main; actual SDL host retained."))
    host_object = work / "knativesystem.o"
    execute(common + ["-c", str(host_source), "-o", str(host_object)], work / "compile-host.log")
    fixture_object = work / "probe.o"
    fixture_source = ROOT / "tools/runtime/XWireDesktopInputProbe.cpp"
    execute(common + ["-c", str(fixture_source), "-o", str(fixture_object)], work / "compile-probe.log")

    sdl = next(line.split("=", 1)[1] for line in (build / "CMakeCache.txt").read_text().splitlines()
               if line.startswith("SDL2_RUNTIME:FILEPATH="))
    environment = os.environ.copy()
    environment.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
                       SDL_RENDER_DRIVER="software", BW64_XWIRE="1")
    results = {}
    variants = [("baseline", baseline)] if args.check_baseline else []
    variants.append(("fixed", fixed))
    for label, directory in variants:
        replacements = {"platform/sdl/knativesystem.cpp": str(host_object)}
        # Quoted sibling includes select these complete shadow headers. Rebuild
        # every allocation/access TU after adding the connection protocol mutex.
        flags = [common[0], "-I" + str(directory)] + common[1:]
        for name in REQUIRED_TUS:
            obj = work / f"{label}-{name}.o"
            execute(flags + ["-c", str(directory / name), "-o", str(obj)],
                    work / f"compile-{label}-{name}.log")
            replacements["source/x11wire/" + name] = str(obj)
        objects = [str(fixture_object)]
        seen, reused = set(), 0
        for command in commands:
            path = Path(command["file"])
            relative = path.relative_to(source).as_posix() if path.is_relative_to(source) else ""
            if relative == "source/sdl/main.cpp":
                continue
            if relative in replacements:
                objects.append(replacements[relative])
                seen.add(relative)
            else:
                obj = object_path(command)
                if not obj.is_file():
                    raise RuntimeError(f"Build the actual headless core first: missing {obj}")
                objects.append(str(obj))
                reused += 1
        if seen != set(replacements):
            raise RuntimeError("Missing real XWire/SDL cache units: " + str(set(replacements) - seen))
        executable = work / f"xwire-input-{label}"
        execute([base[0], "-Wl,--gc-sections", "-pthread", "-o", str(executable)] + objects + [sdl, "-lm"],
                work / f"link-{label}.log")
        result = execute([str(executable)], work / f"run-{label}.log", timeout=20,
                         env=environment, require_success=False)
        passes, failures = result.stdout.count("PASS:"), result.stdout.count("FAIL:")
        qualified = (result.returncode == 0 and passes == 33 and failures == 0
                     and "REAL_WIRE_INPUT_RESULT failed=0" in result.stdout) if label == "fixed" else (
                         result.returncode == 1 and
                         "FAIL: second client receives each motion/button/key event once" in result.stdout)
        results[label] = {"host_exit": result.returncode, "passed_checks": passes,
                          "failed_checks": failures, "qualified": qualified,
                          "binary_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                          "reused_real_objects": reused, "log": str(work / f"run-{label}.log")}
        print(f"{label}: exit={result.returncode}, passes={passes}, failures={failures}", flush=True)
    report = {"source_commit": pin, "scope": "actual SDL and X11 wire events across two client sockets",
              "work_directory": str(work), "results": results,
              "desktop_overlay": desktop_info, "input_overlay": input_info,
              "fixture_sha256": hashlib.sha256(fixture_source.read_bytes()).hexdigest(),
              "complete_xwire_header_and_all_three_units_rebuilt": True,
              "concurrency_stress_tested": False, "asan_instrumented": False,
              "wine_executed": False, "uwp_tested": False, "xbox_tested": False,
              "upstream_modified": False, "passed": all(item["qualified"] for item in results.values())}
    (work / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    if not report["passed"]:
        raise RuntimeError(f"Real XWire input regression failed; inspect {work / 'result.json'}")


if __name__ == "__main__":
    main()
