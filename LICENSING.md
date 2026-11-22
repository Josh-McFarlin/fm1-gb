# Licensing

**This repository is MPL-2.0** (full text in `LICENSE`), file by file: every source file carries an
`SPDX-License-Identifier: MPL-2.0` header. If you change one of these files and distribute it, publish your change to
that file under MPL-2.0. It holds FM-GB (`gb/`) and the part of fm1-base it is built on (`device/`, `include/`,
`host/`, `tools/`, `web/`, `assets/`), with felucca-bsp as a submodule (`bsp/`). FM-GB has no radio: nothing of JieLi's
Bluetooth or WiFi libraries is built in.

## Where the code comes from

| Code | Origin | Why it is MPL-2.0 |
| --- | --- | --- |
| `gb/` (FM-GB: the emulator, the app, its tools, page and tests) | written for this repository by the fm1-gb authors | their own |
| Leo Kuroshita's code (from Felucca, through the firmwares below) | [Felucca](https://github.com/hugelton/Felucca), GPL-3.0-only there | his permission to use the Felucca code he owns under MPL-2.0: [hugelton/felucca-bsp#4](https://github.com/hugelton/felucca-bsp/issues/4) |
| Charles Vestal's code | the platform layer of [fm1-x0x](https://github.com/charlesvestal/fm1-x0x), [fm1-fomni](https://github.com/charlesvestal/fm1-fomni) and the other FM-1 firmwares, GPL-3.0-only there | his own |
| Chris Farrell's code in `web/fm1ota.js` and `web/fm1pkg.js` (each file's header says which) | [fm-static](https://github.com/seajaysec/fm-static), GPL-3.0-only there | his permission to use it under MPL-2.0: [seajaysec/fm-static#1](https://github.com/seajaysec/fm-static/issues/1#issuecomment-6410795859) |
| `device/boot_lzma.h`, `tools/boot_pack.py` and `boot_image` in `device/main_fm1.c` (the LZMA-packed load images, `boot_pack = true`). Copyright (C) 2026 Kerem Kilic (Ellic Studio) and Charles Vestal | [Melodee](https://github.com/keremimo/melodee) (`firmware/src/boot_lzma.h`, `tools/boot_audio.py`, `firmware/src/main.c`), GPL-3.0-only there | its author's permission to use Melodee's code in fm1-base under MPL-2.0 (stated by fm1-base's maintainer, 2026-10-31) |
| `device/res.c` (the resource region, `resources = FILE`; not built into FM-GB) and the resource functions in `web/fm1ota.js`; the pooled store in `device/storage.c` (APP_STORE_POOL). Copyright (C) 2026 Charles Vestal | written for fm1-base after [Melodee](https://github.com/keremimo/melodee)'s resource region (`firmware/src/res.c`, `tools/fm1_resources.py`, `web/fm1res.js`) and shared storage pool, by Kerem Kilic (Ellic Studio), GPL-3.0-only there | MPL-2.0, with its author's permission (as above) |
| The SD card (`device/sd.c`; felucca-bsp's `fm1_sd_hw.h`), from m-bootloader's SD card stage (MIT there); not built into FM-GB | Julius Vitkauskas | the author's own |
| The SD card's streams for an app and its read ahead (`device/sd_ahead.h`, the streams in `device/sd.c`) | Henri David | the author's own |
| `bsp/` | [felucca-bsp](https://github.com/hugelton/felucca-bsp), a git submodule | MPL-2.0 there |
| `device/bt_own.c`, `device/bt_own_central.c`, `device/bt_own_audio.c` (the glue to fm1-base's own Bluetooth stack, which is not in this repository; not built into FM-GB) | Charles Vestal | his own |
| felucca-bsp's radio drivers (`bsp/chip/fm1_ble.h`, `fm1_ble_rf.h`, `fm1_bredr.h`) | Optimist, by Henri David | MPL-2.0 there, from the same offer (`bsp/NOTICE`) |
| The companion services (`device/companion.c`; not built into FM-GB). Copyright (C) 2026 Henri David (hdavid); the store's objects as files Copyright (C) 2026 Charles Vestal | written for fm1-base ([charlesvestal/fm1-base#26](https://github.com/charlesvestal/fm1-base/pull/26)) | MPL-2.0 as contributed |

The screen reader's firmware side, `device/announce.c` and `tools/gen_words.py` (Charles Vestal's, from
[fm1-reader](https://github.com/charlesvestal/fm1-reader), where they were MIT), is MPL-2.0 like the rest, and
fm1-base holds their one copy. The older firmwares that are not on fm1-base (X0X, FoMni-1, MONK-1 and Mocky, which are
GPL) carry copies made before, which stay under their repositories' licences until fm1-reader's
`tools/sync_announce.sh` copies them again. MPL-2.0 is compatible with the GPL (its section 3.3), so the new copies
can go into those firmwares too.

## JieLi's code in felucca-bsp

felucca-bsp (`bsp/`) keeps two parts under their own licence, with its `NOTICE`:

| What | Licence | Where |
| --- | --- | --- |
| The radio's start-up tables: JieLi's `rf_init` as M-VAVE's stock V15 firmware runs it (Copyright (c) 2024-2026 Zhuhai Jieli Technology Co., Ltd., AC79 AIoT SDK; Copyright (c) 2026 Zhuhai Shengke / M-VAVE, `fm1_open`), packed by Optimist; the header states the changes | Apache-2.0 (`LICENSES/Apache-2.0.txt`) | `bsp/chip/ble_rf_tables_v15.h` |
| The radio's calibration at every start: JieLi's SDK calibration (`wl_rf_common.a`, Copyright (c) 2024-2026 Zhuhai Jieli Technology Co., Ltd.) rewritten in C by Charles Vestal; the header states the changes | Apache-2.0 | `bsp/chip/fm1_rf_cal.h` |

FM-GB has no radio, so neither is built into it.

The GPL copies in Felucca and in those firmwares stay under the GPL; the copies here are MPL-2.0. No code by other
Felucca contributors is in fm1-base: the history of every file was checked when it was imported (from fm1-fomni
`7e7547f`).

## Not part of fm1-base or FM-GB

| What | Licence | Where |
| --- | --- | --- |
| Barlow Semi Condensed (The Barlow Project Authors) | SIL OFL 1.1 | `assets/fonts/BarlowSemiCondensed-*.ttf`, `assets/fonts/Barlow-OFL.txt` |
| Terminus (Dimitar Toshkov Zhekov) | SIL OFL 1.1 | `assets/fonts/ter-u*.bdf`, `assets/fonts/Terminus-LICENSE.txt` |
| JieLi AC79 SDK files a package needs (`uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin`) | Apache-2.0 | read from your SDK checkout at build time; never in this repository. Every FM-GB package carries them: ship `LICENSES/Apache-2.0.txt` with one |
| Game Boy ROMs | their owners' | never in this repository, its packages or its pages: the player adds their own when installing |
