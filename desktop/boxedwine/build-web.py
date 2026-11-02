#!/usr/bin/env python3
"""Build the pinned Boxedwine interpreter; never launch Wine or a guest program."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

HERE = Path(__file__).resolve().parent
PINS = json.loads((HERE / "pins.json").read_text())


def command(*args: object, cwd: Path | None = None, env: dict | None = None) -> str:
    result = subprocess.run([str(a) for a in args], cwd=cwd, env=env,
                            check=True, text=True, stdout=subprocess.PIPE)
    return result.stdout.strip()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fetch", action="store_true", help="Fetch the exact pinned source if absent")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--rootfs", type=Path, help="Optional statically verified, pinned Wine filesystem")
    options = parser.parse_args()
    source, output = options.source.resolve(), options.output.resolve()
    if any(c.isspace() for c in str(source)):
        parser.error("Upstream Make requires a source path without whitespace, such as /tmp/xcdm-boxedwine-source")
    if source == output or source in output.parents or output in source.parents:
        parser.error("Source and output must be separate directory trees")
    if not 1 <= options.jobs <= 32:
        parser.error("--jobs must be between 1 and 32")
    engine = PINS["engine"]
    if not source.exists():
        if not options.fetch:
            parser.error("Source does not exist; supply --fetch to fetch the pinned commit")
        source.mkdir(parents=True)
        command("git", "init", source)
        command("git", "-C", source, "fetch", "--depth", "1", engine["repository"], engine["commit"])
        command("git", "-C", source, "checkout", "--detach", "FETCH_HEAD")
    if command("git", "-C", source, "rev-parse", "HEAD") != engine["commit"]:
        parser.error("Source HEAD does not match pins.json")
    if command("git", "-C", source, "status", "--porcelain", "--untracked-files=normal"):
        parser.error("Source contains modifications or untracked inputs")
    if command("emcc", "-dumpversion") != engine["emscripten"]:
        parser.error("Activate Emscripten " + engine["emscripten"])
    project = source / "project" / "emscripten"
    environment = os.environ.copy()
    for name in ("EMCC_CFLAGS", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "MAKEFLAGS",
                 "MFLAGS", "EXTRA_CPP_FLAGS", "EXTRA_LD_FLAGS", "RELEASE_FLAGS"):
        environment.pop(name, None)
    environment["SOURCE_DATE_EPOCH"] = engine["source_date_epoch"]
    # Command-line overrides cannot inherit a JIT/MT target or commercial demo ZIPs.
    flags = ["release", f"-j{options.jobs}", "WASM_EXCEPTIONS_FLAG=-fwasm-exceptions",
             "TAIL_CALL_FLAG=", "RELEASE_FLAGS=", "ZIP_ASSET_FILES=",
             "RUNTIME_MEMORY_FLAGS=-sINITIAL_MEMORY=134217728 -sALLOW_MEMORY_GROWTH -sMAXIMUM_MEMORY=1073741824"]
    print("Building pinned single-threaded Boxedwine interpreter", flush=True)
    subprocess.run(["make", *flags], cwd=project, env=environment, check=True)
    build = project / "Build" / "Release"
    files = ["boxedwine.html", "boxedwine.js", "boxedwine.wasm", "boxedwine.css",
             "boxedwine-shell.js", "fsfs.js"]
    if (build / "webgl-counters.mjs").exists():
        files.append("webgl-counters.mjs")
    if output.exists() and any(output.iterdir()):
        parser.error("Output directory must be absent or empty")
    output.mkdir(parents=True, exist_ok=True)
    for name in files:
        shutil.copy2(build / name, output / name)
    html = (output / "boxedwine.html").read_text()
    marker = "<script src=boxedwine.js async>"
    if html.count(marker) != 1:
        parser.error("Unexpected generated shell HTML; cannot insert the Xbox presentation wrapper")
    (output / "boxedwine.html").write_text(html.replace(marker, "<script src=xbox-shell.js></script>" + marker))
    shutil.copy2(HERE / "xbox-shell.js", output / "xbox-shell.js")
    notices = output / "licenses"
    notices.mkdir()
    for relative in ["license.txt", "lib/simde/COPYING", "lib/softfloat/COPYING.txt",
                     "lib/zlib/README"]:
        shutil.copy2(source / relative, notices / relative.replace("/", "_"))
    # Preserve the MIT notice on the Emscripten filesystem adapter verbatim.
    (notices / "fsfs-MIT-notice.txt").write_text((source / "project/emscripten/fsfs.js").read_text().split("*/", 1)[0] + "*/\n")
    emcc_path = Path(shutil.which("emcc")).resolve()
    emscripten_license = emcc_path.parent / "LICENSE"
    if not emscripten_license.is_file():
        parser.error("Could not locate the activated Emscripten runtime license")
    shutil.copy2(emscripten_license, notices / "Emscripten-LICENSE")
    cache = Path(os.environ.get("EM_CACHE", str(emcc_path.parent / "cache")))
    for pattern, name in [("ports/sdl2/*/LICENSE.txt", "SDL2-runtime-LICENSE.txt"),
                          ("ports/zlib/*/LICENSE", "zlib-runtime-LICENSE.txt")]:
        candidates = list(cache.glob(pattern))
        if len(candidates) != 1:
            parser.error("Could not identify exactly one activated runtime license: " + pattern)
        shutil.copy2(candidates[0], notices / name)
    shutil.copy2(HERE / "pins.json", output / "pins.json")
    shutil.copy2(HERE / "README.md", output / "SOURCE-NOTICE.md")
    rootfs_pin = PINS.get("wine_filesystem")
    if options.rootfs:
        if not rootfs_pin:
            parser.error("No reviewed Wine filesystem is pinned")
        rootfs = options.rootfs.resolve()
        digest = hashlib.sha256(rootfs.read_bytes()).hexdigest()
        if rootfs.stat().st_size != rootfs_pin["size"] or digest != rootfs_pin["sha256"]:
            parser.error("Wine filesystem does not match pins.json")
        with zipfile.ZipFile(rootfs) as archive:
            if archive.testzip() is not None:
                parser.error("Wine filesystem CRC verification failed")
            for name, expected in rootfs_pin["required_files"].items():
                if hashlib.sha256(archive.read(name)).hexdigest() != expected:
                    parser.error("Wine Explorer component hash mismatch: " + name)
            required = ["opt/wine/lib/wine/i386-unix/explorer.exe.so",
                        "opt/wine/lib/wine/i386-unix/shell32.dll.so"]
            if not all(archive.read(name).startswith(b"\x7fELF\x01") for name in required):
                parser.error("Missing actual ELF32 Wine Explorer implementation")
        shutil.copy2(HERE / "licenses" / "Wine-LGPL-2.1.txt", notices / "Wine-LGPL-2.1.txt")
        shutil.copy2(rootfs, output / "wine-explorer.zip")
    manifest = {"schema": 1, "engine": engine, "compiler": command("emcc", "--version"),
                "command": ["make", *flags], "wine_filesystem_included": bool(options.rootfs),
                "guest_executed": False, "files": {}}
    for file in sorted(output.rglob("*")):
        if file.is_file():
            manifest["files"][str(file.relative_to(output))] = {
                "size": file.stat().st_size, "sha256": hashlib.sha256(file.read_bytes()).hexdigest()}
    (output / "build-info.json").write_text(json.dumps(manifest, indent=2) + "\n")
    subprocess.run([sys.executable, str(HERE / "verify-payload.py"), str(output)], check=True)
    print("Built and verified static payload at " + str(output))


if __name__ == "__main__":
    main()
