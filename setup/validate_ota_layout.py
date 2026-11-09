#!/usr/bin/env python3
"""Validate Xiaomi OTA segment boundaries before using the upstream extractor."""

import hashlib
import json
import struct
import sys
from pathlib import Path


IMAGE = struct.Struct("<IIIHH8I")
SEGMENT = struct.Struct("<IIII32s")


def main(path: Path) -> int:
    size = path.stat().st_size
    with path.open("rb") as handle:
        raw = handle.read(IMAGE.size)
        if len(raw) != IMAGE.size:
            raise ValueError("short image header")
        magic, signature_offset, crc32, file_type, model, *offsets = IMAGE.unpack(raw)
        if magic not in (0x31524448, 0x32524448):
            raise ValueError("invalid image magic")
        if signature_offset <= IMAGE.size or signature_offset + 16 > size:
            raise ValueError("signature outside file")
        handle.seek(signature_offset)
        signature_len = int.from_bytes(handle.read(16), "little")
        if signature_len != size - signature_offset - 16:
            raise ValueError("signature length mismatch")

        names = set()
        spans = []
        segments = []
        for offset in offsets:
            if not offset:
                continue
            if offset < IMAGE.size or offset + SEGMENT.size > signature_offset:
                raise ValueError("segment header outside payload")
            handle.seek(offset)
            seg_magic, flash_addr, length, partition, raw_name = SEGMENT.unpack(
                handle.read(SEGMENT.size))
            name = raw_name.split(b"\0", 1)[0].decode("ascii")
            if not name or name in (".", "..") or "/" in name or "\\" in name:
                raise ValueError("unsafe segment name")
            if name in names:
                raise ValueError("duplicate segment name")
            names.add(name)
            end = offset + SEGMENT.size + length
            if length == 0 or end > signature_offset:
                raise ValueError("segment payload outside payload area")
            spans.append((offset, end))
            handle.seek(offset + SEGMENT.size)
            digest = hashlib.sha256()
            remaining = length
            while remaining:
                chunk = handle.read(min(remaining, 1024 * 1024))
                if not chunk:
                    raise ValueError("truncated segment")
                digest.update(chunk)
                remaining -= len(chunk)
            segments.append({"name": name, "header_offset": offset,
                             "payload_offset": offset + SEGMENT.size,
                             "payload_bytes": length, "payload_sha256": digest.hexdigest(),
                             "flash_address": flash_addr, "partition": partition,
                             "segment_magic": hex(seg_magic)})
        if not segments:
            raise ValueError("no segments")
        spans.sort()
        if any(left[1] > right[0] for left, right in zip(spans, spans[1:])):
            raise ValueError("overlapping segments")
    print(json.dumps({"file": path.name, "bytes": size, "magic": hex(magic),
                      "signature_offset": signature_offset,
                      "signature_bytes": signature_len, "header_crc32": hex(crc32),
                      "file_type": file_type, "model": model,
                      "segments": segments, "layout_check": "PASS"},
                     ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ota", type=Path)
    raise SystemExit(main(parser.parse_args().ota))
