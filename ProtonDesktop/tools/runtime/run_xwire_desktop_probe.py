#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the production desktop compositor with real scene/framebuffer objects.

Uses an existing Linux headless build only for its pinned compiler flags. No
cached objects are linked, and no Wine, rootfs, SDL window, UWP, or Xbox runs.
The actual generated server and self-authored scene fixture are both compiled
under ASan/UBSan. The isolated link excludes vptr checks because unrelated
kernel RTTI is intentionally absent after dead-section elimination.
"""
from pathlib import Path
import argparse
import importlib.util
import json
import platform
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def execute(command, log, timeout=120):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=timeout)
    log.write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); inspect {log}\n{result.stdout[-5000:]}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "external/Boxedwine64-full")
    parser.add_argument("--headless-build", type=Path, required=True)
    parser.add_argument("--ram-parent", type=Path, default=Path("/dev/shm"))
    args = parser.parse_args()
    if platform.system() != "Linux":
        raise RuntimeError("The isolated sanitizer scene probe requires Linux")
    source, build = args.source.resolve(), args.headless_build.resolve()
    work = Path(tempfile.mkdtemp(prefix="protondesktop-desktop-scene-", dir=args.ram_parent))
    report = {"status": "incomplete", "scope": "real generated XWire compositor scene",
              "wine_executed": False, "uwp_executed": False, "xbox_executed": False,
              "cached_objects_linked": False, "sanitizers": ["address", "undefined (except vptr)"],
              "work_directory": str(work)}
    try:
        script = work / "prepare.cmake"
        script.write_text(f'''include("{ROOT.as_posix()}/tools/uwp-core/prepare_profile_overlay.cmake")
proton_prepare_uwp_profile_overlay("{source.as_posix()}" "{work.as_posix()}/overlay")
include("{ROOT.as_posix()}/tools/runtime/prepare_wine_session.cmake")
proton_prepare_wine_session("{source.as_posix()}" "{work.as_posix()}/overlay" sources)
''')
        execute(["cmake", "-P", str(script)], work / "prepare.log")
        spec = importlib.util.spec_from_file_location("desktop_overlay", ROOT / "tools/runtime/prepare_xwire_desktop.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        report["overlay"] = module.prepare_xwire_desktop(source, work / "overlay")
        commands = json.loads((build / "compile_commands.json").read_text())
        base = shlex.split(commands[0]["command"])
        base = base[:base.index("-o")]
        includes = [ROOT, work / "overlay/include", work / "overlay/source/sdl",
                    work / "overlay/source/x11wire"]
        includes += [source / item for item in ("source/io", "source/x11", "source/x11wire",
                     "source/sdl", "source", "source/kernel", "platform/sdl")]
        flags = [base[0]] + ["-I" + str(path) for path in includes]
        flags += [value for value in base[1:] if not value.startswith("-O") and not value.startswith("-fsanitize=")]
        flags += ["-O1", "-g1", "-ffunction-sections", "-fdata-sections",
                  "-fsanitize=address,undefined", "-fno-sanitize=vptr", "-fno-omit-frame-pointer"]
        objects = []
        for index, path in enumerate((work / "overlay/source/x11wire/xwireserver.cpp",
                                      ROOT / "tools/runtime/XWireDesktopSceneProbe.cpp")):
            obj = work / f"scene-{index}.o"
            execute(flags + ["-c", str(path), "-o", str(obj)], work / f"compile-{index}.log")
            objects.append(str(obj))
        binary = work / "desktop-scene"
        execute([base[0], "-fsanitize=address,undefined", "-fno-sanitize=vptr", "-Wl,--gc-sections",
                 "-pthread"] + objects + ["-o", str(binary)], work / "link.log")
        log = execute([str(binary)], work / "scene.log", timeout=15)
        if "scene probe: 15 passed, 0 failed" not in log:
            raise RuntimeError("The actual scene did not report all fifteen checks")
        report.update(status="passed", passed_checks=15, failed_checks=0)
    except Exception as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        (work / "result.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
