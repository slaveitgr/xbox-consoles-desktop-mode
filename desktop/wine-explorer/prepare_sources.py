#!/usr/bin/env python3
"""Stage the exact Wine Explorer source selection without executing its code.

SPDX-License-Identifier: MIT
The staged upstream sources retain the licenses recorded in source-lock.json.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import urllib.error
import urllib.request


HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]


def checked_path(project: str, name: str) -> PurePosixPath:
    path = PurePosixPath(name)
    if project not in ("wine", "mihawk") or path.is_absolute() or ".." in path.parts:
        raise ValueError(f"Invalid locked source path: {project}/{name}")
    return path


def validate(data: bytes, entry: dict) -> None:
    if len(data) != entry["size"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
        raise ValueError(f"Source checksum differs: {entry['project']}/{entry['path']}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=HERE / "staged")
    parser.add_argument("--offline", action="store_true", help="Use existing staged files or the pinned Mihawk submodule only")
    args = parser.parse_args()
    lock = json.loads((HERE / "source-lock.json").read_text(encoding="utf-8"))
    if lock["schema"] != 1:
        raise ValueError("Unknown source lock schema")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    def stage(entry: dict) -> int:
        project = entry["project"]
        relative = checked_path(project, entry["path"])
        target = output / project / relative
        if not target.resolve().is_relative_to(output):
            raise ValueError("Staging target leaves the output directory")
        if target.is_file():
            validate(target.read_bytes(), entry)
            return entry["size"]
        if project == "mihawk" and (REPO / "external/MihawkOS").exists():
            # git show uses the locked commit even if the worktree is changed.
            command = ["git", "-C", str(REPO / "external/MihawkOS"), "show",
                       lock["projects"][project]["commit"] + ":" + str(relative)]
            data = subprocess.check_output(command)
        else:
            if args.offline:
                raise FileNotFoundError(f"Missing staged source: {project}/{relative}")
            spec = lock["projects"][project]
            repository = spec["repository"].removeprefix("https://github.com/")
            url = f"https://raw.githubusercontent.com/{repository}/{spec['commit']}/{relative}"
            request = urllib.request.Request(url, headers={"User-Agent": "MihawkXbox-source-staging"})
            with urllib.request.urlopen(request, timeout=45) as response:
                data = response.read(4 * 1024 * 1024 + 1)
        validate(data, entry)
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_name(target.name + ".part")
        temporary.write_bytes(data)
        temporary.replace(target)
        return len(data)

    with ThreadPoolExecutor(max_workers=6) as pool:
        total = sum(pool.map(stage, lock["files"]))
    (output / "provenance.json").write_text(json.dumps({
        "schema": 1,
        "source_lock_sha256": hashlib.sha256((HERE / "source-lock.json").read_bytes()).hexdigest(),
        "projects": lock["projects"],
        "file_count": len(lock["files"]),
        "total_bytes": total,
        "status": "source staged only; no Wine build or Xbox execution established",
    }, indent=2) + "\n", encoding="utf-8")
    print(f"Verified {len(lock['files'])} source files ({total} bytes) in {output}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError, urllib.error.URLError) as error:
        raise SystemExit(str(error)) from error
