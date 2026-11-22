#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 The fm1-gb authors
"""FM-GB: put your own Game Boy ROM into the FM-GB firmware, and install it on an FM-1.

  gb/tools/fm1gb.py install [ROM.gb]       pick a ROM (a file dialog when none is given), make the package, install it
  gb/tools/fm1gb.py pack ROM.gb [-o OUT.fwsc] [--base FM-GB.fwsc]
                                           the package with the ROM in it, nothing installed
  gb/tools/fm1gb.py info PACKAGE.fwsc      what game a package holds
  gb/tools/fm1gb.py image ROM.gb -o OUT.bin [--compress]
                                           the slot's tail alone (image and trailer): the host simulator's FM1GB_IMAGE

The base is the emulator's own package, with no game in it: --base, else gb/build/fm-gb.fwsc (built by
`tools/build.py gb` when it is not there yet and the JieLi toolchain is). Making the package needs nothing else: no
toolchain, no SDK, just Python 3. The ROM goes at the end of the app slot, after the emulator's code, with a 64-byte
trailer at the slot's very end that the firmware reads at its start (gb/rom.c). A ROM bigger than the room has its
banks LZ4-compressed, as many as it takes, the best-compressing first; the firmware decompresses them into a cache as
the game maps them.

Your ROM stays on your computer and in your FM-1: nothing is uploaded anywhere, and no ROM comes with FM-GB."""
import argparse
import os
import struct
import subprocess
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import fm1pkg_make as pkg   # noqa: E402  (crc16, the ciphers, entries, ufw: the package's format)

GBR_MAGIC = 0x42474D46     # "FMGB"
GBR_VERSION = 1
GBR_F_TABLE = 1
GBR_RAW = 0x80000000
TRAILER = 64
BANK = 0x4000
UFW_BLOCKS, UFW_BLK, UFW_KEEP = 20, 0x30, 0x2F
APP_HEAD = pkg.APP_HEAD


# ----------------------------------------------------------------------------------------------- LZ4 ---
def lz4(src, depth=48):
    """LZ4 block format (no frame), hash chains with lazy matching: offline, so it may take its time"""
    n, out, anchor, i = len(src), bytearray(), 0, 0
    head, prev = {}, [0] * n
    limit = n - 12                                  # (the format's tail: the last match starts 12 bytes before the end)

    def insert(p):
        k = src[p:p + 4]
        prev[p] = head.get(k, -1)
        head[k] = p

    def best(p):
        if p >= limit:
            return 0, 0
        k, c, ml, mo, d = src[p:p + 4], head.get(src[p:p + 4], -1), 0, 0, depth
        while c >= 0 and d and p - c <= 65535:
            if src[c:c + 4] == k:
                m = 4
                end = n - 5
                while p + m < end and src[c + m] == src[p + m]:
                    m += 1
                if m > ml:
                    ml, mo = m, p - c
            c, d = prev[c], d - 1
        return ml, mo

    def lenbytes(v):
        b = bytearray()
        while v >= 255:
            b.append(255)
            v -= 255
        b.append(v)
        return b

    while i < limit:
        ml, mo = best(i)
        if ml < 4:
            insert(i)
            i += 1
            continue
        insert(i)
        ml2, mo2 = best(i + 1)                      # lazy: a longer match one byte on wins
        if ml2 > ml + 1:
            i += 1
            insert(i)
            ml, mo = ml2, mo2
        lit = src[anchor:i]
        out.append(min(len(lit), 15) << 4 | min(ml - 4, 15))
        if len(lit) >= 15:
            out += lenbytes(len(lit) - 15)
        out += lit
        out += bytes((mo & 0xFF, mo >> 8))
        if ml - 4 >= 15:
            out += lenbytes(ml - 4 - 15)
        for p in range(i + 1, min(i + ml, limit)):
            insert(p)
        i += ml
        anchor = i
    lit = src[anchor:]
    out.append(min(len(lit), 15) << 4)
    if len(lit) >= 15:
        out += lenbytes(len(lit) - 15)
    out += lit
    return bytes(out)


def unlz4(blk, size):
    out, i = bytearray(), 0
    while i < len(blk):
        tok = blk[i]
        i += 1
        ll = tok >> 4
        if ll == 15:
            while True:
                b = blk[i]
                i += 1
                ll += b
                if b != 255:
                    break
        out += blk[i:i + ll]
        i += ll
        if i >= len(blk):
            break
        off = blk[i] | blk[i + 1] << 8
        i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = blk[i]
                i += 1
                ml += b
                if b != 255:
                    break
        for _ in range(ml + 4):
            out.append(out[-off])
    if len(out) != size:
        raise ValueError("lz4: a bank did not decompress whole")
    return bytes(out)


