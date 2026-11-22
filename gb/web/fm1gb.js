// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 The fm1-gb authors
//
// FM-GB in the browser: a Game Boy ROM put into the FM-GB package, as gb/tools/fm1gb.py does it (the same bytes, which
// gb/tools/test_fm1gb.py checks). Nothing leaves the page: the ROM is read from the player's own file.
//
//   romInfo(rom)                     the header read: { title, type, mapper, romKiB, ramKiB, cgbOnly, supported, why }
//   pack(base, rom, name, opt)       the base package (Uint8Array, the emulator's fm-gb.fwsc) with the ROM in it -> a new
//                                    .fwsc (Uint8Array); opt.compress forces bank compression, opt.onProgress(i, n)
//   gameOf(pkg)                      the game a package holds: { name, romSize, compressed, ok } or null

const BANK = 0x4000, TRAILER = 64, APP_HEAD = 0x120, APP_TAIL = 0xF24, KEY = 0x980F;
const GBR_MAGIC = 0x42474D46, GBR_VERSION = 1, GBR_F_TABLE = 1, GBR_RAW = 0x80000000;
const UFW_BLOCKS = 20, UFW_BLK = 0x30, UFW_KEEP = 0x2F, RES_TYPE = 101, RES_AT = 0x14;

// ------------------------------------------------------------------------------------------- checksums, ciphers ---
const T16 = new Uint16Array(256);
for (let i = 0; i < 256; i++) {
  let c = i << 8;
  for (let k = 0; k < 8; k++) c = c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1;
  T16[i] = c & 0xFFFF;
}
export function crc16(d, c = 0) {
  for (let i = 0; i < d.length; i++) c = ((c << 8) & 0xFFFF) ^ T16[((c >> 8) ^ d[i]) & 0xFF];
  return c;
}
const T32 = new Uint32Array(256);
for (let i = 0; i < 256; i++) {
  let c = i;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
  T32[i] = c >>> 0;
}
export function crc32(d) {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < d.length; i++) c = T32[(c ^ d[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}
function enc(b, off, n, key = 0xFFFF) {             // JieLi's ENC stream cipher, in place
  for (let i = 0; i < n; i++) {
    b[off + i] ^= key & 0xFF;
    key = ((key << 1) ^ (key & 0x8000 ? 0x1021 : 0)) & 0xFFFF;
  }
}
function sfc(b, off, n, key) {                       // ENC per 32-byte block, the key mixed with the block's address
  for (let i = 0; i < n; i += 32) enc(b, off + i, Math.min(n - i, 32), key ^ ((i >> 2) & 0xFFFF));
}
const rd16 = (b, o) => b[o] | (b[o + 1] << 8);
const rd32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
const wr16 = (b, o, v) => { b[o] = v & 0xFF; b[o + 1] = (v >> 8) & 0xFF; };
const wr32 = (b, o, v) => { b[o] = v & 0xFF; b[o + 1] = (v >>> 8) & 0xFF; b[o + 2] = (v >>> 16) & 0xFF; b[o + 3] = (v >>> 24) & 0xFF; };
const appSlot = (appHi) => appHi - 0x4000 - APP_HEAD - APP_TAIL;

// ------------------------------------------------------------------------------------------------------- LZ4 ---
// the block format, hash chains (depth 48) with lazy matching: gb/tools/fm1gb.py lz4(), byte for byte
export function lz4(src, depth = 48) {
  const n = src.length, out = [], limit = n - 12, head = new Map(), prev = new Int32Array(n);
  const key = (p) => (src[p] | (src[p + 1] << 8) | (src[p + 2] << 16) | (src[p + 3] << 24)) >>> 0;
  const insert = (p) => { const k = key(p); prev[p] = head.has(k) ? head.get(k) : -1; head.set(k, p); };
  const best = (p) => {
    if (p >= limit) return [0, 0];
    const k = key(p);
    let c = head.has(k) ? head.get(k) : -1, ml = 0, mo = 0, d = depth;
    while (c >= 0 && d && p - c <= 65535) {
      if (key(c) === k) {
        let m = 4;
        const end = n - 5;
        while (p + m < end && src[c + m] === src[p + m]) m++;
        if (m > ml) { ml = m; mo = p - c; }
      }
      c = prev[c]; d--;
    }
    return [ml, mo];
  };
  const lenBytes = (v) => { while (v >= 255) { out.push(255); v -= 255; } out.push(v); };
  let anchor = 0, i = 0;
  while (i < limit) {
    let [ml, mo] = best(i);
    if (ml < 4) { insert(i); i++; continue; }
    insert(i);
    const [ml2, mo2] = best(i + 1);
    if (ml2 > ml + 1) { i++; insert(i); ml = ml2; mo = mo2; }
    const lit = i - anchor;
    out.push((Math.min(lit, 15) << 4) | Math.min(ml - 4, 15));
    if (lit >= 15) lenBytes(lit - 15);
    for (let p = anchor; p < i; p++) out.push(src[p]);
    out.push(mo & 0xFF, mo >> 8);
    if (ml - 4 >= 15) lenBytes(ml - 4 - 15);
    for (let p = i + 1; p < Math.min(i + ml, limit); p++) insert(p);
    i += ml;
    anchor = i;
  }
  const lit = n - anchor;
  out.push(Math.min(lit, 15) << 4);
  if (lit >= 15) lenBytes(lit - 15);
  for (let p = anchor; p < n; p++) out.push(src[p]);
  return Uint8Array.from(out);
}

export function unlz4(blk, size) {
  const out = new Uint8Array(size);
  let i = 0, o = 0;
  while (i < blk.length) {
    const tok = blk[i++];
    let ll = tok >> 4;
    if (ll === 15) { let b; do { b = blk[i++]; ll += b; } while (b === 255); }
    for (let k = 0; k < ll; k++) out[o++] = blk[i++];
    if (i >= blk.length) break;
    const off = blk[i] | (blk[i + 1] << 8);
    i += 2;
    let ml = tok & 15;
    if (ml === 15) { let b; do { b = blk[i++]; ml += b; } while (b === 255); }
    for (let k = 0; k < ml + 4; k++, o++) out[o] = out[o - off];
  }
  if (o !== size) throw new Error("lz4: a bank did not decompress whole");
  return out;
}

// -------------------------------------------------------------------------------------------------- the ROM ---
const MAPPERS = { 0x00: "ROM", 0x01: "MBC1", 0x02: "MBC1+RAM", 0x03: "MBC1+RAM+BATTERY", 0x05: "MBC2", 0x06: "MBC2+BATTERY",
  0x08: "ROM+RAM", 0x09: "ROM+RAM+BATTERY", 0x0F: "MBC3+TIMER+BATTERY", 0x10: "MBC3+TIMER+RAM+BATTERY", 0x11: "MBC3",
  0x12: "MBC3+RAM", 0x13: "MBC3+RAM+BATTERY", 0x19: "MBC5", 0x1A: "MBC5+RAM", 0x1B: "MBC5+RAM+BATTERY",
  0x1C: "MBC5+RUMBLE", 0x1D: "MBC5+RUMBLE+RAM", 0x1E: "MBC5+RUMBLE+RAM+BATTERY", 0xFF: "HuC1" };

export function romInfo(rom) {
  if (rom.length < 0x8000) return { supported: false, why: "too small for a Game Boy ROM (32 KiB at least)" };
  const cgb = rom[0x143], type = rom[0x147];
  let title = "";
  for (let i = 0x134; i < ((cgb & 0x80) ? 0x13F : 0x144) && rom[i]; i++) title += String.fromCharCode(rom[i] < 32 || rom[i] > 126 ? 63 : rom[i]);
  const ramKiB = [0, 2, 8, 32, 128, 64][rom[0x149]] || 0;
  const info = { title: title.trim(), type, mapper: MAPPERS[type] || `type 0x${type.toString(16)}`, romKiB: Math.ceil(rom.length / 1024),
                 ramKiB: (type === 5 || type === 6) ? 0.5 : ramKiB, cgbOnly: cgb === 0xC0, color: !!(cgb & 0x80), supported: true, why: "" };
  let sum = 0;
  for (let i = 0x134; i < 0x14D; i++) sum = (sum - rom[i] - 1) & 0xFF;
  info.headerOk = sum === rom[0x14D];
  if (info.cgbOnly) Object.assign(info, { supported: false, why: "a Game Boy Color-only game: FM-GB plays original Game Boy games (and Color games that also run on one)" });
  else if (!(type in MAPPERS)) Object.assign(info, { supported: false, why: `cartridge type 0x${type.toString(16)} is not supported (ROM only, MBC1, MBC2, MBC3, MBC5 are)` });
  return info;
}

function padBanks(rom) {
  const n = Math.ceil(rom.length / BANK) * BANK, out = new Uint8Array(n).fill(0xFF);
  out.set(rom);
  return out;
}

function makeImage(rom, room, force, onProgress) {
  if (rom.length <= room && !force) return { image: rom, flags: 0 };
  const nb = rom.length / BANK, packed = [];
  for (let i = 0; i < nb; i++) {
    const b = rom.subarray(i * BANK, (i + 1) * BANK), z = lz4(b);
    packed.push(z);
    if (onProgress) onProgress(i + 1, nb);
  }
  const raw = new Array(nb).fill(true);
  let total = 8 * nb + rom.length;
  const order = [...Array(nb).keys()].slice(1).sort((a, b) => (packed[a].length - packed[b].length) || (a - b));
  for (const i of order) {
    if (total <= room && !force) break;
    if (packed[i].length < BANK) { raw[i] = false; total -= BANK - packed[i].length; }
  }
  if (total > room) throw new Error(`the ROM does not fit, even compressed: ${total >> 10} KiB for ${room >> 10} KiB of room`);
  const parts = [], table = new Uint8Array(8 * nb);
  let off = 8 * nb;
  for (let i = 0; i < nb; i++) {
    const d = raw[i] ? rom.subarray(i * BANK, (i + 1) * BANK) : packed[i];
    wr32(table, 8 * i, off);
    wr32(table, 8 * i + 4, (d.length | (raw[i] ? GBR_RAW : 0)) >>> 0);
    parts.push(d);
    off += d.length;
    if (off & 3) { const pad = 4 - (off & 3); parts.push(new Uint8Array(pad).fill(0xFF)); off += pad; }
  }
  const image = new Uint8Array(off);
  image.set(table);
  let at = 8 * nb;
  for (const p of parts) { image.set(p, at); at += p.length; }
  return { image, flags: GBR_F_TABLE };
}

function slotTail(rom, room, name, force, onProgress) {
  const { image, flags } = makeImage(rom, room - TRAILER - 16, force, onProgress);
  const pad = (16 - (image.length % 16)) % 16, tail = new Uint8Array(image.length + pad + TRAILER).fill(0xFF);
  tail.set(image);
  const t = tail.subarray(image.length + pad);
  t.fill(0);
  wr32(t, 0, GBR_MAGIC); wr32(t, 4, GBR_VERSION); wr32(t, 8, image.length + pad); wr32(t, 12, image.length);
  wr32(t, 16, rom.length); wr32(t, 20, crc32(image)); wr32(t, 24, flags);
  const nm = name.slice(0, 31);
  for (let i = 0; i < nm.length; i++) { const c = nm.charCodeAt(i); t[28 + i] = c < 128 ? c : 63; }
  wr32(t, 60, crc32(t.subarray(0, 60)));
  return tail;
}

// ---------------------------------------------------------------------------------------------- the package ---
function ufwParse(raw) {
  if (raw.length < UFW_BLOCKS * UFW_BLK) throw new Error("not an FM-1 package (too short)");
  let product = "";
  const logical = new Uint8Array(raw.length - UFW_BLOCKS);
  for (let i = 0; i < UFW_BLOCKS; i++) {
    const m = raw[i * UFW_BLK + UFW_KEEP];
    if (m !== 0x7D) product += String.fromCharCode((m - i - 1) & 0xFF);
    logical.set(raw.subarray(i * UFW_BLK, i * UFW_BLK + UFW_KEEP), i * UFW_KEEP);
  }
  logical.set(raw.subarray(UFW_BLOCKS * UFW_BLK), UFW_BLOCKS * UFW_KEEP);
  const h = logical.slice(0, 0x40);
  enc(h, 0, 0x40);
  if (crc16(h.subarray(2, 0x40)) !== rd16(h, 0)) throw new Error("not an FM-1 package (its header's checksum)");
  const files = [];
  for (let i = 0, n = rd16(h, 8); i < n; i++) {
    const e = logical.slice(0x40 + i * 0x50, 0x90 + i * 0x50);
    enc(e, 0, 0x50);
    const typ = rd16(e, 0), off = rd32(e, 8), len = rd32(e, 12), data = logical.slice(off, off + len);
    if (crc16(data) !== rd16(e, 4)) throw new Error(`the package's part ${i} is damaged`);
    files.push({ typ, data, extra: typ === RES_TYPE ? rd32(e, RES_AT) : 0 });
  }
  return { product, files };
}

function ufwBuild(files, product) {                 // tools/fm1pkg_make.py ufw()
  let off = 0x400;
  const ents = [];
  files.forEach((f, i) => {
    off = (off + 0xFF) & ~0xFF;
    const e = new Uint8Array(0x50);
    wr16(e, 0, f.typ); wr16(e, 2, i); wr16(e, 4, crc16(f.data)); wr16(e, 6, 0);
    wr32(e, 8, off); wr32(e, 12, f.data.length); wr32(e, 16, (f.data.length + 31) & ~31);
    if (f.typ === RES_TYPE) wr32(e, RES_AT, f.extra);
    const name = f.typ === 0 ? "flash.bin" : f.typ === 100 ? "ota.bin" : "res.bin";
    for (let k = 0; k < name.length; k++) e[0x40 + k] = name.charCodeAt(k);
    ents.push({ e, off, data: f.data });
    off += f.data.length;
  });
  const total = off, hdr = new Uint8Array(0x40), lst = new Uint8Array(0x50 * ents.length);
  wr32(hdr, 4, total); wr16(hdr, 8, files.length); wr16(hdr, 10, 0x0004); wr16(hdr, 12, 0x0200); wr16(hdr, 14, 0);
  "AC791N".split("").forEach((c, k) => { hdr[16 + k] = c.charCodeAt(0); });
  ents.forEach((x, i) => { const y = x.e.slice(); enc(y, 0, 0x50); lst.set(y, i * 0x50); });
  wr16(hdr, 2, crc16(lst));
  wr16(hdr, 0, crc16(hdr.subarray(2, 0x40)));
  enc(hdr, 0, 0x40);
  const logical = new Uint8Array(total).fill(0xFF);
  logical.set(hdr, 0);
  logical.set(lst, 0x40);
  for (const x of ents) logical.set(x.data, x.off);
  if (product.length > 20) throw new Error("the package's identity is too long");
  const raw = new Uint8Array(total + UFW_BLOCKS);
  for (let i = 0; i < UFW_BLOCKS; i++) {
    raw.set(logical.subarray(i * UFW_KEEP, (i + 1) * UFW_KEEP), i * UFW_BLK);
    raw[i * UFW_BLK + UFW_KEEP] = i < product.length ? (product.charCodeAt(i) + i + 1) & 0xFF : 0x7D;
  }
  raw.set(logical.subarray(UFW_BLOCKS * UFW_KEEP), UFW_BLOCKS * UFW_BLK);
  return raw;
}

function appArea(flash) {                           // the app area deciphered: { area, blk, slot }
  const head = flash.slice(0x4000, 0x4020);
  sfc(head, 0, 32, KEY);
  const body = head.subarray(2, 32);
  const name = String.fromCharCode(...body.subarray(14, 27));
  if (crc16(body) !== rd16(head, 0) || name !== "app_area_head") throw new Error("the package's app area does not decipher: not an FM-GB package?");
  const blk = rd32(body, 6), area = flash.slice(0x4000, 0x4000 + blk);
  sfc(area, 0, blk, KEY);
  return { area, blk, slot: appSlot(flash.length) };
}

function trailerOf(app) {
  const t = app.subarray(app.length - TRAILER);
  if (rd32(t, 0) !== GBR_MAGIC || crc32(t.subarray(0, 60)) !== rd32(t, 60)) return null;
  let name = "";
  for (let i = 28; i < 60 && t[i]; i++) name += String.fromCharCode(t[i]);
  return { back: rd32(t, 8), imageLen: rd32(t, 12), romSize: rd32(t, 16), imageCrc: rd32(t, 20), flags: rd32(t, 24), name };
}

function codeEnd(app) {
  const tr = trailerOf(app);
  let n = tr ? app.length - TRAILER - tr.back : app.length;
  while (n > 0 && app[n - 1] === 0xFF) n--;
  return n;
}

export function pack(base, romFile, name, opt = {}) {
  const info = romInfo(romFile);
  if (!info.supported) throw new Error(info.why);
  const rom = padBanks(romFile);
  const { product, files } = ufwParse(base);
  const fl = files.find((f) => f.typ === 0);
  if (!fl) throw new Error("the base package has no flash.bin");
  const flash = fl.data.slice(), { area, blk, slot } = appArea(flash);
  const app = area.subarray(APP_HEAD, APP_HEAD + slot), end = codeEnd(app);
  if (end < 0x1000) throw new Error("the base package's app is empty?");
  const room = slot - ((end + 0xFFF) & ~0xFFF);
  const tail = slotTail(rom, room, name, !!opt.compress, opt.onProgress);
  app.fill(0xFF, end);
  app.set(tail, slot - tail.length);
  // app.bin's entry and the area's head: their checksums again
  const e = area.subarray(32, 64);
  wr16(e, 2, crc16(app));
  wr16(e, 0, crc16(e.subarray(2, 32)));
  const h = area.subarray(0, 32);
  wr16(h, 2, crc16(area.subarray(32, blk)));
  wr16(h, 0, crc16(h.subarray(2, 32)));
  sfc(area, 0, blk, KEY);
  flash.set(area, 0x4000);
  const out = files.map((f) => (f.typ === 0 ? { ...f, data: flash } : f));
  return { pkg: ufwBuild(out, product), product, codeBytes: end, romBytes: rom.length, tailBytes: tail.length, room,
           compressed: rd32(tail, tail.length - TRAILER + 24) & GBR_F_TABLE ? true : false };
}

export function gameOf(pkgBytes) {
  const { files } = ufwParse(pkgBytes);
  const fl = files.find((f) => f.typ === 0);
  const { area, slot } = appArea(fl.data);
  const app = area.subarray(APP_HEAD, APP_HEAD + slot), tr = trailerOf(app);
  if (!tr) return null;
  const img = app.subarray(app.length - TRAILER - tr.back, app.length - TRAILER - tr.back + tr.imageLen);
  return { name: tr.name, romSize: tr.romSize, compressed: !!(tr.flags & GBR_F_TABLE), ok: crc32(img) === tr.imageCrc };
}
