#!/usr/bin/env python3
"""Build pinned stock or XCDM Wine AMD64 PE evidence; never run a guest PE.

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
import shlex
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
XCDM_WARNING = "NOT SAFE TO EXECUTE: XCDM Wine PE evidence; guest admission and native providers remain unqualified."
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


def validate_build_identity(existing: dict, variant: str, patch_sha256: str | None) -> None:
    """Never mix stock and patched objects or a different guest patch."""
    if existing.get("source_sha256") != LOCK["wine"]["archive_sha256"]:
        raise BuildError("Existing build marker belongs to a different source archive")
    if existing.get("variant", "stock") != variant or existing.get("guest_patch_sha256") != patch_sha256:
        raise BuildError("Existing build marker belongs to a different variant or guest patch")


def patched_file_hashes(source: Path) -> dict[str, str]:
    return {name: sha256(source / name) for name in LOCK["xcdm"]["patched_files"]}


def verify_reused_patch(source: Path, marker: dict, patch_sha256: str) -> None:
    if marker.get("variant") != "xcdm" or marker.get("guest_patch_sha256") != patch_sha256:
        raise BuildError("Reused source does not carry the required XCDM patch identity")
    if marker.get("patched_files_sha256") != patched_file_hashes(source):
        raise BuildError("A reused patched source file changed; use a fresh XCDM build root")


def validate_compiler_macros(text: str, flags: list[str]) -> dict:
    """Qualify the actual PE compiler mode without executing its output."""
    macros = dict(re.findall(r"^#define[ \t]+(\w+)(?:[ \t]+(.*))?$", text, re.M))
    if int(macros.get("__clang_major__", "0")) < 19 or "_MSC_VER" not in macros:
        raise BuildError("XCDM requires clang >=19 in MSVC mode (_MSC_VER)")
    if macros.get("XCDM_GUEST") != "1" or "USE_COMPILER_EXCEPTIONS" not in macros:
        raise BuildError("XCDM_GUEST=1 and Wine USE_COMPILER_EXCEPTIONS must be effective")
    if "-fasync-exceptions" not in flags:
        raise BuildError("Wine configure did not enable -fasync-exceptions")
    return {name: macros.get(name) for name in ("__clang_major__", "_MSC_VER", "XCDM_GUEST", "USE_COMPILER_EXCEPTIONS")}


def compiler_seh_proof(source: Path, build_dir: Path, root: Path, runner) -> dict:
    text = (build_dir / "Makefile").read_text()
    names = ("x86_64_CC", "x86_64_CFLAGS", "x86_64_EXTRACFLAGS", "CROSSCFLAGS")
    variables = {name: (re.search(r"^" + name + r"\s*=\s*(.*)$", text, re.M) or [None, ""])[1] for name in names}
    flags = shlex.split(" ".join(variables[name] for name in names[1:]))
    command = shlex.split(variables["x86_64_CC"]) + flags + ["-I" + str(source / "include"),
              "-include", "excpt.h", "-dM", "-E", "-x", "c", "/dev/null"]
    runner.run(command, build_dir, "compiler-seh-macros.log")
    macros = validate_compiler_macros((root / "logs/compiler-seh-macros.log").read_text(), flags)
    proof = {"status": "compiler_mode_verified_without_execution", "makefile_variables": variables,
             "command": command, "macros": macros, "header_sha256": sha256(source / "include/excpt.h"),
             "guest_code_executed": False,
             "scope": "Actual configured PE flags and Wine header macro selection; no runtime exception qualification"}
    write_json(root / "compiler-seh-proof.json", proof)
    return proof


def audit_xcdm_selection(source: Path, build_dir: Path, root: Path, targets: set[str], runner) -> dict:
    exclusions = LOCK["xcdm"]["excluded_guest_providers"]
    selected = sorted(target for target in targets if Path(target).name.lower() not in exclusions)
    selection = {"audit_candidate_pe": selected,
                 "excluded_providers": {Path(t).name.lower(): {"path": t, "required_provider": "bridge-or-replacement", "reason": exclusions[Path(t).name.lower()]}
                                        for t in sorted(targets) if Path(t).name.lower() in exclusions},
                 "scope": "Static audit superset, including Bridge-owned kernel32/kernelbase; strict module ownership remains authoritative",
                 "all_gates_blocked": True, "safe_to_execute": False}
    write_json(root / "guest-selection.json", selection)
    tool = REPO / "tools/audit_wine_guest.py"
    output = root / "guest-admission-audit.json"
    command = [sys.executable, str(tool), "binary", *[str(build_dir / target) for target in selected],
               "--wine-source", str(source), "--wine-build-dir", str(build_dir), "--json", str(output)]
    result = runner.run(command, build_dir, "guest-admission-audit.log", allowed_returncodes=(0, 1, 2))
    report = json.loads(output.read_text()) if output.is_file() else {}
    expected = {Path(target).name.lower() for target in selected}
    reported = set(report.get("modules", {}))
    summary = {"tool": str(tool), "tool_sha256": sha256(tool), "command": command, "returncode": result,
               "expected_module_count": len(expected), "reported_module_count": len(reported),
               "missing_modules": sorted(expected - reported), "unexpected_modules": sorted(reported - expected),
               "build_problems": report.get("build"),
               "module_problems": {name: entry.get("problems") for name, entry in report.get("modules", {}).items() if entry.get("problems")},
               "tool_gate_passed": result == 0 and reported == expected,
               "all_gates_blocked": True, "guest_qualified": False, "safe_to_execute": False,
               "scope": "Static audit of a candidate superset only, not a guest admission list; strict ownership and native provider/loader/runtime execution gates remain authoritative"}
    write_json(root / "guest-admission-summary.json", summary)
    return summary


class Runner:
    def __init__(self, root: Path, env: dict[str, str]):
        self.root, self.env = root, env

    def run(self, args: list[str], cwd: Path, log_name: str, allowed_returncodes=(0,)) -> int:
        log = self.root / "logs" / log_name
        record = {"argv": args, "cwd": str(cwd), "log": str(log), "started_utc_epoch": time.time()}
        print(f"Running {log_name}", flush=True)
        with log.open("w", encoding="utf-8") as output:
            completed = subprocess.run(args, cwd=cwd, env=self.env, stdout=output, stderr=subprocess.STDOUT)
        record["returncode"] = completed.returncode
        with (self.root / "commands.jsonl").open("a", encoding="utf-8") as output:
            output.write(json.dumps(record) + "\n")
        if completed.returncode not in allowed_returncodes:
            raise BuildError(f"Build step failed ({completed.returncode}); inspect {log}")
        return completed.returncode


def build(args) -> dict:
    if sys.platform != "linux" or os.uname().machine != "x86_64":
        raise BuildError("This driver supports a native x86_64 Linux build host only")
    verify(args.source_archive, LOCK["wine"]["archive_sha256"])
    verify(args.autoconf_deb, LOCK["autoconf"]["sha256"])
    xcdm = args.variant == "xcdm"
    warning = XCDM_WARNING if xcdm else WARNING
    patch_sha256 = LOCK["xcdm"]["patch_sha256"] if xcdm else None
    if xcdm:
        verify(args.guest_patch, patch_sha256)
    for tool in ("gcc", "g++", "make", "perl", "m4", "bison", "flex", "dpkg-deb", "pkg-config"):
        if shutil.which(tool) is None:
            raise BuildError(f"Required host build tool is unavailable: {tool}; nothing was installed")
    if xcdm and shutil.which("patch") is None:
        raise BuildError("The patch tool is required for an XCDM build; nothing was installed")
    toolchain = args.toolchain_bin.resolve()
    compiler_hashes = {}
    for name, expected in LOCK["llvm"]["sha256"].items():
        tool = toolchain / name
        compiler_hashes[name] = sha256(tool)
        if compiler_hashes[name] != expected and not args.allow_unpinned_toolchain:
            raise BuildError(f"Unpinned {name}; use the recorded SDK or explicitly select --allow-unpinned-toolchain")

    root = args.build_root.resolve()
    marker = root / (".xcdm-wine-pe-build.json" if xcdm else ".stock-wine-pe-build.json")
    if root.exists() and any(root.iterdir()) and not marker.is_file():
        raise BuildError("Refusing to reuse a nonempty directory without this driver's build marker")
    root.mkdir(parents=True, exist_ok=True)
    if marker.is_file():
        validate_build_identity(json.loads(marker.read_text()), args.variant, patch_sha256)
    write_json(marker, {"source_sha256": LOCK["wine"]["archive_sha256"], "variant": args.variant,
                        "guest_patch_sha256": patch_sha256, "warning": warning})
    (root / "NOT-SAFE-TO-EXECUTE.txt").write_text(warning + "\n", encoding="utf-8")
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
        if not xcdm:
            write_json(source_marker, {"archive_sha256": LOCK["wine"]["archive_sha256"], "stock_source_at_extraction": True, "variant": "stock"})

    tools = alias / "tool-aliases"
    for name, target in (("clang", "clang"), ("clang++", "clang++"), ("lld-link", "lld"),
                         ("ld.lld", "lld"), ("llvm-ar", "llvm-ar"), ("llvm-ranlib", "llvm-ar"),
                         ("llvm-dlltool", "llvm-ar"), ("llvm-strip", "llvm-strip")):
        ensure_alias(tools / name, toolchain / target)
    if xcdm:
        ensure_alias(tools / "x86_64-windows-clang", toolchain / "clang")
    env = dict(os.environ)
    env["PATH"] = str(tools) + os.pathsep + os.environ.get("PATH", os.defpath)
    env["XDG_CACHE_HOME"] = str(alias / "cache")
    env["x86_64_CC"] = str(tools / "clang")
    if xcdm:
        env["x86_64_CC"] += " -target x86_64-windows -fuse-ld=lld --no-default-config"
        env["CROSSCFLAGS"] = LOCK["xcdm"]["cross_cflags"]
    runner = Runner(root, env)
    if xcdm:
        if source_work_reused:
            verify_reused_patch(source, json.loads(source_marker.read_text()), patch_sha256)
        else:
            patch = str(args.guest_patch.resolve())
            runner.run(["patch", "--dry-run", "--fuzz=0", "--batch", "--forward", "-p1", "-i", patch], source, "guest-patch-check.log")
            runner.run(["patch", "--fuzz=0", "--batch", "--forward", "-p1", "-i", patch], source, "guest-patch-apply.log")
            write_json(source_marker, {"archive_sha256": LOCK["wine"]["archive_sha256"], "stock_source_at_extraction": True,
                                       "variant": "xcdm", "guest_patch_sha256": patch_sha256,
                                       "patched_files_sha256": patched_file_hashes(source)})
    write_json(root / "build-provenance.json", {
        "schema": 1, "status": "building", "warning": warning, "variant": args.variant,
        "source_lock": LOCK, "toolchain_sha256": compiler_hashes,
        "source_archive_sha256": LOCK["wine"]["archive_sha256"],
        "autoconf_deb_sha256": LOCK["autoconf"]["sha256"],
        "host_build_tools_executed": True, "guest_code_executed": False,
        "system_packages_installed": False, "deployed": False,
        "guest_header_patch_applied_by_driver": xcdm, "guest_patch_sha256": patch_sha256,
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
    proof = compiler_seh_proof(source, build_dir, root, runner) if xcdm else None

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
    audit = audit_xcdm_selection(source, build_dir, root, targets, runner) if xcdm else None
    manifest = {"schema": 1, "warning": warning, "variant": args.variant, "status": closure["status"], "source_lock": LOCK,
                "source_archive": str(args.source_archive.resolve()), "source_archive_sha256": LOCK["wine"]["archive_sha256"],
                "autoconf_deb_sha256": LOCK["autoconf"]["sha256"], "toolchain_bin": str(toolchain),
                "toolchain_sha256": compiler_hashes, "toolchain_matches_lock": compiler_hashes == LOCK["llvm"]["sha256"],
                "compiler_version": subprocess.check_output([str(tools / "clang"), "--version"], text=True).splitlines()[0],
                "host_gcc_version": subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0],
                "configure_arguments": CONFIGURE_ARGS, "build_root": str(root), "path_alias": str(alias),
                "source_work_verification": "reuse_bytes_not_reverified" if source_work_reused else "fresh_verified_archive_extraction",
                "host_build_tools_executed": True, "guest_code_executed": False, "system_packages_installed": False,
                "deployed": False, "guest_header_patch_applied_by_driver": xcdm, "guest_patch_sha256": patch_sha256,
                "compiler_seh_proof": proof, "static_admission_summary": audit,
                "all_execution_gates_blocked": True, "bit_reproducibility_proven": False}
    write_json(root / "build-provenance.json", manifest)
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-archive", type=Path, required=True)
    parser.add_argument("--autoconf-deb", type=Path, required=True)
    parser.add_argument("--toolchain-bin", type=Path, default=REPO / "artifacts/toolchains/emsdk/upstream/bin")
    parser.add_argument("--variant", choices=("stock", "xcdm"), default="stock")
    parser.add_argument("--guest-patch", type=Path, default=REPO / LOCK["xcdm"]["patch"])
    parser.add_argument("--build-root", type=Path)
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--allow-unpinned-toolchain", action="store_true")
    args = parser.parse_args()
    if args.build_root is None:
        args.build_root = REPO / ("artifacts/xcdm-wine-pe-build" if args.variant == "xcdm" else "artifacts/stock-wine-pe-repro")
    if not 1 <= args.jobs <= 32:
        parser.error("--jobs must be between 1 and 32")
    try:
        result = build(args)
    except (BuildError, OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{args.variant} Wine PE build failed: {error}\nNo guest execution or deployment was requested.\n")
    print(f"{result['status']}: {args.build_root.resolve()}")
    print(result["warning"])
    if result["status"] == "incomplete":
        parser.exit(2, "Parsed file dependency closure is incomplete; inspect closure.json.\n")


if __name__ == "__main__":
    main()
