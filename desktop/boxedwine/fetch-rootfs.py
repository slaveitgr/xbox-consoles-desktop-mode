#!/usr/bin/env python3
"""Acquire the pinned official Wine image for inspection/staging, without execution."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request
import zipfile

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    options = parser.parse_args()
    pin = json.loads((HERE / "pins.json").read_text())["wine_filesystem"]
    target = options.output.resolve()
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists():
        temporary = target.with_suffix(target.suffix + ".part")
        if temporary.exists():
            raise ValueError("Temporary download already exists: " + str(temporary))
        try:
            digest, count = hashlib.sha256(), 0
            with urllib.request.urlopen(pin["url"], timeout=60) as response, temporary.open("xb") as output:
                if not response.geturl().startswith("https://"):
                    raise ValueError("HTTPS is required for the Wine filesystem")
                while chunk := response.read(1024 * 1024):
                    count += len(chunk)
                    if count > pin["size"]:
                        raise ValueError("Download exceeds pinned size")
                    digest.update(chunk)
                    output.write(chunk)
            if count != pin["size"] or digest.hexdigest() != pin["sha256"]:
                raise ValueError("Wine filesystem size or SHA-256 mismatch")
            temporary.replace(target)
        finally:
            if temporary.exists():
                temporary.unlink()
    if target.stat().st_size != pin["size"] or hashlib.sha256(target.read_bytes()).hexdigest() != pin["sha256"]:
        raise ValueError("Existing Wine filesystem does not match pins.json")
    with zipfile.ZipFile(target) as archive:
        if archive.testzip() is not None:
            raise ValueError("Wine filesystem CRC failed")
        for name, expected in pin["required_files"].items():
            if hashlib.sha256(archive.read(name)).hexdigest() != expected:
                raise ValueError("Wine component hash mismatch: " + name)
    print(json.dumps({"path": str(target), "size": pin["size"], "sha256": pin["sha256"],
                      "static_verification": "passed", "guest_executed": False}, indent=2))


if __name__ == "__main__":
    main()
