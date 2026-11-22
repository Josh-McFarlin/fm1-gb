#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 The fm1-gb authors
"""The ROM packers' tests: Python's (gb/tools/fm1gb.py) against the reference packager (tools/fm1pkg_make.py) and the
browser's (gb/web/fm1gb.js, through gb/web/pack.mjs) against Python's, byte for byte; a raw ROM, a ROM whose banks are
compressed, and one that fits only compressed; the compressed banks decompressed back. Needs gb/build/fm-gb.fwsc and
gb/build/fm-gb.bin (tools/build.py gb) and the test cartridge (gb/tools/mkdemo.py); node for the browser's half."""
import random
import shutil
import subprocess
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(HERE))
import fm1gb              # noqa: E402
import fm1pkg_make as pkg  # noqa: E402

B = ROOT / "gb" / "build"
T = B / "test"
quiet = lambda *a: None  # noqa: E731


def big_rom():
    """a 1 MiB MBC1 cartridge: the test cartridge's bank 0, then banks that compress well, some not at all"""
    demo = bytearray((T / "demo.gb").read_bytes())
    demo[0x148] = 5                                     # 1 MiB
    demo[0x14D] = (-sum(demo[0x134:0x14D]) - 25) & 0xFF
    rng = random.Random(1)
    rom = bytearray(demo[:0x4000])
    for i in range(1, 64):
        if i % 4 == 0:
            b = bytes(rng.randrange(256) for _ in range(0x4000))            # noise: stays as it is
        else:
            pat = bytes(rng.randrange(256) for _ in range(rng.choice((16, 64, 256))))
            b = (pat * (0x4000 // len(pat) + 1))[:0x4000]
            b = bytes(x ^ (j // 1024 & 3) for j, x in enumerate(b))          # tiles, tables: repeats
        rom += b
    return bytes(rom)


def check_tail(packaged, rom):
    product, files = fm1gb.ufw_parse(packaged)
    fl = fm1gb.Flash(next(d for t, _, d, _ in files if t == 0))
    app = fl.app()
    tr = fm1gb.find_trailer(app)
    assert tr and tr["rom_size"] == len(rom), "no trailer"
    img = app[len(app) - 64 - tr["back"]:][:tr["image_len"]]
    assert zlib.crc32(img) == tr["image_crc"], "the image's CRC"
    if tr["flags"] & fm1gb.GBR_F_TABLE:
        for i in range(len(rom) // 0x4000):
            off, ln = int.from_bytes(img[8 * i:8 * i + 4], "little"), int.from_bytes(img[8 * i + 4:8 * i + 8], "little")
            d = img[off:off + (ln & 0x7FFFFFFF)]
            got = d if ln & fm1gb.GBR_RAW else fm1gb.unlz4(d, 0x4000)
            assert got == rom[i * 0x4000:(i + 1) * 0x4000], f"bank {i}"
        return True
    assert img == rom, "the raw image"
    return False


def main():
    base = (B / "fm-gb.fwsc").read_bytes()
    app = (B / "fm-gb.bin").read_bytes()
    product, files = fm1gb.ufw_parse(base)
    ota = next(d for t, _, d, _ in files if t == 100)
    hi = len(next(d for t, _, d, _ in files if t == 0))
    assert pkg.ufw(pkg.flash_image(app, pkg.KEY, hi), ota, product) == base, "the base package is not app.bin's"
    print("ok   the base package parses back to app.bin")
    demo = fm1gb.read_rom(T / "demo.gb")
    big = big_rom()
    (T / "big.gb").write_bytes(big)
    node = shutil.which("node")
    fails = 0
    for name, rom, force in (("demo", demo, False), ("demo-compressed", demo, True), ("big", big, False)):
        out, st = fm1gb.pack(base, rom, name + ".gb", force, quiet)
        slot = pkg.app_slot(hi)
        end = len(app.rstrip(b"\xFF"))
        tail = fm1gb.slot_tail(rom, slot - ((end + 0xFFF) & ~0xFFF), name + ".gb", force, quiet)
        ref = pkg.ufw(pkg.flash_image(app[:end] + b"\xFF" * (slot - end - len(tail)) + tail, pkg.KEY, hi), ota, product)
        ok = out == ref
        compressed = check_tail(out, rom)
        print(f"{'ok  ' if ok else 'FAIL'} python {name}: the reference's bytes{' (banks compressed)' if compressed else ''}")
        fails += not ok
        (T / f"{name}.fwsc").write_bytes(out)
        (T / f"{name}.gb").write_bytes(rom)
        if node:
            r = subprocess.run([node, str(ROOT / "gb" / "web" / "pack.mjs"), str(B / "fm-gb.fwsc"), str(T / f"{name}.gb"),
                                str(T / f"{name}-js.fwsc")] + (["--compress"] if force else []), capture_output=True, text=True)
            same = r.returncode == 0 and (T / f"{name}-js.fwsc").read_bytes() == out
            print(f"{'ok  ' if same else 'FAIL'} browser {name}: python's bytes {r.stderr.strip()}")
            fails += not same
        # repacking a package that has a game: the old one replaced, the same bytes
        again, _ = fm1gb.pack(out, rom, name + ".gb", force, quiet)
        print(f"{'ok  ' if again == out else 'FAIL'} {name} packed again over itself")
        fails += again != out
    if not node:
        print("note node not found: the browser's packer not checked")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
