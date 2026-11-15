#!/usr/bin/env python3
"""Fetch the pretrained gdi2m model.

Upstream (github.com/maximmashkov1/gdi2m, MIT) publishes no standalone
weights: the exported graph and its two sidecars exist only as RCDATA
resources inside the Windows release binary. This downloads that binary,
checks its hash, and copies the three blobs out of its resource section.
The binary is parsed, never executed.

    python3 scripts/fetch_model.py [--out models/gdi2m] [--exe local.exe]
"""

import argparse
import hashlib
import struct
import sys
import urllib.request
from pathlib import Path

URL = "https://github.com/maximmashkov1/gdi2m/releases/download/0.1.0/gdi2m_ui.exe"
SHA256 = "231125dc565046f32e66900dc8283828570728109ce386dc51e6d969782a03e3"

RT_RCDATA = 10
# tools/gdi2m/resources.h upstream
RESOURCES = {201: "gdi2m.onnx", 202: "gdi2m.json", 203: "gdi2m_front.bin"}


def rcdata(pe: bytes) -> dict[int, bytes]:
    """Every id-named RT_RCDATA resource in a PE image, keyed by id."""
    if pe[:2] != b"MZ":
        raise ValueError("not a PE image")
    coff = struct.unpack_from("<I", pe, 0x3C)[0] + 4
    if pe[coff - 4 : coff] != b"PE\0\0":
        raise ValueError("not a PE image")
    nsections, opt_size = struct.unpack_from("<H", pe, coff + 2)[0], struct.unpack_from("<H", pe, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", pe, opt)[0]
    dirs = opt + {0x10B: 96, 0x20B: 112}[magic]
    rsrc_rva = struct.unpack_from("<I", pe, dirs + 2 * 8)[0]
    if not rsrc_rva:
        raise ValueError("image has no resource section")

    sections = [struct.unpack_from("<IIII", pe, opt + opt_size + 40 * i + 8) for i in range(nsections)]

    def offset(rva: int) -> int:
        for vsize, vaddr, rawsize, rawptr in sections:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                return rva - vaddr + rawptr
        raise ValueError(f"rva {rva:#x} is outside every section")

    base = offset(rsrc_rva)

    def entries(directory: int):
        named, ids = struct.unpack_from("<HH", pe, base + directory + 12)
        for i in range(named + ids):
            name, target = struct.unpack_from("<II", pe, base + directory + 16 + 8 * i)
            yield name, target & 0x7FFFFFFF, bool(target >> 31)

    found = {}
    for rtype, names, is_dir in entries(0):
        if rtype != RT_RCDATA or not is_dir:
            continue
        for rid, langs, is_dir in entries(names):
            if rid >> 31 or not is_dir:
                continue
            _, leaf, _ = next(entries(langs))
            rva, size = struct.unpack_from("<II", pe, base + leaf)
            start = offset(rva)
            found[rid] = pe[start : start + size]
    return found


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", type=Path, default=Path("models/gdi2m"))
    ap.add_argument("--exe", type=Path, help="use an already-downloaded gdi2m_ui.exe")
    args = ap.parse_args()

    if args.exe:
        pe = args.exe.read_bytes()
    else:
        print(f"downloading {URL}", file=sys.stderr)
        with urllib.request.urlopen(URL) as r:
            pe = r.read()
    digest = hashlib.sha256(pe).hexdigest()
    if digest != SHA256:
        print(f"sha256 mismatch: got {digest}, pinned {SHA256}", file=sys.stderr)
        return 1

    blobs = rcdata(pe)
    missing = [name for rid, name in RESOURCES.items() if rid not in blobs]
    if missing:
        print(f"release binary carries no {', '.join(missing)}", file=sys.stderr)
        return 1

    args.out.mkdir(parents=True, exist_ok=True)
    for rid, name in RESOURCES.items():
        (args.out / name).write_bytes(blobs[rid])
        print(f"{args.out / name}  {len(blobs[rid]):>10,d} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
