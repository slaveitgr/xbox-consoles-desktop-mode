#!/usr/bin/env python3
"""Build pinned stock Wine AMD64 PE audit artifacts; never run a guest PE.

SPDX-License-Identifier: MIT
Host configure probes and trusted Wine build tools execute. No package install,
Wine application, DLL initialization, make install/test, or deployment occurs.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

from pe_inventory import inventory

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
LOCK = json.loads((HERE / "source-lock.json").read_text(encoding="utf-8"))
WARNING = "NOT SAFE TO EXECUTE: stock Wine PE evidence; no qualified guest TEB/PEB/accessor or backend."
CONFIGURE_ARGS = ["--enable-win64", "--enable-archs=x86_64", "--disable-tests",
                  "--without-x", "--without-wayland", "--without-opengl", "--without-vulkan"]


class BuildError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify(path: Path, expected: str) -> None:
    if not path.is_file() or sha256(path) != expected:
        raise BuildError(f"Missing input or SHA-256 mismatch: {path}")


def write_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def extract_source(archive: Path, destination: Path) -> None:
    """Extract verified archive regular files and its one known local symlink."""
    prefix = "wine-" + LOCK["wine"]["commit"]
    total = 0
    with tarfile.open(archive, "r:gz") as source:
        members = source.getmembers()
        if len(members) > 25000:
            raise BuildError("Source archive member bound exceeded")
        for member in members:
            parts = Path(member.name).parts
            if not parts or parts[0] != prefix or ".." in parts or Path(member.name).is_absolute():
                raise BuildError("Unexpected source archive path")
            relative = Path(*parts[1:])
            target = destination / relative
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                total += member.size
                if member.size > 64 * 1024 * 1024 or total > 512 * 1024 * 1024:
                    raise BuildError("Source archive expansion bound exceeded")
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.extractfile(member) as data, target.open("wb") as output:
                    shutil.copyfileobj(data, output)
                target.chmod(member.mode & 0o777)
                os.utime(target, (member.mtime, member.mtime))
            elif member.issym() and relative.as_posix() == "tools/gdbinit.py.in" and member.linkname == "gdbinit.py":
                target.parent.mkdir(parents=True, exist_ok=True)
                target.symlink_to("gdbinit.py")
            else:
                raise BuildError(f"Unexpected non-regular source member: {member.name}")


def ensure_alias(link: Path, target: Path) -> None:
    if link.is_symlink():
        if link.resolve() == target.resolve():
            return
        raise BuildError(f"Existing alias has a different target: {link}")
    if link.exists():
        raise BuildError(f"Alias path already exists: {link}")
    link.symlink_to(target, target_is_directory=target.is_dir())


def target_for_library(source: Path, library: str) -> str | None:
    """Translate a file-level PE dependency to a literal pinned Wine module."""
    if not re.fullmatch(r"[a-zA-Z0-9_][a-zA-Z0-9_.+-]*", library):
        return None
    name = library.lower()
    directory = name[:-4] if name.endswith(".dll") else name
    makefile = source / "dlls" / directory / "Makefile.in"
    if not makefile.is_file():
        return None
    match = re.search(r"^MODULE\s*=\s*(\S+)\s*$", makefile.read_text(encoding="utf-8"), re.M)
    if not match or match[1].lower() != name:
        return None
    return f"dlls/{directory}/x86_64-windows/{match[1]}"


class Runner:
    def __init__(self, root: Path, env: dict[str, str]):
        self.root, self.env = root, env

    def run(self, args: list[str], cwd: Path, log_name: str) -> None:
        log = self.root / "logs" / log_name
        record = {"argv": args, "cwd": str(cwd), "log": str(log), "started_utc_epoch": time.time()}
        print(f"Running {log_name}", flush=True)
        with log.open("w", encoding="utf-8") as output:
            completed = subprocess.run(args, cwd=cwd, env=self.env, stdout=output, stderr=subprocess.STDOUT)
        record["returncode"] = completed.returncode
        with (self.root / "commands.jsonl").open("a", encoding="utf-8") as output:
            output.write(json.dumps(record) + "\n")
        if completed.returncode:
            raise BuildError(f"Build step failed ({completed.returncode}); inspect {log}")


def build(args) -> dict:
    if sys.platform != "linux" or os.uname().machine != "x86_64":
        raise BuildError("This driver supports a native x86_64 Linux build host only")
    verify(args.source_archive, LOCK["wine"]["archive_sha256"])
    verify(args.autoconf_deb, LOCK["autoconf"]["sha256"])
    for tool in ("gcc", "g++", "make", "perl", "m4", "bison", "flex", "dpkg-deb", "pkg-config"):
        if shutil.which(tool) is None:
            raise BuildError(f"Required host build tool is unavailable: {tool}; nothing was installed")
    toolchain = args.toolchain_bin.resolve()
    compiler_hashes = {}
    for name, expected in LOCK["llvm"]["sha256"].items():
        tool = toolchain / name
        compiler_hashes[name] = sha256(tool)
        if compiler_hashes[name] != expected and not args.allow_unpinned_toolchain:
            raise BuildError(f"Unpinned {name}; use the recorded SDK or explicitly select --allow-unpinned-toolchain")

    root = args.build_root.resolve()
    marker = root / ".stock-wine-pe-build.json"
    if root.exists() and any(root.iterdir()) and not marker.is_file():
        raise BuildError("Refusing to reuse a nonempty directory without this driver's build marker")
    root.mkdir(parents=True, exist_ok=True)
    if marker.is_file() and json.loads(marker.read_text())["source_sha256"] != LOCK["wine"]["archive_sha256"]:
        raise BuildError("Existing build marker belongs to a different source archive")
    write_json(marker, {"source_sha256": LOCK["wine"]["archive_sha256"], "warning": WARNING})
    (root / "NOT-SAFE-TO-EXECUTE.txt").write_text(WARNING + "\n", encoding="utf-8")
    for directory in ("logs", "build", "tool-aliases", "deps", "autoconf-cache", "cache"):
        (root / directory).mkdir(exist_ok=True)
    alias_name = "xwindows-wine-pe-" + hashlib.sha256(str(root).encode()).hexdigest()[:12]
    alias = Path(tempfile.gettempdir()) / alias_name
    ensure_alias(alias, root)
    source = alias / "source-work"
    source_marker = root / "source-extracted.json"
    source_work_reused = source_marker.is_file()
    if not source_marker.is_file():
        if source.exists():
            raise BuildError("Existing source work tree has no extraction provenance")
        source.mkdir()
        extract_source(args.source_archive, source)
        write_json(source_marker, {"archive_sha256": LOCK["wine"]["archive_sha256"], "stock_source_at_extraction": True})

    tools = alias / "tool-aliases"
    for name, target in (("clang", "clang"), ("clang++", "clang++"), ("lld-link", "lld"),
                         ("ld.lld", "lld"), ("llvm-ar", "llvm-ar"), ("llvm-ranlib", "llvm-ar"),
                         ("llvm-dlltool", "llvm-ar"), ("llvm-strip", "llvm-strip")):
        ensure_alias(tools / name, toolchain / target)
    env = dict(os.environ)
    env["PATH"] = str(tools) + os.pathsep + os.environ.get("PATH", os.defpath)
    env["XDG_CACHE_HOME"] = str(alias / "cache")
    env["x86_64_CC"] = str(tools / "clang")
    runner = Runner(root, env)
    write_json(root / "build-provenance.json", {
        "schema": 1, "status": "building", "warning": WARNING,
        "source_lock": LOCK, "toolchain_sha256": compiler_hashes,
        "source_archive_sha256": LOCK["wine"]["archive_sha256"],
        "autoconf_deb_sha256": LOCK["autoconf"]["sha256"],
        "host_build_tools_executed": True, "guest_code_executed": False,
        "system_packages_installed": False, "deployed": False,
        "guest_header_patch_applied_by_driver": False,
        "source_work_verification": "reuse_bytes_not_reverified" if source_work_reused else "fresh_verified_archive_extraction",
    })
    autoconf = alias / "deps" / "autoconf"
    if not (autoconf / "usr/bin/autoconf").is_file():
        runner.run(["dpkg-deb", "-x", str(args.autoconf_deb.resolve()), str(autoconf)], alias, "extract-autoconf.log")
    macrodir = autoconf / "usr/share/autoconf"
    config = (macrodir / "autom4te.cfg").read_text().replace("/usr/share/autoconf", str(macrodir))
    (root / "autom4te.cfg").write_text(config)
    wrapper = tools / "autom4te"
    wrapper.write_text(f'#!/bin/sh\nexec "{autoconf}/usr/bin/autom4te" --cache="{alias}/autoconf-cache" "$@"\n')
    wrapper.chmod(0o755)
    env.update({"autom4te_perllibdir": str(macrodir), "AC_MACRODIR": str(macrodir),
                "AUTOM4TE_CFG": str(alias / "autom4te.cfg"), "AUTOM4TE": str(wrapper),
                "trailer_m4": str(macrodir / "autoconf/trailer.m4")})
    runner.run([str(autoconf / "usr/bin/autoconf"), "--output=configure", "configure.ac"], source, "autoconf.log")
    runner.run([str(autoconf / "usr/bin/autoheader"), "configure.ac"], source, "autoheader.log")
    runner.run(["perl", "tools/make_requests"], source, "make-requests.log")
    runner.run(["perl", "tools/make_specfiles"], source, "make-specfiles.log")
    runner.run([sys.executable, "dlls/winevulkan/make_vulkan", "-x", "vk.xml", "-X", "video.xml"], source, "make-vulkan.log")
    build_dir = alias / "build"
    runner.run([str(source / "configure"), "--srcdir=" + str(source), *CONFIGURE_ARGS], build_dir, "configure.log")

    targets = set(LOCK["seeds"])
    missing = []
    report = None
    for iteration in range(16):
        runner.run(["make", f"-j{args.jobs}", *sorted(targets)], build_dir, f"make-pe-{iteration:02}.log")
        report = inventory([build_dir / target for target in sorted(targets)])
        write_json(root / "pe-inventory.json", report)
        if report["counts"]["errors"]:
            raise BuildError("PE inventory contains invalid/unreadable outputs; inspect pe-inventory.json")
        present = {Path(target).name.lower() for target in targets}
        absent = sorted(set(report["dependency_libraries"]) - present)
        additions = set()
        missing = []
        for library in absent:
            target = target_for_library(source, library)
            if target:
                additions.add(target)
            else:
                missing.append(library)
        if not additions:
            break
        targets.update(additions)
        if len(targets) > 256:
            raise BuildError("PE dependency-closure module bound exceeded")
    else:
        raise BuildError("PE dependency closure did not stabilize within 16 build passes")
    closure = {"status": "closed_for_parsed_file_dependencies" if not missing else "incomplete",
               "scope": "Ordinary imports, PE delay imports and all exported forwarder modules; runtime LoadLibrary closure remains unknown",
               "missing_modules": missing, "targets": sorted(targets), "binary_count": len(targets),
               "guest_qualified": False, "safe_to_execute": False}
    write_json(root / "closure.json", closure)
    manifest = {"schema": 1, "warning": WARNING, "status": closure["status"], "source_lock": LOCK,
                "source_archive": str(args.source_archive.resolve()), "source_archive_sha256": LOCK["wine"]["archive_sha256"],
                "autoconf_deb_sha256": LOCK["autoconf"]["sha256"], "toolchain_bin": str(toolchain),
                "toolchain_sha256": compiler_hashes, "toolchain_matches_lock": compiler_hashes == LOCK["llvm"]["sha256"],
                "compiler_version": subprocess.check_output([str(tools / "clang"), "--version"], text=True).splitlines()[0],
                "host_gcc_version": subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0],
                "configure_arguments": CONFIGURE_ARGS, "build_root": str(root), "path_alias": str(alias),
                "source_work_verification": "reuse_bytes_not_reverified" if source_work_reused else "fresh_verified_archive_extraction",
                "host_build_tools_executed": True, "guest_code_executed": False, "system_packages_installed": False,
                "deployed": False, "guest_header_patch_applied_by_driver": False, "bit_reproducibility_proven": False}
    write_json(root / "build-provenance.json", manifest)
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-archive", type=Path, required=True)
    parser.add_argument("--autoconf-deb", type=Path, required=True)
    parser.add_argument("--toolchain-bin", type=Path, default=REPO / "artifacts/toolchains/emsdk/upstream/bin")
    parser.add_argument("--build-root", type=Path, default=REPO / "artifacts/stock-wine-pe-repro")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--allow-unpinned-toolchain", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32:
        parser.error("--jobs must be between 1 and 32")
    try:
        result = build(args)
    except (BuildError, OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Stock Wine PE build failed: {error}\nNo guest execution or deployment was requested.\n")
    print(f"{result['status']}: {args.build_root.resolve()}")
    print(WARNING)
    if result["status"] == "incomplete":
        parser.exit(2, "Parsed file dependency closure is incomplete; inspect closure.json.\n")


if __name__ == "__main__":
    main()
