#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 The fm1-gb authors
"""Make demo.gb: a tiny original Game Boy program for FM-GB's tests (no one's game, so it can live in the repository).

  gb/tools/mkdemo.py OUT.gb

A block on a checkered field moves with the pad; A plays a beep (channel 1), B a noise (channel 4); START adds one to
a counter kept in the cartridge's battery RAM (MBC1 + RAM + battery), shown at the top left, so a save and a reload
can be checked. Written with the small assembler below (just the instructions it needs)."""
import re
import sys

R8 = {"b": 0, "c": 1, "d": 2, "e": 3, "h": 4, "l": 5, "(hl)": 6, "a": 7}
R16 = {"bc": 0, "de": 1, "hl": 2, "sp": 3}
CC = {"nz": 0, "z": 1, "nc": 2, "c": 3}
ALU = {"add": 0, "adc": 1, "sub": 2, "sbc": 3, "and": 4, "xor": 5, "or": 6, "cp": 7}


class Asm:
    def __init__(self, org):
        self.org, self.out, self.labels, self.fix = org, bytearray(), {}, []

    def pc(self):
        return self.org + len(self.out)

    def num(self, s, pos=None, size=None, rel=False):
        s = s.strip()
        try:
            return int(s, 0)
        except ValueError:
            self.fix.append((len(self.out) if pos is None else pos, s, size, rel, self.pc()))
            return 0

    def emit(self, *bs):
        self.out += bytes(b & 0xFF for b in bs)

    def line(self, ln):
        ln = ln.split(";")[0].strip()
        if not ln:
            return
        m = re.match(r"^(\w+):\s*(.*)$", ln)
        if m:
            self.labels[m[1]] = self.pc()
            return self.line(m[2])
        op, _, rest = ln.partition(" ")
        args = [a.strip().lower() if not re.match(r"^[\"']", a.strip()) else a.strip() for a in rest.split(",")] if rest else []
        getattr(self, "op_" + op.lower())(args)

    # --- instructions
    def op_db(self, a):
        for x in a:
            if x[0] in "\"'":
                self.out += x[1:-1].encode()
            else:
                self.emit(self.num(x))

    def op_nop(self, a): self.emit(0x00)
    def op_di(self, a): self.emit(0xF3)
    def op_ei(self, a): self.emit(0xFB)
    def op_halt(self, a): self.emit(0x76)
    def op_cpl(self, a): self.emit(0x2F)
    def op_ret(self, a): self.emit(0xC9 if not a else 0xC0 | CC[a[0]] << 3)

    def op_ld(self, a):
        d, s = a
        if d in R8 and s in R8:
            return self.emit(0x40 | R8[d] << 3 | R8[s])
        if d in R8 and s not in R8 and not s.startswith("("):
            self.emit(0x06 | R8[d] << 3)
            return self.emit(self.num(s, len(self.out), 1))
        if d in R16 and not s.startswith("("):
            self.emit(0x01 | R16[d] << 4)
            v = self.num(s, len(self.out), 2)
            return self.emit(v, v >> 8)
        pairs = {("(bc)", "a"): 0x02, ("(de)", "a"): 0x12, ("(hl+)", "a"): 0x22, ("(hl-)", "a"): 0x32,
                 ("a", "(bc)"): 0x0A, ("a", "(de)"): 0x1A, ("a", "(hl+)"): 0x2A, ("a", "(hl-)"): 0x3A}
        if (d, s) in pairs:
            return self.emit(pairs[(d, s)])
        if d == "a" and s.startswith("("):
            self.emit(0xFA)
            v = self.num(s[1:-1], len(self.out), 2)
            return self.emit(v, v >> 8)
        if s == "a" and d.startswith("("):
            self.emit(0xEA)
            v = self.num(d[1:-1], len(self.out), 2)
            return self.emit(v, v >> 8)
        raise SyntaxError(f"ld {d},{s}")

    def op_ldh(self, a):
        d, s = a
        if d == "a":
            self.emit(0xF0, self.num(s[1:-1]) & 0xFF)
        else:
            self.emit(0xE0, self.num(d[1:-1]) & 0xFF)

    def op_inc(self, a):
        r = a[0]
        self.emit(0x04 | R8[r] << 3 if r in R8 else 0x03 | R16[r] << 4)

    def op_dec(self, a):
        r = a[0]
        self.emit(0x05 | R8[r] << 3 if r in R8 else 0x0B | R16[r] << 4)

    def alu(self, k, a):
        s = a[-1]
        if s in R8:
            return self.emit(0x80 | ALU[k] << 3 | R8[s])
        self.emit(0xC6 | ALU[k] << 3)
        self.emit(self.num(s, len(self.out), 1))

    def op_add(self, a): self.alu("add", a)
    def op_sub(self, a): self.alu("sub", a)
    def op_and(self, a): self.alu("and", a)
    def op_or(self, a): self.alu("or", a)
    def op_xor(self, a): self.alu("xor", a)
    def op_cp(self, a): self.alu("cp", a)

    def op_swap(self, a): self.emit(0xCB, 0x30 | R8[a[0]])
    def op_bit(self, a): self.emit(0xCB, 0x40 | int(a[0]) << 3 | R8[a[1]])

    def op_jr(self, a):
        self.emit(0x18 if len(a) == 1 else 0x20 | CC[a[0]] << 3)
        self.fix.append((len(self.out), a[-1], 1, True, self.pc() + 1))
        self.emit(0)

    def op_jp(self, a):
        self.emit(0xC3 if len(a) == 1 else 0xC2 | CC[a[0]] << 3)
        v = self.num(a[-1], len(self.out), 2)
        self.emit(v, v >> 8)

    def op_call(self, a):
        self.emit(0xCD)
        v = self.num(a[-1], len(self.out), 2)
        self.emit(v, v >> 8)

    def link(self):
        for pos, name, size, rel, pc in self.fix:
            v = eval(name, {"__builtins__": {}}, dict(self.labels))   # (a label, or labels and numbers: "end - start")
            if rel:
                d = v - pc
                assert -128 <= d <= 127, f"jr {name}: too far"
                self.out[pos] = d & 0xFF
            elif size == 1:
                self.out[pos] = v & 0xFF
            else:
                self.out[pos], self.out[pos + 1] = v & 0xFF, v >> 8


