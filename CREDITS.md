# Credits

- **The fm1-gb authors**: FM-GB (`gb/`), the Game Boy emulator, its FM-1 app, the ROM packers and the install page.
- **Leo Kuroshita** ([@kurogedelic](https://github.com/kurogedelic), [Hügelton Instruments](https://hugelton.com)):
  Felucca, the firmware this base grew from, and felucca-bsp, which it is built on. Much of fm1-base is his code,
  used under MPL-2.0 with his permission ([hugelton/felucca-bsp#4](https://github.com/hugelton/felucca-bsp/issues/4)).
- **Charles Vestal** ([@charlesvestal](https://github.com/charlesvestal)): fm1-base, and the platform layer of the
  FM-1 firmwares it comes from (X0X, FoMni-1, CTL-1 and the others).
- **keremimo** ([@keremimo](https://github.com/keremimo)): the MIDI clock and transport sync in felucca-bsp's
  `usb.c` ([hugelton/felucca-bsp#3](https://github.com/hugelton/felucca-bsp/pull/3)); from
  [Melodee](https://github.com/keremimo/melodee) (Kerem Kilic, Ellic Studio), the boot's LZMA-packed load images
  (`device/boot_lzma.h`, `tools/boot_pack.py`), and the designs fm1-base's resource region (`device/res.c`)
  and pooled store (`device/storage.c` APP_STORE_POOL) follow, with
  permission.
- **Henri David** (hdavid): felucca-bsp's radio drivers, from [Optimist](https://github.com/w0ts/optimist)
  (offered under GPL-3.0-only or MPL-2.0, [w0ts/optimist#55](https://github.com/w0ts/optimist/issues/55), used under
  MPL-2.0), and fm1-base's companion services and the SD card's streams.
- **Julius Vitkauskas** ([m-bootloader](https://github.com/jvitkauskas/m-bootloader)): fm1-base's SD card driver, from
  m-bootloader's SD stage (MIT).
- JieLi's AC79 SDK (Zhuhai Jieli Technology, Apache-2.0): the radio's calibration that felucca-bsp's
  `chip/fm1_rf_cal.h` rewrites in C, and the radio's start-up tables (`chip/ble_rf_tables_v15.h`, as M-VAVE's stock
  firmware runs them).
- **Chris Farrell** ([@seajaysec](https://github.com/seajaysec)): from [fm-static](https://github.com/seajaysec/fm-static),
  the web installer's finding of an FM-1 under any port name, M-VAVE's Open firmware's identities, the package
  checksum check and the return check after resuming an update, used under MPL-2.0 with his permission
  ([seajaysec/fm-static#1](https://github.com/seajaysec/fm-static/issues/1#issuecomment-6410795859)).
- Fonts: Barlow by The Barlow Project Authors and Terminus by Dimitar Toshkov Zhekov (both SIL OFL 1.1).
