"""Writes each reference's number (OpenMW's RefNum: [file in the load order, index]) into converted cell files that
lack it (FractumSeraph/mw3ds online mode: servers address the world's objects and people by it). level.py writes it
itself; this brings data converted before that up to date without converting again.

    python tools/convert/refnums.py            (MW3DS_OUT / MW3DS_TRIBUNAL / MW3DS_BLOODMOON as for the conversion)
"""
import json
import re
import struct
import sys
import zlib

from level import add_refnums
from mwfiles import load_db
from textures import OUT


def read_cell(path):
    data = path.read_bytes()
    packed = data[:4] == b"MWZ1"
    return json.loads(zlib.decompress(data[8:]) if packed else data), packed


def write_cell(path, cell, packed):
    data = json.dumps(cell, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    if packed:
        data = b"MWZ1" + struct.pack("<I", len(data)) + zlib.compress(data, 9)
    path.write_bytes(data)


def main():
    db = load_db()
    interiors = {k.lower(): r for k, r in db["CELL"].items() if isinstance(k, str)}
    done = skipped = 0
    for path in sorted((OUT / "cells").rglob("*.json")):
        try:
            cell, packed = read_cell(path)
        except (ValueError, zlib.error):
            continue
        if not isinstance(cell, dict) or "refs" not in cell:
            continue
        m = re.fullmatch(r"ext_(m?)(\d+)_(m?)(\d+)", path.stem)
        if m:
            rec = db["CELL"].get((-int(m[2]) if m[1] else int(m[2]), -int(m[4]) if m[3] else int(m[4])))
        else:
            rec = interiors.get((cell.get("name") or "").lower())
        if rec is None or not add_refnums(cell["refs"], rec):
            skipped += bool(cell["refs"])
            continue
        write_cell(path, cell, packed)
        done += 1
    print(f"reference numbers written in {done} cells; {skipped} cells not matched", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