PROGRAM = """
start:  di
        ld sp, 0xFFFE
wait1:  ldh a, (0x44)
        cp 144
        jr c, wait1
        xor a
        ldh (0x40), a           ; LCD off
        ld hl, 0x8000           ; tiles: blank, checker, block, then the digits at 16
        ld de, tiles
        ld bc, tiles_end - tiles
        call copy
        ld hl, 0x9800           ; the field: the checker everywhere
        ld bc, 0x0400
fill:   ld a, 1
        ld (hl+), a
        dec bc
        ld a, b
        or c
        jr nz, fill
        ld hl, 0xFE00           ; OAM cleared
        ld b, 160
oclr:   xor a
        ld (hl+), a
        dec b
        jr nz, oclr
        ld a, 80
        ld (posy), a
        ld (posx), a
        ld a, 0xE4
        ldh (0x47), a           ; BGP
        ldh (0x48), a           ; OBP0
        ld a, 0x80
        ldh (0x26), a           ; sound on
        ld a, 0x77
        ldh (0x24), a
        ld a, 0xFF
        ldh (0x25), a
        ld a, 0x0A              ; the counter from the cartridge's RAM ("Z" at A001 says it is one)
        ld (0x0000), a
        ld a, (0xA001)
        cp 0x5A
        ld a, 0
        jr nz, nosave
        ld a, (0xA000)
nosave: ld (count), a
        xor a
        ld (0x0000), a
        ld (prev), a
        ld a, 0x93
        ldh (0x40), a           ; LCD on: BG, sprites, tiles at 0x8000
main:   ldh a, (0x44)           ; wait for VBlank
        cp 144
        jr nz, main
        call pad
        ld b, a
        ld a, (prev)
        cpl
        and b
        ld c, a                 ; c: newly pressed, b: held
        ld a, b
        ld (prev), a
        bit 0, b
        jr z, n_r
        ld a, (posx)
        inc a
        ld (posx), a
n_r:    bit 1, b
        jr z, n_l
        ld a, (posx)
        dec a
        ld (posx), a
n_l:    bit 2, b
        jr z, n_u
        ld a, (posy)
        dec a
        ld (posy), a
n_u:    bit 3, b
        jr z, n_d
        ld a, (posy)
        inc a
        ld (posy), a
n_d:    bit 4, c
        jr z, n_a
        xor a                   ; A: a beep on channel 1
        ldh (0x10), a
        ld a, 0x80
        ldh (0x11), a
        ld a, 0xF3
        ldh (0x12), a
        ld a, 0x00
        ldh (0x13), a
        ld a, 0x87
        ldh (0x14), a
n_a:    bit 5, c
        jr z, n_b
        xor a                   ; B: a noise on channel 4
        ldh (0x20), a
        ld a, 0xF2
        ldh (0x21), a
        ld a, 0x55
        ldh (0x22), a
        ld a, 0x80
        ldh (0x23), a
n_b:    bit 7, c
        jr z, n_s
        ld a, (count)           ; START: one more, saved
        inc a
        ld (count), a
        ld b, a
        ld a, 0x0A
        ld (0x0000), a
        ld a, b
        ld (0xA000), a
        ld a, 0x5A
        ld (0xA001), a
        xor a
        ld (0x0000), a
n_s:    ld a, (posy)            ; the block
        add a, 16
        ld (0xFE00), a
        ld a, (posx)
        add a, 8
        ld (0xFE01), a
        ld a, 2
        ld (0xFE02), a
        xor a
        ld (0xFE03), a
        ld a, (count)           ; the counter in three digits at the top left
        ld b, 0
hund:   cp 100
        jr c, h_done
        sub 100
        inc b
        jr hund
h_done: ld c, a
        ld a, b
        add a, 16
        ld (0x9821), a
        ld a, c
        ld b, 0
tens:   cp 10
        jr c, t_done
        sub 10
        inc b
        jr tens
t_done: ld c, a
        ld a, b
        add a, 16
        ld (0x9822), a
        ld a, c
        add a, 16
        ld (0x9823), a
vbl:    ldh a, (0x44)           ; out of VBlank before the next frame
        cp 144
        jr nc, vbl
        jp main
pad:    ld a, 0x20              ; -> a: R L U D A B SELECT START, a bit each, 1 = held
        ldh (0x00), a
        ldh a, (0x00)
        ldh a, (0x00)
        cpl
        and 0x0F
        ld b, a
        ld a, 0x10
        ldh (0x00), a
        ldh a, (0x00)
        ldh a, (0x00)
        cpl
        and 0x0F
        swap a
        or b
        ld b, a
        ld a, 0x30
        ldh (0x00), a
        ld a, b
        ret
copy:   ld a, (de)              ; bc bytes from de to hl
        ld (hl+), a
        inc de
        dec bc
        ld a, b
        or c
        jr nz, copy
        ret
"""

