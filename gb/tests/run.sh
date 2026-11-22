#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# FM-GB's tests: its test cartridge in the core's desktop harness, then the app in fm1-base's host simulator, then the
# ROM packers (Python against fm1-base's packager, the browser's against Python's). The packers need the emulator's
# package (tools/build.py gb, with the JieLi toolchain); without it they are left out.
#   gb/tests/run.sh            (from anywhere; PYTHON=.venv/bin/python for the pinned one)
set -e
cd "$(dirname "$0")/../.."
PY="${PYTHON:-python3}"
[ -d .toolchain/node/bin ] && PATH="$PWD/.toolchain/node/bin:$PATH"
mkdir -p gb/build/test
"$PY" gb/tools/mkdemo.py gb/build/test/demo.gb
${CC:-cc} -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -Igb/core -o gb/build/test/gbtest gb/test/gbtest.c gb/core/gb.c
gb/build/test/gbtest gb/build/test/demo.gb -f 90 -b 10:01 -b 40:00 -b 50:80 -b 52:00 >/dev/null
"$PY" tools/build.py gb --host >/dev/null
for t in gb/tests/*.txt; do
    echo "== $t"
    FM1GB_ROM=gb/build/test/demo.gb gb/build/host/fm-gb_host "$t" gb/build/test
done
if [ -f gb/build/fm-gb.fwsc ] && [ -f gb/build/fm-gb.bin ]; then
    echo "== the ROM packers"
    "$PY" gb/tools/test_fm1gb.py
else
    echo "(the ROM packers not tested: no gb/build/fm-gb.fwsc; tools/build.py gb makes it)"
fi
