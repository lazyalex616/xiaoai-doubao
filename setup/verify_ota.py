#!/usr/bin/env python3
"""Offline identity check for the OH2P 1.62.2 stock OTA.

This only reads a local file. It never contacts Xiaomi or a speaker, and a
matching digest alone does not prove that flashing is safe.
"""

import argparse
import hashlib
import json
from pathlib import Path
import sys

EXPECTED_NAME = "mico_all_616cd9d93_1.62.2.bin"
EXPECTED_MD5 = "bc81f5b40f3db5e9a9d5943616cd9d93"


def digest_file(path: Path) -> tuple[int, str, str]:
    md5 = hashlib.md5(usedforsecurity=False)
    sha256 = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            size += len(block)
            md5.update(block)
            sha256.update(block)
    return size, md5.hexdigest(), sha256.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ota", type=Path, help="local stock OTA .bin")
    args = parser.parse_args()
    path = args.ota
    if not path.is_file() or path.is_symlink():
        parser.error("OTA must be a regular, non-symlinked local file")

    size, md5, sha256 = digest_file(path)
    accepted = path.name == EXPECTED_NAME and md5 == EXPECTED_MD5
    print(json.dumps({
        "file": str(path.resolve()),
        "bytes": size,
        "md5": md5,
        "sha256": sha256,
        "expected_file": EXPECTED_NAME,
        "expected_md5": EXPECTED_MD5,
        "identity_check": "PASS" if accepted else "FAIL",
        "scope": "local bytes only; not a firmware authenticity signature or flash validation",
    }, ensure_ascii=False, indent=2))
    return 0 if accepted else 2


if __name__ == "__main__":
    sys.exit(main())
