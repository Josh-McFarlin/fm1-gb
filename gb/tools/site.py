#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 The fm1-gb authors
"""FM-GB's install page as a folder to serve (GitHub Pages, or `python3 -m http.server -d gb/build/site` and
http://localhost:8000: Web MIDI wants https or localhost).

  gb/tools/site.py [--base FM-GB.fwsc] [-o gb/build/site]

The page (gb/web/index.html), its packer (gb/web/fm1gb.js), fm1-base's updater modules (web/fm1ota.js, fm1ports.js,
fm1pkg.js) and the emulator's package with no game in it (gb/build/fm-gb.fwsc, from tools/build.py gb)."""
import argparse
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=str(ROOT / "gb" / "build" / "fm-gb.fwsc"))
    ap.add_argument("-o", "--out", default=str(ROOT / "gb" / "build" / "site"))
    a = ap.parse_args()
    out = Path(a.out)
    base = Path(a.base)
    if not base.exists():
        raise SystemExit(f"{base}: no package; build it first (tools/build.py gb)")
    out.mkdir(parents=True, exist_ok=True)
    for f in (ROOT / "gb" / "web" / "index.html", ROOT / "gb" / "web" / "fm1gb.js", ROOT / "web" / "fm1ota.js",
              ROOT / "web" / "fm1ports.js", ROOT / "web" / "fm1pkg.js"):
        shutil.copyfile(f, out / f.name)
    shutil.copyfile(base, out / "fm-gb.fwsc")
    words = ROOT / "gb" / "web" / "words"           # the screen reader's words (tools/build.py writes them)
    if words.is_dir():
        (out / "words").mkdir(exist_ok=True)
        for f in words.glob("*.json"):
            shutil.copyfile(f, out / "words" / f.name)
    (out / ".nojekyll").write_text("")
    print(f"site     {out}  (serve it: python3 -m http.server -d {out} 8000, then http://localhost:8000)")


if __name__ == "__main__":
    main()
