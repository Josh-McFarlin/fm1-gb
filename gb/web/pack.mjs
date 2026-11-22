// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 The fm1-gb authors
// The browser's packer from the command line (Node 18+), for tests and for anyone without Python:
//   node gb/web/pack.mjs BASE.fwsc ROM.gb OUT.fwsc [--compress]
import { readFileSync, writeFileSync } from "node:fs";
import { basename } from "node:path";
import { pack, romInfo, gameOf } from "./fm1gb.js";

const [base, rom, out, ...rest] = process.argv.slice(2);
if (!base || !rom || !out) {
  console.error("usage: node gb/web/pack.mjs BASE.fwsc ROM.gb OUT.fwsc [--compress]");
  process.exit(2);
}
const romBytes = new Uint8Array(readFileSync(rom));
const info = romInfo(romBytes);
if (!info.supported) { console.error(`${rom}: ${info.why}`); process.exit(1); }
const r = pack(new Uint8Array(readFileSync(base)), romBytes, basename(rom), { compress: rest.includes("--compress") });
writeFileSync(out, r.pkg);
const g = gameOf(r.pkg);
console.log(`${info.title || basename(rom)} (${info.mapper}, ${info.romKiB} KiB) -> ${out}: ${r.pkg.length} B, ` +
            `${g.compressed ? "banks compressed, " : ""}${g.ok ? "checks" : "DAMAGED"}`);
