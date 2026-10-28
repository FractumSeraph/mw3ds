"""Packs converted game data for download (FractumSeraph/mw3ds): a program-only MW3DS with no data on the card fetches
these at its first start (source/mwmp/datadownload.cpp), so the CIA a player is given can be the small one.
    python tools/release/make_datapacks.py out/data-bloodmoon build/datapacks/data-v1
    python tools/release/make_datapacks.py <data> <out> --pack-mb 32 --name "Morrowind, Tribunal and Bloodmoon"

Hosted beside a server's own files, for example https://morrowind.fractumseraph.net/mw3ds/data-v1/ (the 3DS asks for
data-v<its data format>/index.json there; sdmc:/3ds/mw3ds/data_url.txt points it elsewhere). The data is Morrowind's,
converted: host it only where you may.

The folder it writes:
    index.json              {"format": 1, "data": <data_version.txt>, "maxTex": <game.json's>, "name", "bytes" (the
                            files' total), "files", "packs": [{"file", "size", "sha256", "bytes", "files"}]}
    NNN-<sha256 12>.mwp     the packs, about --pack-mb each; the name changes with the content, so a server may cache
                            them for good (index.json: never)

A pack (little-endian): b"MWPK", u32 1, then for each file u16 path length, the path (UTF-8, "/"-separated, relative
to the data folder), u32 size, the bytes; a u16 0 ends it. The files are in sorted order, so the same data packs the
same way twice. Earlier packs' names stay when a later file changes size, unless the change moves a pack boundary.
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import zlib
from pathlib import Path


def max_tex(data):
    raw = (data / "game.json").read_bytes()
    if raw[:4] == b"MWZ1":
        raw = zlib.decompress(raw[8:])
    text = raw.decode("utf-8", "replace")
    at = text.find('"max_tex":')
    if at < 0:
        return 0
    digits = ""
    for c in text[at + 10:]:
        if not c.isdigit():
            break
        digits += c
    return int(digits or 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("data", type=Path, help="a converted data folder (out/data-bloodmoon)")
    ap.add_argument("out", type=Path, help="the folder to write (emptied of old packs first)")
    ap.add_argument("--pack-mb", type=int, default=32)
    ap.add_argument("--name", default="Morrowind, Tribunal and Bloodmoon")
    a = ap.parse_args()
    data = a.data.resolve()
    if not (data / "game.json").is_file():
        sys.exit(f"{data} has no game.json: not a converted data folder")
    try:
        version = int((data / "data_version.txt").read_text(encoding="utf-8-sig").strip())
    except (OSError, ValueError):
        sys.exit(f"{data} has no data_version.txt: convert it again (or write the format there by hand)")

    files = []
    for root, dirs, names in os.walk(data):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        for n in names:
            if n.startswith("."):
                continue
            p = Path(root) / n
            files.append((p.relative_to(data).as_posix(), p))
    files.sort()

    a.out.mkdir(parents=True, exist_ok=True)
    for old in a.out.glob("*.mwp"):
        old.unlink()
    limit = a.pack_mb << 20
    packs, total = [], 0
    i = 0
    while i < len(files):
        tmp = a.out / f"{len(packs):03d}.part"
        h = hashlib.sha256()
        size = content = count = 0
        with open(tmp, "wb") as f:
            def put(b):
                nonlocal size
                f.write(b)
                h.update(b)
                size += len(b)
            put(b"MWPK" + struct.pack("<I", 1))
            while i < len(files) and (count == 0 or content < limit):
                rel, p = files[i]
                body = p.read_bytes()
                name = rel.encode("utf-8")
                put(struct.pack("<H", len(name)) + name + struct.pack("<I", len(body)))
                put(body)
                content += len(body)
                count += 1
                i += 1
            put(struct.pack("<H", 0))
        digest = h.hexdigest()
        final = a.out / f"{len(packs):03d}-{digest[:12]}.mwp"
        os.replace(tmp, final)
        packs.append({"file": final.name, "size": size, "sha256": digest, "bytes": content, "files": count})
        total += content
        print(f"{final.name}: {count} files, {size / 1048576:.1f} MB")

    index = {"format": 1, "data": version, "maxTex": max_tex(data), "name": a.name, "bytes": total,
             "files": len(files), "packs": packs}
    (a.out / "index.json").write_text(json.dumps(index, indent=1) + "\n", encoding="utf-8")
    print(f"{a.out / 'index.json'}: data format {version}, {len(files)} files, {total / 1048576:.1f} MB "
          f"in {len(packs)} packs")


if __name__ == "__main__":
    main()