# ------------------------------------------------------------------------------------------- the ROM ---
def read_rom(path):
    rom = Path(path).read_bytes()
    if len(rom) < 0x8000:
        raise SystemExit(f"{path}: {len(rom)} bytes is too small for a Game Boy ROM (32 KiB at least)")
    hdr = rom[0x100:0x150]
    if (-sum(rom[0x134:0x14D]) - 25) & 0xFF != rom[0x14D]:
        print(f"note: {path}: the header's checksum does not match (a homebrew ROM, or a damaged one); going on")
    if rom[0x143] == 0xC0:
        raise SystemExit(f"{path}: a Game Boy Color-only game; FM-GB plays original Game Boy games (and Color games "
                         "that also run on an original Game Boy)")
    t = rom[0x147]
    known = {0x00, 0x01, 0x02, 0x03, 0x05, 0x06, 0x08, 0x09, 0x0F, 0x10, 0x11, 0x12, 0x13,
             0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0xFF}
    if t not in known:
        raise SystemExit(f"{path}: cartridge type {t:#04x} is not supported (ROM only, MBC1, MBC2, MBC3, MBC5 are)")
    del hdr
    rom += b"\xFF" * (-len(rom) % BANK)            # whole banks
    return rom


def title_of(rom):
    t = rom[0x134:0x144]
    if rom[0x143] & 0x80:
        t = t[:11]
    return t.split(b"\0")[0].decode("ascii", "replace").strip()


def make_image(rom, room, force_compress=False, log=print):
    """-> (image, flags): the ROM as it is when it fits room, else a bank table and the banks, compressed as needed"""
    if len(rom) <= room and not force_compress:
        return rom, 0
    banks = [rom[i:i + BANK] for i in range(0, len(rom), BANK)]
    table = 8 * len(banks)
    log(f"the ROM is {len(rom) // 1024} KiB, the room {room // 1024} KiB: compressing banks...")
    packed = []
    for i, b in enumerate(banks):
        z = lz4(b)
        if unlz4(z, BANK) != b:
            raise SystemExit("fm1gb: the LZ4 compressor failed its own check (a bug: please report it)")
        packed.append(z)
        if log is print:
            print(f"\r  bank {i + 1}/{len(banks)}", end="", flush=True)
    if log is print:
        print()
    raw = [True] * len(banks)
    total = table + len(rom)
    # compress the banks that gain the most first (bank 0 stays as it is: the game reads it all the time)
    order = sorted(range(1, len(banks)), key=lambda i: len(packed[i]) - BANK)
    for i in order:
        if total <= room and not force_compress:
            break
        if len(packed[i]) < BANK:
            raw[i] = False
            total -= BANK - len(packed[i])
    if total > room:
        raise SystemExit(f"fm1gb: the ROM does not fit, even compressed: {total // 1024} KiB for "
                         f"{room // 1024} KiB of room")
    body, ents, off = bytearray(), [], table
    for i, b in enumerate(banks):
        d = b if raw[i] else packed[i]
        ents.append((off, len(d) | (GBR_RAW if raw[i] else 0)))
        body += d
        off += len(d)
        if off & 3:                                 # (each bank 4-aligned)
            pad = 4 - (off & 3)
            body += b"\xFF" * pad
            off += pad
    n_packed = raw.count(False)
    log(f"  {n_packed} of {len(banks)} banks compressed: {total // 1024} KiB in {room // 1024} KiB")
    return b"".join(struct.pack("<II", *e) for e in ents) + bytes(body), GBR_F_TABLE


def trailer(back, image, rom_size, flags, name):
    nm = name.encode("ascii", "replace")[:31].ljust(32, b"\0")
    t = struct.pack("<7I", GBR_MAGIC, GBR_VERSION, back, len(image), rom_size, zlib.crc32(image), flags) + nm
    return t + struct.pack("<I", zlib.crc32(t))


def slot_tail(rom, room, name, force_compress=False, log=print):
    """the image and the trailer, as they end the slot (room: the bytes free for both)"""
    image, flags = make_image(rom, room - TRAILER - 16, force_compress, log)
    pad = -len(image) % 16
    tail = image + b"\xFF" * pad
    return tail + trailer(len(tail), image, len(rom), flags, name)