FONT = {  # 5 x 7 digits, a row a byte
    0: [0x70, 0x88, 0x98, 0xA8, 0xC8, 0x88, 0x70], 1: [0x20, 0x60, 0x20, 0x20, 0x20, 0x20, 0x70],
    2: [0x70, 0x88, 0x08, 0x10, 0x20, 0x40, 0xF8], 3: [0xF8, 0x10, 0x20, 0x10, 0x08, 0x88, 0x70],
    4: [0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x10], 5: [0xF8, 0x80, 0xF0, 0x08, 0x08, 0x88, 0x70],
    6: [0x30, 0x40, 0x80, 0xF0, 0x88, 0x88, 0x70], 7: [0xF8, 0x08, 0x10, 0x20, 0x40, 0x40, 0x40],
    8: [0x70, 0x88, 0x88, 0x70, 0x88, 0x88, 0x70], 9: [0x70, 0x88, 0x88, 0x78, 0x08, 0x10, 0x60],
}


def tiles():
    t = bytearray(16)                                           # 0: blank
    t += bytes(x for r in range(8) for x in ((0x55, 0x00) if r & 1 else (0xAA, 0x00)))   # 1: checker (shade 1)
    t += bytes([0xFF, 0xFF] * 8)                                # 2: the block (shade 3)
    t += bytes(16 * 13)                                         # 3..15
    for d in range(10):                                         # 16..25: the digits (shade 3 on 0)
        rows = FONT[d] + [0]
        t += bytes(x for r in rows for x in (r, r))
    return t


def build():
    a = Asm(0x150)
    for ln in PROGRAM.splitlines():
        a.line(ln)
    a.labels["tiles"] = a.pc()
    data = tiles()
    a.out += data
    a.labels["tiles_end"] = a.pc()
    a.labels.update(posx=0xC000, posy=0xC001, prev=0xC002, count=0xC003)
    a.link()
    rom = bytearray(b"\xFF" * 0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])         # nop; jp 0x150
    rom[0x104:0x134] = bytes(48)                               # (no logo: FM-GB has no boot ROM to check it)
    rom[0x134:0x144] = b"FMGB DEMO".ljust(16, b"\0")
    rom[0x144:0x14D] = bytes([0, 0, 0, 0x03, 0x00, 0x02, 0x01, 0x00, 0x00])   # MBC1+RAM+BATTERY, 32 KiB, 8 KiB
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - (0x14D - 0x134)) & 0xFF
    rom[0x150:0x150 + len(a.out)] = a.out
    s = (sum(rom) - rom[0x14E] - rom[0x14F]) & 0xFFFF
    rom[0x14E:0x150] = bytes([s >> 8, s & 0xFF])
    return bytes(rom)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    open(sys.argv[1], "wb").write(build())
