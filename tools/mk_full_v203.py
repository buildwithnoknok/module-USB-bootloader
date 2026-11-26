#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) noknok
"""
mk_full_v203.py - one 32 KB flash image for a CH32V203 USB module, flash layout 4
(DEV-237): stage-0 @0x0000 + stage-1 @0x0400 + app @0x2000 + app metadata @0x7F00.

This is how a module gets the frozen stage-0 at all: a blank board, or a module
still on the single-stage bootloader 1.x (8 KB at 0x0000), is flashed ONCE with
this image (BOOT0 jumper + WCHISPTool at 0x08000000, or SWD). After that the
stage-1 and the app are updated over USB.

The image covers the whole 32 KB, so nothing stale survives. The control block
page (0x7E00) is left blank: no stage-1 update pending. The metadata is what
the stage-1 VERIFY would write: {magic 0x6E6B5542 "nkUB", app_len, crc32(app)}.

Usage:
  python mk_full_v203.py <stage0.bin> <stage1.bin> <app.bin> <out.bin>
  python mk_full_v203.py --check <out.bin>     # re-validate a built image
"""
import struct
import sys
import zlib

FLASH       = 0x8000
STAGE1_BASE = 0x0400
APP_BASE    = 0x2000
CTRL_OFF    = 0x7E00
META_OFF    = 0x7F00
META_MAGIC  = 0x6E6B5542
S1_HDR      = 0x100          # stage-1 header offset: NKS1, base, layout, version, target
NKS1, V203, LAYOUT = 0x31534B4E, 0x33303256, 4


def fail(msg):
    sys.exit("mk_full_v203: " + msg)


def check(img):
    """Return a list of problems with a full layout-4 image (empty = good)."""
    p = []
    if len(img) != FLASH:
        return ["size %d, want %d" % (len(img), FLASH)]
    if img[0:4] == b"\xff" * 4:
        p.append("no stage-0 at 0x0000")
    magic, base, layout, ver, target = struct.unpack_from("<5I", img, STAGE1_BASE + S1_HDR)
    if (magic, base, layout, target) != (NKS1, 0x400, LAYOUT, V203):
        p.append("stage-1 header at 0x0500 is not NKS1/0x400/layout 4/V203")
    if img[CTRL_OFF:CTRL_OFF + 256] != b"\xff" * 256:
        p.append("control block 0x7E00 not blank (a stage-1 update would be pending)")
    m, ln, crc = struct.unpack_from("<III", img, META_OFF)
    if m != META_MAGIC:
        p.append("no app metadata magic at 0x7F00")
    elif not (0 < ln <= CTRL_OFF - APP_BASE) or zlib.crc32(img[APP_BASE:APP_BASE + ln]) & 0xFFFFFFFF != crc:
        p.append("app metadata does not match the app (len %d)" % ln)
    return p


def main(argv):
    if argv[:1] == ["--check"]:
        img = open(argv[1], "rb").read()
        probs = check(img)
        for x in probs:
            print("FAIL:", x)
        if not probs:
            print("OK: %s is a valid layout-4 full image" % argv[1])
        return 1 if probs else 0
    if len(argv) != 4:
        fail("usage: mk_full_v203.py <stage0.bin> <stage1.bin> <app.bin> <out.bin>")
    s0, s1, app = (open(f, "rb").read() for f in argv[:3])
    if len(s0) > STAGE1_BASE:
        fail("stage-0 is %d B, more than 1 KB" % len(s0))
    if len(s1) > APP_BASE - STAGE1_BASE:
        fail("stage-1 is %d B, more than 7 KB" % len(s1))
    if len(app) > CTRL_OFF - APP_BASE:
        fail("app is %d B, more than the 23.5 KB app region" % len(app))
    img = bytearray(b"\xff" * FLASH)
    img[0:len(s0)] = s0
    img[STAGE1_BASE:STAGE1_BASE + len(s1)] = s1
    img[APP_BASE:APP_BASE + len(app)] = app
    crc = zlib.crc32(app) & 0xFFFFFFFF
    img[META_OFF:META_OFF + 12] = struct.pack("<III", META_MAGIC, len(app), crc)
    probs = check(bytes(img))
    if probs:
        fail("; ".join(probs))
    open(argv[3], "wb").write(img)
    print("v203 layout 4: stage-0 %d B, stage-1 %d B @0x0400, app %d B @0x2000 crc 0x%08X -> %s"
          % (len(s0), len(s1), len(app), crc, argv[3]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