# ------------------------------------------------------------------------------------------ the package ---
def ufw_parse(raw):
    """a .fwsc -> (product, [(type, name, data, extra)])"""
    if len(raw) < UFW_BLOCKS * UFW_BLK:
        raise SystemExit("not an FM-1 package (too short)")
    product, logical = "", bytearray()
    for i in range(UFW_BLOCKS):
        m = raw[i * UFW_BLK + UFW_KEEP]
        if m != 0x7D:
            product += chr((m - i - 1) & 0xFF)
        logical += raw[i * UFW_BLK:i * UFW_BLK + UFW_KEEP]
    logical += raw[UFW_BLOCKS * UFW_BLK:]
    h = bytearray(logical[:0x40])
    pkg.enc(h, 0, 0x40)
    if pkg.crc16(h[2:0x40]) != struct.unpack_from("<H", h, 0)[0]:
        raise SystemExit("not an FM-1 package (its header's checksum)")
    n = struct.unpack_from("<H", h, 8)[0]
    files = []
    for i in range(n):
        e = bytearray(logical[0x40 + i * 0x50:0x90 + i * 0x50])
        pkg.enc(e, 0, 0x50)
        typ, _, crc, _, off, ln, _ = struct.unpack_from("<HHHHIII", e, 0)
        data = bytes(logical[off:off + ln])
        if pkg.crc16(data) != crc:
            raise SystemExit(f"the package's part {i} is damaged (its checksum)")
        name = e[0x40:0x50].split(b"\0")[0].decode("ascii", "replace")
        extra = struct.unpack_from("<I", e, pkg.RES_AT)[0] if typ == pkg.RES_TYPE else 0
        files.append((typ, name, data, extra))
    return product, files


class Flash:
    """flash.bin's app area, deciphered: its directory, the slot, and the way back"""

    def __init__(self, flash, key=pkg.KEY):
        self.flash, self.key = bytearray(flash), key
        self.app_hi = len(flash)
        self.slot = pkg.app_slot(self.app_hi)
        head = bytearray(self.flash[0x4000:0x4020])
        pkg.sfc(head, 0, 32, 0, key)
        crc, body = struct.unpack_from("<H", head, 0)[0], head[2:32]
        if pkg.crc16(body) != crc or body[-16:].split(b"\0")[0] != b"app_area_head":
            raise SystemExit("the package's app area does not decipher (another chip key? not an fm1-base package?)")
        self.blk = struct.unpack_from("<I", body, 6)[0]
        self.area = bytearray(self.flash[0x4000:0x4000 + self.blk])
        pkg.sfc(self.area, 0, self.blk, 0, key)

    def app(self):
        return bytes(self.area[APP_HEAD:APP_HEAD + self.slot])

    def set_app(self, app):
        assert len(app) == self.slot
        self.area[APP_HEAD:APP_HEAD + self.slot] = app
        # app.bin's entry (the directory's first) and the area's head: their checksums again
        e = self.area[32:64]
        body = bytearray(e[2:])
        struct.pack_into("<H", body, 0, pkg.crc16(app))
        self.area[32:64] = struct.pack("<H", pkg.crc16(body)) + body
        h = bytearray(self.area[0:32])
        body = bytearray(h[2:])
        struct.pack_into("<H", body, 0, pkg.crc16(self.area[32:self.blk]))
        self.area[0:32] = struct.pack("<H", pkg.crc16(body)) + body
        enc = bytearray(self.area)
        pkg.sfc(enc, 0, self.blk, 0, self.key)
        self.flash[0x4000:0x4000 + self.blk] = enc

    def bytes(self):
        return bytes(self.flash)


def find_trailer(app):
    t = app[-TRAILER:]
    magic, ver, back, ilen, rsize, icrc, flags = struct.unpack_from("<7I", t, 0)
    if magic != GBR_MAGIC or zlib.crc32(t[:60]) != struct.unpack_from("<I", t, 60)[0]:
        return None
    return dict(version=ver, back=back, image_len=ilen, rom_size=rsize, image_crc=icrc, flags=flags,
                name=t[28:60].split(b"\0")[0].decode("ascii", "replace"))


def code_end(app):
    """where the emulator's code ends in the slot (the rest 0xFF), any game already there taken out"""
    tr = find_trailer(app)
    if tr:
        app = app[:len(app) - TRAILER - tr["back"]]
    return len(app.rstrip(b"\xFF"))


def pack(base_pkg, rom, name, force_compress=False, log=print):
    product, files = ufw_parse(base_pkg)
    flash = next((d for t, _, d, _ in files if t == 0), None)
    if flash is None:
        raise SystemExit("the base package has no flash.bin")
    fl = Flash(flash)
    app = fl.app()
    end = code_end(app)
    if end < 0x1000:
        raise SystemExit("the base package's app is empty?")
    room = fl.slot - ((end + 0xFFF) & ~0xFFF)       # (a sector of space after the code)
    tail = slot_tail(rom, room, name, force_compress, log)
    new = app[:end] + b"\xFF" * (fl.slot - end - len(tail)) + tail
    fl.set_app(new)
    out = [(t, n, fl.bytes() if t == 0 else d, x) for t, n, d, x in files]
    flash_new = out[0][2] if out[0][0] == 0 else next(d for t, _, d, _ in out if t == 0)
    ota = next(d for t, _, d, _ in out if t == 100)
    res = next(((d, x) for t, _, d, x in out if t == pkg.RES_TYPE), None)
    return pkg.ufw(flash_new, ota, product, res[0] if res else None, res[1] if res else 0), dict(
        product=product, code=end, slot=fl.slot, room=room, tail=len(tail))


def info(raw):
    product, files = ufw_parse(raw)
    flash = next(d for t, _, d, _ in files if t == 0)
    fl = Flash(flash)
    app = fl.app()
    tr = find_trailer(app)
    print(f"package  identity {product}, app slot {fl.slot} B (app_end {fl.app_hi:#x}), code {code_end(app)} B")
    if not tr:
        print("game     none")
        return None
    img = app[len(app) - TRAILER - tr["back"]:][:tr["image_len"]]
    ok = zlib.crc32(img) == tr["image_crc"]
    print(f"game     {tr['name']!r}: ROM {tr['rom_size'] // 1024} KiB, image {tr['image_len']} B"
          f"{' (banks compressed)' if tr['flags'] & GBR_F_TABLE else ''}, {'checks' if ok else 'DAMAGED'}")
    return tr


def base_package(path):
    if path:
        return Path(path)
    p = ROOT / "gb" / "build" / "fm-gb.fwsc"
    if not p.exists():
        print("building FM-GB first (tools/build.py gb)...")
        r = subprocess.run([sys.executable, str(ROOT / "tools" / "build.py"), str(ROOT / "gb")])
        if r.returncode or not p.exists():
            raise SystemExit("fm1gb: no base package: build it (tools/build.py gb, which needs the JieLi toolchain: "
                             "tools/setup_toolchain.sh) or give one with --base")
    return p


def pick_rom():
    try:                                            # a file dialog where there is a desktop
        import tkinter
        from tkinter import filedialog
        root = tkinter.Tk()
        root.withdraw()
        p = filedialog.askopenfilename(title="Choose a Game Boy ROM for FM-GB",
                                       filetypes=[("Game Boy ROMs", "*.gb *.gbc *.GB *.GBC"), ("All files", "*")])
        root.destroy()
        if p:
            return p
    except Exception:
        pass
    p = input("The Game Boy ROM to install (a .gb file): ").strip().strip("'\"")
    if not p:
        raise SystemExit("no ROM chosen")
    return p


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack", help="make a package with your ROM in it")
    p.add_argument("rom")
    p.add_argument("-o", "--out")
    p.add_argument("--base", help="the emulator's package (default gb/build/fm-gb.fwsc)")
    p.add_argument("--compress", action="store_true", help="compress banks even when the ROM fits (a test)")
    p = sub.add_parser("install", help="pick a ROM, make the package and install it on the FM-1")
    p.add_argument("rom", nargs="?")
    p.add_argument("--base")
    p.add_argument("--port", help="the FM-1's MIDI port (tools/fm1_install.py --port)")
    p = sub.add_parser("info", help="what a package holds")
    p.add_argument("package")
    p = sub.add_parser("image", help="the slot's tail alone, for the host simulator (FM1GB_IMAGE)")
    p.add_argument("rom")
    p.add_argument("-o", "--out", required=True)
    p.add_argument("--room", type=lambda s: int(s, 0), default=760 * 1024)
    p.add_argument("--compress", action="store_true")
    a = ap.parse_args()

    if a.cmd == "info":
        info(Path(a.package).read_bytes())
        return 0
    if a.cmd == "image":
        rom = read_rom(a.rom)
        tail = slot_tail(rom, a.room, Path(a.rom).name, a.compress)
        Path(a.out).write_bytes(tail)
        print(f"image    {a.out}: {len(tail)} B")
        return 0
    rom_path = a.rom or pick_rom()
    rom = read_rom(rom_path)
    base = base_package(a.base)
    title = title_of(rom) or Path(rom_path).stem
    out, st = pack(base.read_bytes(), rom, Path(rom_path).name, getattr(a, "compress", False))
    dst = Path(getattr(a, "out", None) or (ROOT / "gb" / "build" / (Path(rom_path).stem + ".fwsc")))
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(out)
    print(f"game     {title} ({len(rom) // 1024} KiB) from {rom_path}")
    print(f"package  {dst}  ({st['code'] // 1024} KiB of emulator, {st['tail'] // 1024} KiB of game, "
          f"{(st['room'] - st['tail']) // 1024} KiB to spare)")
    if a.cmd == "install":
        cmd = [sys.executable, str(ROOT / "tools" / "fm1_install.py"), str(dst)]
        if a.port:
            cmd += ["--port", a.port]
        print("installing: " + " ".join(cmd))
        return subprocess.call(cmd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
